// voicechat - in-game voice chat for patched game clients.
//
// Hosts the voice server of the LANCommander Interposer's voice API next to
// the game server and mirrors the game into it, so the clients (the Battlefield
// 1942 patch's voice chat) find it and talk to whom the game says:
//
//  - Every player is registered under voice game id = RMM player id + 1 (the
//    voice API keeps 0 for none; on BF1942 the RMM id is the engine id). Clients
//    log in by claiming their game player (INTERPOSER_COMMS_AUTH_CLAIM), so
//    voice always matches who is on the game server. Bots only with
//    voicechat.includeBots 1 (for testing).
//  - Teams 1 and 2 are the server groups "Axis" and "Allies" (BF1942; "Team 1"
//    and "Team 2" elsewhere): each player sees only their own team's, which is
//    their team radio.
//  - The living players' positions and facing drive proximity chat (and its
//    stereo panning); the dead drop out of proximity but keep the radio.
//  - The voice port goes into the server's query replies as \commsport\<port>
//    (RMM_CAP_QUERY_RULES), so clients don't need it configured.
//
// The voice server is the LANCommander Interposer's, embedded: this plugin links
// its static library built with the VOICE feature only (RMM_INTERPOSER_DIR,
// RMM_INTERPOSER_FEATURES), so there is no Interposer DLL to deploy. The
// plugin doesn't attach the Interposer: no hooks, config, session log or
// Interposer plugins, only the voice API.
//
// Commands:
//   voice                      status (anyone)
//   voiceadmin restart         restart the voice server (applies port etc.)   admin flag h
//   voiceadmin <command...>    the voice server's own admin commands
//                              (status, players, kick <player>, mute <player>, limit ...)
#include <rmm/rmm.hpp>

#include <LANCommander.Interposer.Comms.Server.h>

#include <cstdlib>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace {

// ---- the library's log, from any of its threads -----------------------------------

std::mutex g_logMu;
std::deque<std::pair<int, std::string>> g_logLines;

void INTERPOSER_CALL OnVoiceLog(interposer_comms_log_level level, const char* message, void*) {
    int rmmLevel = level >= INTERPOSER_COMMS_LOG_ERROR  ? RMM_LOG_ERROR
                   : level >= INTERPOSER_COMMS_LOG_WARN ? RMM_LOG_WARN
                   : level >= INTERPOSER_COMMS_LOG_INFO ? RMM_LOG_INFO
                                                        : RMM_LOG_DEBUG;
    std::lock_guard<std::mutex> lk(g_logMu);

    if (g_logLines.size() < 256)
        g_logLines.emplace_back(rmmLevel, message ? message : "");
}

void FlushVoiceLog() {
    std::deque<std::pair<int, std::string>> lines;
    {
        std::lock_guard<std::mutex> lk(g_logMu);
        lines.swap(g_logLines);
    }

    for (auto& l : lines)
        rmm::Log(l.first, "[voice] " + l.second);
}

}  // namespace

