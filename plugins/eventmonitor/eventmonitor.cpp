// eventmonitor - demonstrates the event system end to end:
//  * logs every gameplay event (spawns, kills, vehicles, flags, chat...) to
//    rmm/logs/events_<date>.log
//  * chat filter: PRE-phase chat hook that blocks or censors banned words
//    where the game allows it, and warns/kicks repeat offenders otherwise
//  * publishes a custom event ("chatfilter.violation") other plugins can hook
//  * publishes an interface ("IChatFilter", v1) other plugins can call
#include <rmm/rmm.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <map>
#include <set>
#include <sstream>

namespace {

/// Payload of the custom "chatfilter.violation" event.
struct ViolationEvent {
    rmm_player_t player;
    const char* word;
    int32_t count;  // violations so far this map
};

/// Interface published to other plugins (C ABI: plain function pointers).
struct IChatFilterV1 {
    uint32_t version;
    void(RMM_CALL* add_word)(const char* word);
    int32_t(RMM_CALL* violations)(rmm_player_t player);
};

std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    return s;
}

}  // namespace

class EventMonitor : public rmm::Plugin {
public:
    static EventMonitor* self;

    bool OnLoad(bool, std::string&) override {
        self = this;
        logEnabled_ = rmm::Var("eventmonitor.log", "1", "Write gameplay events to logs/events_*.log",
                               RMM_VAR_ARCHIVE | RMM_VAR_NUMERIC);
        words_ = rmm::Var("chatfilter.words", "noob,camper", "Comma separated banned words", RMM_VAR_ARCHIVE);
        kickAfter_ = rmm::Var("chatfilter.kickAfter", "3", "Kick after N violations per map (0 = never)",
                              RMM_VAR_ARCHIVE | RMM_VAR_NUMERIC);
        words_.OnChange([this](const std::string&, const std::string&) { ParseWords(); });
        ParseWords();

        violationEvent_ = rmm::Api().events->register_custom(rmm::g_self, "chatfilter.violation", RMM_EVF_BLOCKABLE);
        iface_ = IChatFilterV1{1, &AddWordThunk, &ViolationsThunk};
        rmm::Api().core->iface_register(rmm::g_self, "IChatFilter", 1, &iface_);

        HookLogging();
        HookChatFilter();

        rmm::Command("vehicles", RMM_CMD_ALL, 0, "Who is in which vehicle", "", [](rmm::CmdArgs& a) {
            int n = 0;

            for (rmm_player_t p : rmm::Players()) {
                auto info = rmm::GetPlayer(p);

                if (info && info->vehicle) {
                    a.Replyf("%s: %s", info->name, info->vehicle_template);
                    ++n;
                }
            }

            if (!n)
                a.Reply("Nobody is in a vehicle.");
        });

        // Tell the admin what this game can actually do.
        if (!rmm::EventBlockable(RMM_EV_CHAT))
            rmm::Info("chat cannot be blocked on %s; the filter will warn/kick instead", rmm::Api().game->name());

        return true;
    }

    void OnUnload() override {
        if (file_)
            std::fclose(file_);

        file_ = nullptr;
        self = nullptr;
    }

private:
    // Logging
    void Write(const std::string& line) {
        if (!logEnabled_.Bool())
            return;

        if (!file_) {
            char date[16];
            std::time_t t = std::time(nullptr);
            std::strftime(date, sizeof date, "%Y%m%d", std::localtime(&t));
            file_ = std::fopen(rmm::Path(RMM_PATH_LOGS, std::string("events_") + date + ".log").c_str(), "a");

            if (!file_)
                return;
        }

        std::fprintf(file_, "%.2f %s\n", rmm::Api().game->time(), line.c_str());
        std::fflush(file_);
    }

    static std::string N(rmm_player_t p) {
        return p == RMM_PLAYER_NONE ? "<world>" : rmm::PlayerName(p) + "#" + std::to_string(p);
    }

