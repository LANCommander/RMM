#include "core.h"

#include "platform.h"
#include "util.h"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>

namespace rmm {

Core& Core::Get() {
    static Core instance;

    return instance;
}

const rmm_api* Core::Api() const {
    static const rmm_api* api = BuildApiTables();

    return api;
}

double Core::Time() const { return plat::MonotonicSeconds() - startTime_; }

std::string Core::BuildPath(int32_t kind, const std::string& rel) const {
    static const char* dirs[] = {"", "plugins", "configs", "data", "logs", "gamedata"};
    std::string base = root_;

    if (kind > RMM_PATH_ROOT && kind <= RMM_PATH_GAMEDATA)
        base = plat::JoinPath(root_, dirs[kind]);

    return rel.empty() ? base : plat::JoinPath(base, rel);
}

bool Core::Start(std::unique_ptr<IGameAdapter> adapter, const std::string& rootDir, std::string& err) {
    if (started_) {
        err = "already started";

        return false;
    }

    adapter_ = std::move(adapter);
    root_ = rootDir;
    startTime_ = plat::MonotonicSeconds();
    lastFrame_ = Time();

    for (int k = RMM_PATH_PLUGINS; k <= RMM_PATH_GAMEDATA; ++k)
        plat::MakeDirs(BuildPath(k, ""));

    plat::MakeDirs(BuildPath(RMM_PATH_CONFIGS, "plugins"));
    log_.Open(BuildPath(RMM_PATH_LOGS, ""));
    log_.Info("Refractor MultiMod %s starting - %s (%s backend), root %s", RMM_VERSION_STRING, adapter_->GameName(),
              adapter_->Backend(), root_.c_str());

    events_.SetFlagsProvider([this](int32_t ev) { return adapter_->EventFlags(ev); });
    vars_.SetArchivePath(BuildPath(RMM_PATH_CONFIGS, "rmm_vars.cfg"));
    vars_.LoadArchive();
    RegisterCoreVars();
    admin_.SetPaths(BuildPath(RMM_PATH_CONFIGS, "admins.cfg"), BuildPath(RMM_PATH_DATA, "bans.txt"));
    plugins_.SetDirs(BuildPath(RMM_PATH_PLUGINS, ""), BuildPath(RMM_PATH_PLUGINS, ".cache"));
    RegisterBuiltinCommands(*this);
    InstallCoreHooks();
    started_ = true;

    if (!adapter_->Attach(*this, err)) {
        log_.Error("Game adapter failed to attach: %s", err.c_str());

        return false;
    }

    return true;
}

void Core::OnGameReady() {
    if (ready_)
        return;

    ready_ = true;
    log_.SetConsoleSink([this](const std::string& line) { adapter_->ConsolePrint(line); });
    admin_.Load();
    std::string coreCfg = BuildPath(RMM_PATH_CONFIGS, "core.cfg");

    if (plat::FileExists(coreCfg))
        ExecConfig(coreCfg);

    plugins_.LoadAll();
    log_.Info("%zu plugin(s) loaded", plugins_.All().size());
    events_.Dispatch(RMM_EV_SERVER_START, nullptr);
}

void Core::Frame() {
    if (!ready_)
        return;

    double now = Time();
    rmm_ev_frame ev{now - lastFrame_, now};
    lastFrame_ = now;
    events_.Dispatch(RMM_EV_FRAME, &ev);
    timers_.Process(now);
    chat_.Process(now);
    bots_.Process(now);
    vars_.PollWatches(now);
    plugins_.ProcessPending();
}

void Core::Shutdown() {
    if (!started_)
        return;

    if (ready_)
        events_.Dispatch(RMM_EV_SERVER_SHUTDOWN, nullptr);

    if (ready_)
        chat_.Flush();

    plugins_.UnloadAll();
    vars_.SaveArchive();

    if (adapter_)
        adapter_->Detach();

    log_.SetConsoleSink(nullptr);
    log_.Info("Refractor MultiMod shut down");
    log_.Close();
    started_ = ready_ = false;
}

static void RMM_CALL OnLogLevelChanged(rmm_handle_t, const char*, const char* v, void*) {
    Core::Get().Log().SetMinLevel(std::atoi(v));
}

void Core::RegisterCoreVars() {
    auto mk = [this](const char* n, const char* d, const char* desc, uint32_t f) {
        return vars_.Create(nullptr, n, d, desc, f);
    };

    rmm_handle_t ver = mk("rmm.version", RMM_VERSION_STRING, "Refractor MultiMod version", RMM_VAR_READONLY);
    (void)ver;
    mk("rmm.chatTrigger", "!", "Prefix for public chat commands", RMM_VAR_ARCHIVE);
    mk("rmm.silentTrigger", "/", "Prefix for silent chat commands (hidden where the game allows)", RMM_VAR_ARCHIVE);
    mk("rmm.replyFallback", "1", "1 = reply via public chat when the game has no private messages",
       RMM_VAR_ARCHIVE | RMM_VAR_NUMERIC);
    rmm_handle_t pace =
        mk("rmm.chatInterval", "1.0", "Seconds between chat lines to the same player/team/everyone (0 = no pacing)",
           RMM_VAR_ARCHIVE | RMM_VAR_NUMERIC);
    vars_.Get(pace)->hasMin = true;
    vars_.Get(pace)->min = 0;
    vars_.Get(pace)->hasMax = true;
    vars_.Get(pace)->max = 10;
    mk("rmm.mirrorEngineBans", "0", "1 = also add RMM bans to the engine's own ban list",
       RMM_VAR_ARCHIVE | RMM_VAR_NUMERIC);
    rmm_handle_t lvl = mk("rmm.logLevel", "1", "0 debug, 1 info, 2 warn, 3 error", RMM_VAR_ARCHIVE | RMM_VAR_NUMERIC);
    vars_.Get(lvl)->hasMin = true;
    vars_.Get(lvl)->min = 0;
    vars_.Get(lvl)->hasMax = true;
    vars_.Get(lvl)->max = 3;
    log_.SetMinLevel(CoreVarInt("rmm.logLevel"));
    vars_.HookChange(nullptr, lvl, &OnLogLevelChanged, nullptr);
}

void Core::InstallCoreHooks() {
    // Clear permissions and runtime grants on disconnect, late in POST so
    // plugins still see admin status. Player ids are slot numbers and get reused.
    events_.Hook(
        nullptr, RMM_EV_PLAYER_DISCONNECT, RMM_PHASE_POST, -1000,
        [](int32_t, int32_t, void* d, void*) -> int32_t {
            Core::Get().Admin().PlayerLeft(static_cast<rmm_ev_player*>(d)->player);
            Core::Get().Chat().PlayerLeft(static_cast<rmm_ev_player*>(d)->player);
            Core::Get().BotInput().Forget(static_cast<rmm_ev_player*>(d)->player);

            return RMM_CONTINUE;
        },
        nullptr);
    // Map change: kill map-scoped timers and release all bot control.
    events_.Hook(
        nullptr, RMM_EV_MAP_LOAD, RMM_PHASE_POST, 1000,
        [](int32_t, int32_t, void*, void*) -> int32_t {
            Core::Get().Timers().OnMapChange();
            Core::Get().BotInput().Clear();

            return RMM_CONTINUE;
        },
        nullptr);
}

std::string Core::CoreVarString(const std::string& name) {
    Var* v = vars_.Find(name);

    return v ? v->value : std::string();
}

int32_t Core::CoreVarInt(const std::string& name) { return std::atoi(CoreVarString(name).c_str()); }

std::string Core::PlayerLabel(rmm_player_t p) {
    if (p == RMM_PLAYER_NONE)
        return "Console";

    rmm_player_info info{};
    info.struct_size = sizeof info;

    if (adapter_ && adapter_->GetPlayer(p, info))
        return info.name;

    return "#" + std::to_string(p);
}

bool Core::OnPlayerConnect(rmm_player_t p, const std::string& name, const std::string& address, std::string& reject) {
    admin_.PlayerLeft(p);  // a fresh occupant of this id: never inherit grants (even if a disconnect was missed)
    // Core policy first: RMM bans (name/IP are known at connect time).
    rmm_player_info info{};
    info.struct_size = sizeof info;
    CopyOut(name, info.name, sizeof info.name);
    CopyOut(address, info.address, sizeof info.address);
    std::string reason;

    if (admin_.CheckPlayerBanned(info, reason)) {
        reject = "You are banned: " + reason;
        log_.Info("Rejected banned player %s (%s): %s", name.c_str(), address.c_str(), reason.c_str());

        if (!(events_.Flags(RMM_EV_PLAYER_CONNECT) & RMM_EVF_BLOCKABLE))
            adapter_->KickPlayer(p, reject);

        return false;
    }

    char rejectBuf[256] = "Connection rejected by server";
    rmm_ev_player_connect ev{p, name.c_str(), address.c_str(), rejectBuf, sizeof rejectBuf};
    int32_t res = events_.Dispatch(RMM_EV_PLAYER_CONNECT, &ev);

    if (res >= RMM_HANDLED) {
        reject = rejectBuf;

        return false;
    }

    return true;
}

void Core::OnPlayerAuth(rmm_player_t p) {
    admin_.Invalidate(p);
    rmm_player_info info{};
    info.struct_size = sizeof info;

    if (adapter_->GetPlayer(p, info)) {
        std::string reason;

        if (admin_.CheckPlayerBanned(info, reason)) {
            log_.Info("Kicking banned player %s (%s): %s", info.name, info.auth_id, reason.c_str());
            adapter_->KickPlayer(p, "You are banned: " + reason);

            return;
        }
    }

    rmm_ev_player ev{p};
    events_.Dispatch(RMM_EV_PLAYER_AUTH, &ev);
}

void Core::OnPlayerDisconnect(rmm_player_t p) {
    rmm_ev_player ev{p};
    events_.Dispatch(RMM_EV_PLAYER_DISCONNECT, &ev);
}

bool Core::OnChat(rmm_player_t p, int32_t channel, std::string& text) {
    bool blockable = (events_.Flags(RMM_EV_CHAT) & RMM_EVF_BLOCKABLE) != 0;

    if (p != RMM_PLAYER_NONE) {
        bool hide = false;

        if (cmds_.HandleChat(p, text, hide) && hide && blockable) {
            plugins_.ProcessPending();

            return true;  // silent command: swallow the line entirely
        }
    }

    std::vector<char> buf(text.size() + 256, '\0');
    std::memcpy(buf.data(), text.data(), text.size());
    rmm_ev_chat ev{p, channel, buf.data(), buf.size()};
    int32_t res = events_.Dispatch(RMM_EV_CHAT, &ev);
    buf.back() = '\0';

    if (res == RMM_CHANGED && (events_.Flags(RMM_EV_CHAT) & RMM_EVF_MUTABLE))
        text = buf.data();

    plugins_.ProcessPending();

    return res >= RMM_HANDLED;
}

void Core::OnMapLoad(const std::string& map, const std::string& mode, const std::string& mod, int32_t size) {
    log_.Info("Map load: %s (%s, %s, size %d)", map.c_str(), mode.c_str(), mod.c_str(), size);
    rmm_ev_map ev{map.c_str(), mode.c_str(), mod.c_str(), size};
    events_.Dispatch(RMM_EV_MAP_LOAD, &ev);
}

bool Core::OnConsoleCommand(int32_t source, rmm_player_t player, const std::string& line, std::string* out) {
    rmm_ev_rcon ev{source, player, line.c_str()};

    if (events_.Dispatch(RMM_EV_RCON_COMMAND, &ev) >= RMM_HANDLED)
        return true;

    auto argv = SplitArgs(line);

    if (argv.empty())
        return false;

    bool handled = cmds_.Execute(source, player, line, out);

    if (!handled && vars_.Find(argv[0]))
        handled = ExecLine(source, player, line, out);

    plugins_.ProcessPending();

    return handled;
}

bool Core::ExecLine(int32_t source, rmm_player_t caller, const std::string& line, std::string* out) {
    std::string t = Trim(line);

    if (t.empty() || t[0] == ';' || t[0] == '#' || StartsWith(t, "//") || StartsWith(t, "rem "))
        return true;

    if (cmds_.Execute(source, caller, t, out))
        return true;

    auto argv = SplitArgs(t);

    if (Var* v = vars_.Find(argv[0])) {
        if (argv.size() == 1) {
            bool hide = (v->flags & RMM_VAR_PROTECTED) && caller != RMM_PLAYER_NONE &&
                        !admin_.HasFlags(caller, RMM_ADMFLAG_ROOT);

            if (out)
                *out += v->name + " = " + (hide ? "***" : QuoteArg(v->value));

            return true;
        }

        std::string err;
        bool ok = vars_.Set(nullptr, v, argv[1], &err);

        if (!ok && out)
            *out += v->name + ": " + err;

        return ok;
    }

    // Only ask for the output if there's somewhere to put it: adapters that
    // capture engine output would otherwise swallow it (e.g. config files).
    std::string engineOut;
    bool ok = adapter_->Exec(t, out ? &engineOut : nullptr);

    if (out)
        *out += engineOut;

    return ok;
}

bool Core::ExecConfig(const std::string& path) {
    std::ifstream f(path);

    if (!f)
        return false;

    log_.Debug("Executing %s", path.c_str());
    std::string line;

    while (std::getline(f, line)) {
        // Note: to preset a variable of a plugin that is not loaded yet, use
        // "rmm var <name> <value>" (stored as pending); a bare "<name> <value>"
        // for an unknown name is passed to the engine console.
        std::string t = Trim(line);

        if (t.empty())
            continue;

        std::string out;
        ExecLine(RMM_SRC_PLUGIN, RMM_PLAYER_NONE, t, &out);

        if (!out.empty())
            log_.Debug("%s", out.c_str());
    }

    return true;
}

bool Core::IfaceRegister(Plugin* owner, const std::string& name, uint32_t version, void* iface) {
    if (name.empty() || !iface)
        return false;

    auto it = ifaces_.find(name);

    if (it != ifaces_.end() && it->second.owner != owner)
        return false;

    ifaces_[name] = IfaceRec{owner, version, iface};

    return true;
}

void* Core::IfaceQuery(const std::string& name, uint32_t minVersion, uint32_t* outVersion) {
    auto it = ifaces_.find(name);

    if (it == ifaces_.end() || it->second.version < minVersion)
        return nullptr;

    if (it->second.owner && it->second.owner->state == PluginState::Failed)
        return nullptr;

    if (outVersion)
        *outVersion = it->second.version;

    return it->second.ptr;
}

void Core::ReleaseResources(Plugin* p) {
    events_.RemoveAll(p);
    cmds_.RemoveAll(p);
    vars_.RemoveAll(p);
    timers_.RemoveAll(p);
    botInput_.ReleaseAll(p);
    query_.RemoveAll(p);

    for (auto it = ifaces_.begin(); it != ifaces_.end();)
        it = it->second.owner == p ? ifaces_.erase(it) : std::next(it);
}

void Core::OnPluginFault(Plugin* p, const char* what, const std::string& fault) {
    std::string msg = Format("Plugin '%s' crashed in %s: %s. Plugin disabled; use 'rmm plugins reload %s'.",
                             p->name.empty() ? p->file.c_str() : p->name.c_str(), what, fault.c_str(), p->file.c_str());
    log_.Error("%s", msg.c_str());
    plugins_.MarkFailed(p, std::string(what) + ": " + fault);
}

}  // namespace rmm