class VoiceChat : public rmm::Plugin {
public:
    bool OnLoad(bool, std::string& error) override {
        const uint32_t num = RMM_VAR_NUMERIC;
        enabled_ = rmm::Var("voicechat.enabled", "1", "Host the voice server (1) or not (0)", num);
        port_ =
            rmm::Var("voicechat.port", "27800", "UDP port of the voice server (voiceadmin restart applies it)", num);
        port_.Bounds(1, 65535);
        bind_ = rmm::Var("voicechat.bindAddress", "", "Address the voice server listens on (empty = all)");
        password_ = rmm::Var("voicechat.password", "",
                             "Password voice clients must also give (empty = none; the clients' voiceChat.password)",
                             RMM_VAR_PROTECTED);
        adminKey_ =
            rmm::Var("voicechat.adminKey", "",
                     "Key for the voice server's remote admin channel (empty = no remote admin)", RMM_VAR_PROTECTED);
        maxPlayers_ = rmm::Var("voicechat.maxPlayers", "256", "Most voice clients at once", num);
        maxPlayers_.Bounds(1, 1024);
        maxBitrate_ = rmm::Var("voicechat.maxBitrate", "32000", "Highest Opus bitrate clients may send, bits/s", num);
        maxBitrate_.Bounds(6000, 128000);
        range_ = rmm::Var("voicechat.proximityRange", "50", "Metres beyond which nearby voice can't be heard", num);
        range_.Bounds(1, 10000);
        fullRange_ = rmm::Var("voicechat.proximityFullVolumeRange", "4",
                              "Metres within which nearby voice is at full volume", num);
        fullRange_.Bounds(0, 10000);
        bots_ = rmm::Var("voicechat.includeBots", "0", "Register bots as voice players too (testing)", num);
        interval_ = rmm::Var("voicechat.updateInterval", "0.1", "Seconds between two position/team updates", num);
        interval_.Bounds(0.05, 1.0);

        enabled_.OnChange([this](const std::string&, const std::string& v) {
            if (std::atoi(v.c_str()))
                failed_ = false;  // try again on the next tick
            else
                Stop("voicechat.enabled 0");
        });

        for (rmm::Var* v : {&range_, &fullRange_})
            v->OnChange([this](const std::string&, const std::string&) { ConfigureZone(); });

        rmm::Command("voice", RMM_CMD_ALL, 0, "Voice chat status", "voice", [this](rmm::CmdArgs& a) { Status(a); });
        rmm::Command("voiceadmin", RMM_CMD_ALL, RMM_ADMFLAG_CONFIG, "Voice server admin: restart, or its own commands",
                     "voiceadmin restart | status | players | kick <player> [reason] | mute <player> | unmute <player> "
                     "| limit <name> <value> | help",
                     [this](rmm::CmdArgs& a) { Admin(a); });
        rmm::On(RMM_EV_SERVER_SHUTDOWN, [this] {
            Stop("the server is shutting down");

            return RMM_CONTINUE;
        });
        // Start on the first tick, after rmm/configs/plugins/voicechat.cfg has run.
        rmm::Every(0.05, [this] {
            Tick();

            return true;
        });

        (void)error;

        return true;
    }

    void OnUnload() override {
        Stop("unloading");

        // The handler is this image's code, which is about to be unmapped.
        if (apiChecked_)
            interposer_comms_set_log_handler(nullptr, INTERPOSER_COMMS_LOG_OFF, nullptr);

        FlushVoiceLog();
    }

private:
    struct Tracked {
        interposer_comms_player_id voice = INTERPOSER_COMMS_INVALID_ID;
        std::string name;
        int32_t team = -1;
        bool positioned = false;
    };

    // ---- the voice API --------------------------------------------------------------

    bool CheckApi() {
        if (apiChecked_)
            return true;

        if (interposer_comms_get_api_version() != INTERPOSER_COMMS_API_VERSION) {
            rmm::Error("The embedded Interposer has voice API version %u, this plugin was written for %u; no voice "
                       "chat",
                       interposer_comms_get_api_version(), INTERPOSER_COMMS_API_VERSION);

            return false;
        }

        interposer_comms_set_log_handler(&OnVoiceLog, INTERPOSER_COMMS_LOG_INFO, nullptr);
        apiChecked_ = true;
        rmm::Info("Voice server: embedded LANCommander Interposer %s", interposer_comms_get_version_string());

        return true;
    }

    const char* ResultText(interposer_comms_result r) const { return interposer_comms_result_to_string(r); }

    // ---- start / stop ----------------------------------------------------------------

    bool Start() {
        if (!CheckApi())
            return false;

        std::string password = password_.Str(), adminKey = adminKey_.Str(), bind = bind_.Str();
        interposer_comms_server_config config;
        interposer_comms_server_config_init(&config);
        config.bind_address = bind.empty() ? nullptr : bind.c_str();
        config.port = static_cast<uint16_t>(port_.Int());
        config.name = rmm::Api().game->name();
        config.auth_mode = INTERPOSER_COMMS_AUTH_CLAIM;
        config.password = password.empty() ? nullptr : password.c_str();
        config.admin_key = adminKey.empty() ? nullptr : adminKey.c_str();
        config.max_players = static_cast<uint32_t>(maxPlayers_.Int());
        config.max_private_groups_per_player = 1;
        config.opus_max_bitrate = static_cast<uint32_t>(maxBitrate_.Int());
        config.position_authority = INTERPOSER_COMMS_POSITION_SERVER;
        interposer_comms_server* server = nullptr;
        interposer_comms_result r = interposer_comms_server_create(&config, &server);

        if (r == INTERPOSER_COMMS_OK)
            r = interposer_comms_server_start(server);

        if (r != INTERPOSER_COMMS_OK) {
            rmm::Error("The voice server can't start on port %u: %s", config.port, ResultText(r));

            if (server)
                interposer_comms_server_destroy(server);

            return false;
        }

        server_ = server;
        ConfigureZone();
        bool bf1942 = rmm::Api().game->id() == RMM_GAME_BF1942;
        const char* names[3] = {nullptr, bf1942 ? "Axis" : "Team 1", bf1942 ? "Allies" : "Team 2"};

        for (int t = 1; t <= 2; ++t) {
            interposer_comms_group_config team;
            interposer_comms_group_config_init(&team);
            team.type = INTERPOSER_COMMS_GROUP_SERVER;
            team.name = names[t];
            teams_[t] = INTERPOSER_COMMS_INVALID_ID;
            interposer_comms_server_create_group(server_, &team, &teams_[t]);
        }

        port_now_ = config.port;

        if (!rmm::SetQueryRule("commsport", std::to_string(port_now_)))
            rmm::Warn("The query replies can't carry the voice port (rmm caps: query_rules); clients need "
                      "voiceChat.port %u",
                      port_now_);

        rmm::Info("Voice server listening on port %u (bots %s)", port_now_, bots_.Bool() ? "included" : "left out");

        return true;
    }