    void HookLogging() {
        rmm::On<rmm_ev_map>(RMM_EV_MAP_LOAD, [this](rmm_ev_map& e) {
            Write(rmm::Format("MAP %s %s %s %d", e.map, e.mode, e.mod, e.size));
            violations_.clear();

            return RMM_CONTINUE;
        });
        rmm::On<rmm_ev_map>(RMM_EV_ROUND_START, [this](rmm_ev_map& e) {
            Write(rmm::Format("ROUND_START %s %s", e.map, e.mode));

            return RMM_CONTINUE;
        });
        rmm::On<rmm_ev_round_end>(RMM_EV_ROUND_END, [this](rmm_ev_round_end& e) {
            Write(rmm::Format("ROUND_END winner=%d", e.winner));

            return RMM_CONTINUE;
        });
        rmm::On<rmm_ev_player>(RMM_EV_PLAYER_AUTH, [this](rmm_ev_player& e) {
            auto i = rmm::GetPlayer(e.player);

            if (!i) {
                Write("JOIN " + N(e.player));

                return RMM_CONTINUE;
            }

            Write("JOIN " + N(e.player) + " auth=" + (i->auth_id[0] ? i->auth_id : "<none>") + " addr=" + i->address);

            return RMM_CONTINUE;
        });
        rmm::On<rmm_ev_player>(RMM_EV_PLAYER_DISCONNECT, [this](rmm_ev_player& e) {
            Write("LEAVE " + N(e.player));

            return RMM_CONTINUE;
        });
        rmm::On<rmm_ev_player_spawn>(RMM_EV_PLAYER_SPAWN, [this](rmm_ev_player_spawn& e) {
            Write(rmm::Format("SPAWN %s kit=%s at %.0f,%.0f,%.0f", N(e.player).c_str(), e.kit, e.position.x,
                              e.position.y, e.position.z));

            return RMM_CONTINUE;
        });
        rmm::On<rmm_ev_player_killed>(RMM_EV_PLAYER_KILLED, [this](rmm_ev_player_killed& e) {
            Write(rmm::Format("KILL %s -> %s weapon=%s%s%s%s", N(e.attacker).c_str(), N(e.victim).c_str(), e.weapon,
                              e.vehicle[0] ? (std::string(" vehicle=") + e.vehicle).c_str() : "",
                              e.teamkill ? " TEAMKILL" : "", e.suicide ? " SUICIDE" : ""));

            return RMM_CONTINUE;
        });
        rmm::On<rmm_ev_player_change>(RMM_EV_PLAYER_TEAM, [this](rmm_ev_player_change& e) {
            Write(rmm::Format("TEAM %s %d -> %d", N(e.player).c_str(), e.old_value, e.new_value));

            return RMM_CONTINUE;
        });
        rmm::On<rmm_ev_vehicle>(RMM_EV_VEHICLE_ENTER, [this](rmm_ev_vehicle& e) {
            Write(rmm::Format("ENTER %s %s seat=%d%s", N(e.player).c_str(), e.vehicle_template, e.seat,
                              e.is_driver ? " (driver)" : ""));

            return RMM_CONTINUE;
        });
        rmm::On<rmm_ev_vehicle>(RMM_EV_VEHICLE_EXIT, [this](rmm_ev_vehicle& e) {
            Write(rmm::Format("EXIT %s %s", N(e.player).c_str(), e.vehicle_template));

            return RMM_CONTINUE;
        });
        rmm::On<rmm_ev_weapon_switch>(RMM_EV_PLAYER_WEAPON_SWITCH, [this](rmm_ev_weapon_switch& e) {
            static const char* reasons[] = {"player", "network", "game", "plugin"};
            Write(rmm::Format("WEAPON %s %d:%s -> %d:%s (%s)", N(e.player).c_str(), e.from_slot, e.from_weapon,
                              e.to_slot, e.to_weapon, e.reason >= 0 && e.reason < 4 ? reasons[e.reason] : "?"));

            return RMM_CONTINUE;
        });
        // POST only: blocked spawns don't show.
        rmm::On<rmm_ev_spawner>(RMM_EV_SPAWNER_SPAWN, [this](rmm_ev_spawner& e) {
            Write(rmm::Format("SPAWNER %s at %.0f,%.0f,%.0f team %d -> 0x%llx", e.object_template, e.position.x,
                              e.position.y, e.position.z, e.team, static_cast<unsigned long long>(e.object)));

            return RMM_CONTINUE;
        });
        rmm::On<rmm_ev_control_point>(RMM_EV_CONTROL_POINT, [this](rmm_ev_control_point& e) {
            std::string who;

            for (int32_t i = 0; i < e.player_count && e.players; ++i)
                who += (who.empty() ? " by " : ", ") + N(e.players[i]);

            Write(rmm::Format("FLAG %s %d -> %d%s", e.name, e.old_team, e.new_team, who.c_str()));

            return RMM_CONTINUE;
        });
        rmm::On<rmm_ev_vehicle>(RMM_EV_VEHICLE_DESTROYED, [this](rmm_ev_vehicle& e) {
            Write(rmm::Format("DESTROYED %s (%s) by %s", e.vehicle_template, e.root_template, N(e.player).c_str()));

            return RMM_CONTINUE;
        });
        rmm::On<rmm_ev_object_destroyed>(RMM_EV_OBJECT_DESTROYED, [this](rmm_ev_object_destroyed& e) {
            Write(
                rmm::Format("OBJECT_DESTROYED %s by %s weapon=%s", e.object_template, N(e.attacker).c_str(), e.weapon));

            return RMM_CONTINUE;
        });
        // Hooking the bot events makes the core compare bot lists once a second.
        rmm::On<rmm_ev_bot>(RMM_EV_BOT_ADDED, [this](rmm_ev_bot& e) {
            Write(rmm::Format("BOT_ADDED #%d %s bot=%d team=%d tag=%d", e.player, e.name, e.bot_id, e.team, e.tag));

            return RMM_CONTINUE;
        });
        rmm::On<rmm_ev_bot>(RMM_EV_BOT_REMOVED, [this](rmm_ev_bot& e) {
            Write(rmm::Format("BOT_REMOVED #%d %s bot=%d team=%d", e.player, e.name, e.bot_id, e.team));

            return RMM_CONTINUE;
        });
        rmm::On<rmm_ev_bot>(RMM_EV_BOT_ADD_FAILED, [this](rmm_ev_bot& e) {
            Write(rmm::Format("BOT_ADD_FAILED %s team=%d tag=%d: %s", e.name, e.team, e.tag, e.reason));

            return RMM_CONTINUE;
        });
        // Observe chat in POST: this is the final (possibly censored) text.
        rmm::On<rmm_ev_chat>(RMM_EV_CHAT, [this](rmm_ev_chat& e) {
            Write(rmm::Format("CHAT[%d] %s: %s", e.channel, N(e.player).c_str(), e.text));

            return RMM_CONTINUE;
        });
    }

