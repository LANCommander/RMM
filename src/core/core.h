// The RMM core: owns every subsystem and mediates between the game adapter
// (below) and plugins (above).
#pragma once

#include "admin_manager.h"
#include "bot_control.h"
#include "bot_tracker.h"
#include "chat_queue.h"
#include "command_registry.h"
#include "event_bus.h"
#include "logger.h"
#include "plugin_manager.h"
#include "query_rules.h"
#include "safe_call.h"
#include "timer_manager.h"
#include "var_registry.h"

#include "../games/game_adapter.h"

#include <map>
#include <memory>
#include <string>

#define RMM_VERSION_STRING "1.0.0"

namespace rmm {

class Core {
public:
    static Core& Get();

    // Lifecycle, driven by the host/adapter.
    /// Create subsystems and attach the adapter. @p rootDir = RMM root folder.
    bool Start(std::unique_ptr<IGameAdapter> adapter, const std::string& rootDir, std::string& err);
    /// The adapter can now execute console commands: load plugins.
    void OnGameReady();
    /// Call every server tick (or as often as the backend allows).
    void Frame();
    void Shutdown();

    bool Ready() const { return ready_; }

    IGameAdapter& Game() { return *adapter_; }

    EventBus& Events() { return events_; }

    VarRegistry& Vars() { return vars_; }

    CommandRegistry& Cmds() { return cmds_; }

    TimerManager& Timers() { return timers_; }

    AdminManager& Admin() { return admin_; }

    PluginManager& Plugins() { return plugins_; }

    Logger& Log() { return log_; }

    /// Paced in-game chat output (say_all/say_team/private messages/replies).
    ChatQueue& Chat() { return chat_; }

    /// Bot add requests and BOT_ADDED/REMOVED events.
    BotTracker& Bots() { return bots_; }

    /// Who drives which bot, latched/recorded bot input.
    BotControl& BotInput() { return botInput_; }

    /// Plugin pairs for the server's query replies (any thread).
    QueryRules& Query() { return query_; }

    const rmm_api* Api() const;

    // Adapter entry points: implement core policy around raw events.
    /// Generic dispatch for events with no core policy.
    int32_t Dispatch(int32_t ev, void* data) { return events_.Dispatch(ev, data); }

    /// @return false to reject the player (reason filled).
    bool OnPlayerConnect(rmm_player_t p, const std::string& name, const std::string& address,
                         std::string& rejectReason);
    void OnPlayerAuth(rmm_player_t p);
    void OnPlayerDisconnect(rmm_player_t p);
    /// @p text may be modified by plugins. @return true to block the message.
    bool OnChat(rmm_player_t p, int32_t channel, std::string& text);
    void OnMapLoad(const std::string& map, const std::string& mode, const std::string& mod, int32_t size);
    /// Console/RCon line typed by an admin. @return true if RMM handled it (do
    /// not pass to the engine). Output appended to @p out.
    bool OnConsoleCommand(int32_t source, rmm_player_t player, const std::string& line, std::string* out);

    /// RMM command -> RMM var get/set -> engine console, in that order.
    bool ExecLine(int32_t source, rmm_player_t caller, const std::string& line, std::string* out);
    bool ExecConfig(const std::string& path);
    std::string BuildPath(int32_t kind, const std::string& rel) const;
    double Time() const;
    std::string PlayerLabel(rmm_player_t p);
    int32_t CoreVarInt(const std::string& name);
    std::string CoreVarString(const std::string& name);

    bool IfaceRegister(Plugin* owner, const std::string& name, uint32_t version, void* iface);
    void* IfaceQuery(const std::string& name, uint32_t minVersion, uint32_t* outVersion);

    /// Remove everything a plugin registered.
    void ReleaseResources(Plugin* p);

    /// Run plugin code with crash isolation. @p p may be nullptr (core code).
    template <class F> bool CallPlugin(Plugin* p, const char* what, F&& f) {
        if (!p) {
            f();

            return true;
        }

        std::string fault;
        ++callDepth_;
        bool ok = Guard(f, fault);
        --callDepth_;

        if (!ok)
            OnPluginFault(p, what, fault);

        return ok;
    }

    int CallDepth() const { return callDepth_; }

private:
    Core() = default;
    void OnPluginFault(Plugin* p, const char* what, const std::string& fault);
    void RegisterCoreVars();
    void InstallCoreHooks();

    std::unique_ptr<IGameAdapter> adapter_;
    EventBus events_;
    VarRegistry vars_;
    CommandRegistry cmds_;
    TimerManager timers_;
    AdminManager admin_;
    PluginManager plugins_;
    Logger log_;
    ChatQueue chat_;
    BotTracker bots_;
    BotControl botInput_;
    QueryRules query_;

    struct IfaceRec {
        Plugin* owner;
        uint32_t version;
        void* ptr;
    };

    std::map<std::string, IfaceRec> ifaces_;

    std::string root_;
    double startTime_ = 0, lastFrame_ = 0;
    int callDepth_ = 0;
    bool started_ = false, ready_ = false;
};

/// Built-in console/chat commands ("rmm ...", "help"). builtin_commands.cpp
void RegisterBuiltinCommands(Core& core);
/// Stable lower-case keys for rmm_setting / rmm_cap values, as used in
/// gamedata files ("gravity", "kick", ...). nullptr if out of range.
const char* SettingKey(int32_t setting);
const char* CapKey(int32_t cap);
/// rmm_world_api::find_objects filtering (pattern, RMM_FIND_NO_* flags, and a
/// 3D radius around @p center, nearest first; whole map when center is null).
/// builtin_commands.cpp
std::vector<IGameAdapter::ObjectRef> FilterObjects(std::vector<IGameAdapter::ObjectRef> all, const std::string& pattern,
                                                   const rmm_vec3* center, float radius, uint32_t flags);
/// Fill the C API tables. api_tables.cpp
const rmm_api* BuildApiTables();

}  // namespace rmm