    void Stop(const char* why) {
        if (!server_)
            return;

        rmm::RemoveQueryRule("commsport");
        interposer_comms_server_stop(server_);
        interposer_comms_server_destroy(server_);
        server_ = nullptr;
        players_.clear();
        port_now_ = 0;
        FlushVoiceLog();
        rmm::Info("Voice server stopped (%s)", why);
    }

    void ConfigureZone() {
        if (!server_)
            return;

        // Refractor is Y up and left-handed, in metres.
        interposer_comms_zone_config zone;
        interposer_comms_zone_config_init(&zone);
        zone.max_distance = static_cast<float>(range_.Float());
        zone.min_distance = static_cast<float>(fullRange_.Float());
        zone.up = interposer_comms_vec3{0.0f, 1.0f, 0.0f};
        zone.left_handed = 1;

        if (interposer_comms_server_configure_zone(server_, 0, &zone) != INTERPOSER_COMMS_OK)
            rmm::Warn("voicechat.proximityRange %.1f / proximityFullVolumeRange %.1f are not valid; the voice server "
                      "keeps its previous ones",
                      zone.max_distance, zone.min_distance);
    }

    // ---- the game, mirrored ---------------------------------------------------------

    void Tick() {
        FlushVoiceLog();

        if (!enabled_.Bool() || failed_)
            return;

        if (!server_ && !Start()) {
            failed_ = true;  // until voicechat.enabled is set again or voiceadmin restart

            return;
        }

        double now = rmm::Api().game->time();

        if (now >= nextSync_) {
            nextSync_ = now + interval_.Float();
            Sync();
        }

        DrainEvents();
    }

    void Forget(rmm_player_t id) {
        auto it = players_.find(id);

        if (it == players_.end())
            return;

        interposer_comms_server_remove_player(server_, it->second.voice);
        players_.erase(it);
    }

    void Sync() {
        bool withBots = bots_.Bool(), positions = rmm::HasCap(RMM_CAP_PLAYER_POSITION);
        std::map<rmm_player_t, bool> seen;

        for (rmm_player_t id : rmm::Players()) {
            auto info = rmm::GetPlayer(id);

            if (!info || id < 0 || !info->name[0] || (info->is_bot && !withBots))
                continue;

            seen[id] = true;
            auto it = players_.find(id);

            if (it != players_.end() && it->second.name != info->name) {
                Forget(id);  // the slot went to someone else (or a rename): start over
                it = players_.end();
            }

            if (it == players_.end()) {
                Tracked t;
                interposer_comms_result r =
                    interposer_comms_server_add_player(server_, static_cast<uint64_t>(id) + 1, info->name, &t.voice);

                if (r != INTERPOSER_COMMS_OK) {
                    rmm::Warn("Can't add #%d %s to voice: %s", id, info->name, ResultText(r));
                    continue;
                }

                t.name = info->name;
                it = players_.emplace(id, t).first;
                rmm::Debug("#%d %s added to voice (team %d%s)", id, info->name, info->team,
                           info->is_bot ? ", a bot" : "");
            }

            Tracked& t = it->second;

            if (t.team != info->team) {
                if (t.team == 1 || t.team == 2)
                    interposer_comms_server_remove_from_group(server_, teams_[t.team], t.voice);

                if (info->team == 1 || info->team == 2)
                    interposer_comms_server_add_to_group(server_, teams_[info->team], t.voice);

                t.team = info->team;
            }

            bool positioned = positions && info->is_alive;

            if (positioned) {
                const rmm_vec3& p = info->position;
                const rmm_vec3& f = info->forward;
                interposer_comms_vec3 at{p.x, p.y, p.z}, forward{f.x, f.y, f.z};
                bool facing = f.x != 0.0f || f.y != 0.0f || f.z != 0.0f;
                // The facing is what the voice server pans by: without it everyone is heard dead centre.
                interposer_comms_server_update_player_position(server_, t.voice, 0, at, facing ? &forward : nullptr);
            } else if (t.positioned) {
                interposer_comms_server_clear_player_position(server_, t.voice);
            }

            t.positioned = positioned;
        }

        std::vector<rmm_player_t> gone;

        for (auto& kv : players_)
            if (!seen.count(kv.first))
                gone.push_back(kv.first);

        for (rmm_player_t id : gone) {
            rmm::Debug("#%d %s left voice", id, players_[id].name.c_str());
            Forget(id);
        }
    }