    // Chat filter
    void ParseWords() {
        banned_.clear();
        std::stringstream ss(words_.Str());
        std::string w;

        while (std::getline(ss, w, ',')) {
            w = Lower(w);
            w.erase(std::remove_if(w.begin(), w.end(), [](unsigned char c) { return std::isspace(c); }), w.end());

            if (!w.empty())
                banned_.insert(w);
        }
    }

    void HookChatFilter() {
        rmm::On<rmm_ev_chat>(
            RMM_EV_CHAT,
            [this](rmm_ev_chat& e) -> rmm_result {
                if (e.player == RMM_PLAYER_NONE || rmm::HasFlags(e.player, RMM_ADMFLAG_CHAT))
                    return RMM_CONTINUE;

                std::string text = e.text, lower = Lower(text);
                const std::string* hit = nullptr;

                for (auto& w : banned_)
                    if (lower.find(w) != std::string::npos) {
                        hit = &w;
                        break;
                    }

                if (!hit)
                    return RMM_CONTINUE;

                int count = ++violations_[e.player];
                ViolationEvent ve{e.player, hit->c_str(), count};

                // Other plugins may veto the punishment by blocking our custom event.
                if (rmm::Api().events->fire(rmm::g_self, violationEvent_, &ve) >= RMM_HANDLED)
                    return RMM_CONTINUE;

                int kickAfter = kickAfter_.Int();

                if (kickAfter > 0 && count >= kickAfter) {
                    rmm::Api().admin->kick(e.player, "Language", RMM_PLAYER_NONE);

                    return RMM_HANDLED;
                }

                if (rmm::EventBlockable(RMM_EV_CHAT)) {
                    rmm::Api().players->message(e.player, "Watch your language.");

                    return RMM_HANDLED;  // message never reaches other players
                }

                if (rmm::Api().events->flags(RMM_EV_CHAT) & RMM_EVF_MUTABLE) {
                    for (auto& w : banned_)  // censor in place
                        for (size_t pos = Lower(text).find(w); pos != std::string::npos;
                             pos = Lower(text).find(w, pos + 1))
                            text.replace(pos, w.size(), std::string(w.size(), '*'));

                    std::snprintf(e.text, e.text_cap, "%s", text.c_str());

                    return RMM_CHANGED;
                }

                rmm::SayAll(rmm::Format("[Filter] %s: watch your language (%d/%d)", rmm::PlayerName(e.player).c_str(),
                                        count, kickAfter));

                return RMM_CONTINUE;
            },
            RMM_PHASE_PRE, RMM_PRIORITY_HIGH);
    }

    static void RMM_CALL AddWordThunk(const char* word) {
        if (!self || !word)
            return;

        std::string list = self->words_.Str();
        self->words_.Set(list.empty() ? word : list + "," + word);
    }

    static int32_t RMM_CALL ViolationsThunk(rmm_player_t p) {
        if (!self)
            return 0;

        auto it = self->violations_.find(p);

        return it == self->violations_.end() ? 0 : it->second;
    }

    rmm::Var logEnabled_, words_, kickAfter_;
    std::set<std::string> banned_;
    std::map<rmm_player_t, int> violations_;
    int32_t violationEvent_ = 0;
    IChatFilterV1 iface_{};
    std::FILE* file_ = nullptr;
};

EventMonitor* EventMonitor::self = nullptr;

RMM_DECLARE_PLUGIN(EventMonitor, "Event Monitor", "1.0.0", "RMM Team",
                   "Event logging, chat filter, custom events and interfaces demo", "", "evmon", RMM_GAME_ANY, 0)
