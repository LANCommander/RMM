// The seam between the game-independent core and a specific game/backend.
//
// One adapter exists per (game, backend) pair. It is the only code that knows
// how to talk to the engine: how to run a console command, where player data
// lives, which hooks produce which events. Everything an adapter *can* express
// as data (console command templates, setting -> variable names) belongs in
// rmm/gamedata/<game>.ini instead, so server patches and mods can be supported
// without recompiling. See AdapterBase for the data-driven default behaviour.
#pragma once

#include <rmm/rmm_api.h>

#include <string>
#include <vector>

namespace rmm {

class Core;

class IGameAdapter {
public:
    virtual ~IGameAdapter() = default;

    // ---- identity ------------------------------------------------------------
    virtual rmm_game GameId() const = 0;
    virtual const char* GameName() const = 0;
    virtual const char* ShortName() const = 0;  // gamedata file stem
    virtual const char* Backend() const = 0;    // "native", "python-bridge", "sim"
    virtual std::string ModName() = 0;

    // ---- lifecycle -----------------------------------------------------------
    /// Install hooks. The adapter must later call Core::OnGameReady() once
    /// console commands work, Core::Frame() regularly, and Core::Dispatch()
    /// for events. Called once.
    virtual bool Attach(Core& core, std::string& error) = 0;
    virtual void Detach() = 0;

    // ---- capabilities ----------------------------------------------------------
    virtual bool HasCap(int32_t cap) const = 0;
    /// RMM_EVF_* flags for a built-in event on this game/backend.
    virtual uint32_t EventFlags(int32_t eventId) const = 0;
    virtual int32_t Status() = 0;  // rmm_game_status

    // ---- console ---------------------------------------------------------------
    virtual bool Exec(const std::string& command, std::string* output) = 0;
    /// Print a line to the server console (used as the log sink).
    virtual void ConsolePrint(const std::string& line) = 0;

    // ---- variables ---------------------------------------------------------------
    virtual std::string SettingVar(int32_t setting) const = 0;
    virtual bool GetEngineVar(const std::string& name, std::string& value) = 0;
    virtual bool SetEngineVar(const std::string& name, const std::string& value) = 0;

    // ---- players -----------------------------------------------------------------
    virtual int32_t MaxPlayers() = 0;
    virtual std::vector<rmm_player_t> PlayerIds() = 0;
    virtual bool GetPlayer(rmm_player_t id, rmm_player_info& out) = 0;
    virtual bool KickPlayer(rmm_player_t id, const std::string& reason) = 0;
    virtual bool BanPlayerEngine(rmm_player_t id, int32_t minutes, const std::string& reason) = 0;
    virtual bool KillPlayer(rmm_player_t id) = 0;
    virtual bool SetTeam(rmm_player_t id, int32_t team) = 0;
    virtual bool PrivateMessage(rmm_player_t id, const std::string& msg) = 0;
    virtual bool SetScore(rmm_player_t id, int32_t score) = 0;
    virtual bool Teleport(rmm_player_t id, const rmm_vec3& pos) = 0;

    // ---- maps --------------------------------------------------------------------
    virtual bool CurrentMap(rmm_map_info& out) = 0;
    virtual bool ChangeMap(const std::string& map, const std::string& mode, int32_t size) = 0;
    virtual bool RestartMap() = 0;
    virtual bool RunNextMap() = 0;
    virtual int32_t MaplistCount() = 0;
    virtual bool MaplistGet(int32_t index, rmm_map_info& out) = 0;
    virtual bool MaplistAppend(const std::string& map, const std::string& mode, int32_t size) = 0;
    virtual bool MaplistClear() = 0;
    virtual bool SetNextMap(int32_t index) = 0;

    // ---- messaging & round ------------------------------------------------------------
    virtual bool SayAll(const std::string& msg) = 0;
    virtual bool SayTeam(int32_t team, const std::string& msg) = 0;
    virtual bool CenterMessage(const std::string& msg) = 0;

    /// Longest chat line the game shows (0 = no limit). The chat queue splits
    /// longer messages at word boundaries so each piece is paced.
    virtual size_t ChatLineLimit() const { return 0; }

    virtual int32_t GetTickets(int32_t team) = 0;
    virtual bool SetTickets(int32_t team, int32_t tickets) = 0;
    virtual bool ObjectTemplate(rmm_object_t obj, std::string& out) = 0;

    // ---- damage ----------------------------------------------------------------------
    virtual bool DamagePlayer(rmm_player_t id, float amount, rmm_player_t attacker) = 0;
    virtual bool PlayerHealth(rmm_player_t id, float& health, float& maxHealth) = 0;

    // ---- world objects ---------------------------------------------------------------
    struct VehicleRef {
        rmm_object_t handle;
        std::string templateName;
        bool isRoot;
    };

    /// Every player-controllable object (vehicles and their seats). The core
    /// applies template patterns and RMM_FIND_* flags.
    virtual std::vector<VehicleRef> Vehicles() = 0;
    virtual bool ObjectValid(rmm_object_t obj) = 0;
    /// @p out arrives zeroed with struct_size set; fill what the game knows.
    virtual bool GetObjectInfo(rmm_object_t obj, rmm_object_info& out) = 0;
    /// Players anywhere in @p obj's root vehicle.
    virtual std::vector<rmm_player_t> ObjectOccupants(rmm_object_t obj) = 0;
    virtual bool SetObjectPosition(rmm_object_t obj, const rmm_vec3& pos) = 0;
    virtual bool SetObjectRotation(rmm_object_t obj, const rmm_vec3& yawPitchRoll) = 0;
    virtual bool SetObjectVelocity(rmm_object_t obj, const rmm_vec3& linear, const rmm_vec3& angular) = 0;
    /// Add @p velocity (m/s, Y up) to a living on-foot player's soldier.
    virtual bool PushPlayer(rmm_player_t id, const rmm_vec3& velocity) = 0;