    void DrainEvents() {
        interposer_comms_server_event e;

        while (server_ && interposer_comms_server_poll_event(server_, &e) == INTERPOSER_COMMS_OK) {
            switch (e.type) {
                case INTERPOSER_COMMS_SERVER_EVENT_PLAYER_CONNECTED:
                    rmm::Info("%s connected to voice", VoiceName(e.player).c_str());
                    break;
                case INTERPOSER_COMMS_SERVER_EVENT_PLAYER_DISCONNECTED:
                    rmm::Info("%s disconnected from voice %s", VoiceName(e.player).c_str(), e.text);
                    break;
                case INTERPOSER_COMMS_SERVER_EVENT_AUTH_FAILED:
                    rmm::Info("A voice login was refused: %s", e.text);
                    break;
                case INTERPOSER_COMMS_SERVER_EVENT_ADMIN_COMMAND:
                    rmm::Info("Voice admin command: %s", e.text);
                    break;
                default:
                    break;
            }
        }
    }

    std::string VoiceName(interposer_comms_player_id voice) const {
        for (auto& kv : players_)
            if (kv.second.voice == voice)
                return rmm::Format("#%d %s", kv.first, kv.second.name.c_str());

        return rmm::Format("voice player %u", voice);
    }

    // ---- commands ----------------------------------------------------------------------

    void Status(rmm::CmdArgs& a) {
        if (!server_) {
            a.Reply(!enabled_.Bool() ? "Voice chat is off (voicechat.enabled 0)."
                                     : "The voice server isn't running (see the log; voiceadmin restart).");

            return;
        }

        int connected = 0;

        for (auto& kv : players_) {
            interposer_comms_server_player_info info{};

            if (interposer_comms_server_get_player_info(server_, kv.second.voice, &info) == INTERPOSER_COMMS_OK &&
                info.connected)
                ++connected;
        }

        a.Replyf("Voice chat on port %u: %d of %d player(s) connected.", port_now_, connected,
                 static_cast<int>(players_.size()));
    }

    void Admin(rmm::CmdArgs& a) {
        std::string line = a.All();

        if (line == "restart") {
            Stop("restart");
            failed_ = false;

            if (!enabled_.Bool())
                a.Reply("Voice chat is off (voicechat.enabled 0).");
            else if (Start())
                a.Replyf("Voice server restarted on port %u.", port_now_);
            else {
                failed_ = true;
                a.Reply("The voice server didn't start; see the log.");
            }

            return;
        }

        if (!server_) {
            a.Reply("The voice server isn't running.");

            return;
        }

        std::vector<char> out(4096, '\0');
        interposer_comms_result r = interposer_comms_server_execute_command(
            server_, line.empty() ? "help" : line.c_str(), out.data(), out.size());

        for (const std::string& l : rmm::detail::SplitLines(out.data()))
            a.Reply(l);

        if (r != INTERPOSER_COMMS_OK && r != INTERPOSER_COMMS_ERR_BUFFER_TOO_SMALL)
            a.Replyf("(%s)", ResultText(r));
    }

    rmm::Var enabled_, port_, bind_, password_, adminKey_, maxPlayers_, maxBitrate_, range_, fullRange_, bots_,
        interval_;
    bool apiChecked_ = false;
    interposer_comms_server* server_ = nullptr;
    interposer_comms_group_id teams_[3] = {};
    std::map<rmm_player_t, Tracked> players_;
    uint16_t port_now_ = 0;
    double nextSync_ = 0;
    bool failed_ = false;
};

RMM_DECLARE_PLUGIN(VoiceChat, "Voice Chat", "1.0.0", "RMM Team",
                   "Voice server for patched clients: proximity and team radio, mirrored from the game", "",
                   "voicechat", RMM_GAME_ANY, 0)