    // ---- any world object, spawn/remove, terrain ----------------------------------------
    struct ObjectRef {
        rmm_object_t handle;
        std::string templateName;
        rmm_vec3 position;
        bool isVehicle;
        bool isSoldier;
    };

    /// Every top-level world object. The core applies patterns, radius and flags.
    virtual std::vector<ObjectRef> WorldObjects() = 0;
    virtual rmm_object_t SpawnObject(const std::string& tmpl, const rmm_vec3& pos, const rmm_vec3& yawPitchRoll) = 0;
    virtual bool RemoveObject(rmm_object_t obj) = 0;
    virtual bool GroundHeight(float x, float z, float& height) = 0;
    virtual bool TemplateExists(const std::string& tmpl) = 0;

    // ---- bots (the core maps player ids to BotRefs and runs add requests) ---------
    struct BotRef {
        rmm_player_t player;
        int32_t botId;  // engine bot number
        std::string name;
    };

    struct BotSpawn {
        std::string name, kit, personality;
        int32_t team = 1;
        bool at = false;  // use pos
        rmm_vec3 pos{};
    };

    enum class BotAddState { Pending, Done, Failed };

    /// Is the AI running now? Must be safe to call in any game mode; the other
    /// bot functions are only called after it returned true in the same call chain.
    virtual bool BotsAvailable() = 0;
    /// Bots the AI controls, with their player slots.
    virtual std::vector<BotRef> Bots() = 0;
    /// @p out arrives zeroed with struct_size, player, bot_id and name set.
    virtual bool GetBotInfo(const BotRef& bot, rmm_bot_info& out) = 0;
    virtual std::vector<std::string> BotBehaviours(const BotRef& bot) = 0;
    /// Start adding a bot (@p s.name is already unique and console-safe).
    virtual bool BotAddBegin(const BotSpawn& s, std::string& err) = 0;
    /// Called every ~0.25 s after BotAddBegin (@p attempt = 1, 2, ...) until it
    /// stops returning Pending. The core then looks the bot up by name.
    virtual BotAddState BotAddPoll(const BotSpawn& s, int attempt, std::string& err) = 0;
    virtual bool BotRemove(const BotRef& bot) = 0;
    virtual bool BotRemoveAll() = 0;
    virtual bool BotMoveTo(const BotRef& bot, const rmm_vec3& pos, float radius) = 0;
    virtual bool BotPatrol(const BotRef& bot, const std::vector<rmm_vec3>& points, float radius) = 0;
    virtual bool BotOrder(const BotRef& bot, const std::string& behaviour, const std::string& modifiers) = 0;
    virtual bool BotGetStrategy(int32_t team, std::string& out) = 0;
    virtual std::vector<std::string> BotStrategies(int32_t team) = 0;
    virtual bool BotSetStrategy(int32_t team, const std::string& strategy) = 0;
    virtual bool BotGetSkill(float& skill) = 0;
    virtual bool BotSetSkill(float skill) = 0;

    /// Most points BotPatrol accepts.
    virtual int32_t BotMaxPatrolPoints() const { return 2; }

    // ---- kits and rounds -------------------------------------------------------------
    /// Replace a living on-foot player's kit with one made from @p kitTemplate.
    virtual bool SetKit(rmm_player_t id, const std::string& kitTemplate) = 0;
    /// End the current round with @p winner (1/2, 0 = draw).
    virtual bool EndRoundNow(int32_t winner) = 0;

    // ---- vehicles --------------------------------------------------------------------
    /// Put a living player into @p vehicle's seat @p seat (-1 = first free).
    virtual bool PutInVehicle(rmm_player_t id, rmm_object_t vehicle, int32_t seat) = 0;
    virtual bool TakeOutOfVehicle(rmm_player_t id) = 0;
    /// Damage the root of @p obj (not a soldier); destroying it raises the destroyed events.
    virtual bool DamageObject(rmm_object_t obj, float amount, rmm_player_t attacker) = 0;

    // ---- items -----------------------------------------------------------------------
    /// The items of a living on-foot player in slot order; false if dead/unknown/in a vehicle.
    virtual bool GetItems(rmm_player_t id, std::vector<rmm_item_info>& out) = 0;
    virtual bool SelectItem(rmm_player_t id, int32_t slot) = 0;
    virtual bool RemoveItem(rmm_player_t id, int32_t slot) = 0;

    // ---- spawn timing ----------------------------------------------------------------
    virtual bool GetSpawnTiming(rmm_spawn_timing& out) = 0;
    virtual bool SetSpawnTiming(const rmm_spawn_timing& timing) = 0;

    // ---- control points --------------------------------------------------------------
    virtual std::vector<rmm_object_t> ControlPoints() = 0;
    /// @p out arrives zeroed with struct_size and object set.
    virtual bool GetControlPoint(rmm_object_t cp, rmm_control_point_info& out) = 0;
    /// Change the owner at once (spawns follow); raises CONTROL_POINT with no players.
    virtual bool SetControlPoint(rmm_object_t cp, int32_t team) = 0;
    virtual bool LockControlPoint(rmm_object_t cp, bool locked) = 0;
};

}  // namespace rmm
