// Data-driven default implementation of IGameAdapter.
//
// Refractor exposes nearly all server control through console commands
// ("admin.kickPlayer 3", "game.sayAll \"hi\"", "maplist.append ..."). The
// names differ between games and even between mods, so AdapterBase reads them
// from rmm/gamedata/<game>.ini:
//
//   [commands]   kick = admin.kickPlayer {id}
//   [settings]   gravity = physics.gravity
//
// Placeholders: {id} {team} {index} {minutes} {score} {tickets} {size} {x} {y}
// {z} {mod} are inserted verbatim; {msg} {reason} {map} {mode} {name} are quoted. A
// command/setting that is missing from gamedata is reported as unsupported
// (and the matching capability is off). Concrete adapters only have to
// provide console execution, player data, events and lifecycle.
#pragma once

#include "../game_adapter.h"
#include "../../core/util.h"

#include <map>
#include <string>

namespace rmm {

class AdapterBase : public IGameAdapter {
public:
    // ---- gamedata ------------------------------------------------------------------
    bool LoadGameData(const std::string& gamedataDir, std::string& err);

    const Ini& Data() const { return data_; }

    bool HasTemplate(const std::string& key) const { return !data_.Get("commands", key).empty(); }

    /// Build a command from a template; returns "" if the template is missing.
    std::string BuildCommand(const std::string& key, const std::map<std::string, std::string>& args) const;
    /// Build and execute.
    bool RunTemplate(const std::string& key, const std::map<std::string, std::string>& args,
                     std::string* out = nullptr);

    // ---- IGameAdapter defaults ---------------------------------------------------------
    const char* ShortName() const override { return shortName_.c_str(); }

    std::string ModName() override { return mod_; }

    bool HasCap(int32_t cap) const override;

    int32_t Status() override { return status_; }

    std::string SettingVar(int32_t setting) const override;
    bool GetEngineVar(const std::string& name, std::string& value) override;
    bool SetEngineVar(const std::string& name, const std::string& value) override;

    bool KickPlayer(rmm_player_t id, const std::string& reason) override;
    bool BanPlayerEngine(rmm_player_t id, int32_t minutes, const std::string& reason) override;
    bool KillPlayer(rmm_player_t id) override;
    bool SetTeam(rmm_player_t id, int32_t team) override;
    bool PrivateMessage(rmm_player_t id, const std::string& msg) override;
    bool SetScore(rmm_player_t id, int32_t score) override;
    bool Teleport(rmm_player_t id, const rmm_vec3& pos) override;

    bool CurrentMap(rmm_map_info& out) override;
    bool ChangeMap(const std::string& map, const std::string& mode, int32_t size) override;
    bool RestartMap() override;
    bool RunNextMap() override;
    int32_t MaplistCount() override;
    bool MaplistGet(int32_t index, rmm_map_info& out) override;
    bool MaplistAppend(const std::string& map, const std::string& mode, int32_t size) override;
    bool MaplistClear() override;
    bool SetNextMap(int32_t index) override;

    bool SayAll(const std::string& msg) override;
    bool SayTeam(int32_t team, const std::string& msg) override;
    bool CenterMessage(const std::string& msg) override;
    int32_t GetTickets(int32_t team) override;
    bool SetTickets(int32_t team, int32_t tickets) override;
    bool ObjectTemplate(rmm_object_t obj, std::string& out) override;

    // Object/damage/item defaults are "unsupported", except occupants derived from player info.
    bool DamagePlayer(rmm_player_t, float, rmm_player_t) override { return false; }

    bool PlayerHealth(rmm_player_t, float&, float&) override { return false; }

    std::vector<VehicleRef> Vehicles() override { return {}; }

    bool ObjectValid(rmm_object_t) override { return false; }

    bool GetObjectInfo(rmm_object_t, rmm_object_info&) override { return false; }

    std::vector<rmm_player_t> ObjectOccupants(rmm_object_t obj) override;

    bool SetObjectPosition(rmm_object_t, const rmm_vec3&) override { return false; }

    bool SetObjectRotation(rmm_object_t, const rmm_vec3&) override { return false; }

    bool SetObjectVelocity(rmm_object_t, const rmm_vec3&, const rmm_vec3&) override { return false; }

    bool PushPlayer(rmm_player_t, const rmm_vec3&) override { return false; }

    std::vector<ObjectRef> WorldObjects() override { return {}; }

    rmm_object_t SpawnObject(const std::string&, const rmm_vec3&, const rmm_vec3&) override { return 0; }

    bool RemoveObject(rmm_object_t) override { return false; }

    bool GroundHeight(float, float, float&) override { return false; }

    bool TemplateExists(const std::string&) override { return false; }

    // Bots: console templates bot_* in [commands], parameters in [bots]
    // (adapter_bots.cpp; see gamedata/bf1942.ini for the output formats parsed).
    bool BotsAvailable() override;
    std::vector<BotRef> Bots() override;
    bool GetBotInfo(const BotRef& bot, rmm_bot_info& out) override;
    std::vector<std::string> BotBehaviours(const BotRef& bot) override;
    bool BotAddBegin(const BotSpawn& s, std::string& err) override;
    BotAddState BotAddPoll(const BotSpawn& s, int attempt, std::string& err) override;
    bool BotRemove(const BotRef& bot) override;
    bool BotRemoveAll() override;
    bool BotMoveTo(const BotRef& bot, const rmm_vec3& pos, float radius) override;
    bool BotPatrol(const BotRef& bot, const std::vector<rmm_vec3>& points, float radius) override;
    bool BotOrder(const BotRef& bot, const std::string& behaviour, const std::string& modifiers) override;
    bool BotGetStrategy(int32_t team, std::string& out) override;
    std::vector<std::string> BotStrategies(int32_t team) override;
    bool BotSetStrategy(int32_t team, const std::string& strategy) override;
    bool BotGetSkill(float& skill) override;
    bool BotSetSkill(float skill) override;
    int32_t BotMaxPatrolPoints() const override;

    // Console templates set_kit ({id} {kit}) and end_round ({team}).
    bool SetKit(rmm_player_t id, const std::string& kitTemplate) override;
    bool EndRoundNow(int32_t winner) override;

    // No console command does this on any game.
    bool PutInVehicle(rmm_player_t, rmm_object_t, int32_t) override { return false; }

    bool TakeOutOfVehicle(rmm_player_t) override { return false; }

    bool DamageObject(rmm_object_t, float, rmm_player_t) override { return false; }

    bool GetItems(rmm_player_t, std::vector<rmm_item_info>&) override { return false; }

    bool SelectItem(rmm_player_t, int32_t) override { return false; }

    bool RemoveItem(rmm_player_t, int32_t) override { return false; }

    bool GetSpawnTiming(rmm_spawn_timing&) override { return false; }

    bool SetSpawnTiming(const rmm_spawn_timing&) override { return false; }

    std::vector<rmm_object_t> ControlPoints() override { return {}; }

    bool GetControlPoint(rmm_object_t, rmm_control_point_info&) override { return false; }

    bool SetControlPoint(rmm_object_t, int32_t) override { return false; }

    bool LockControlPoint(rmm_object_t, bool) override { return false; }

protected:
    /// Run a bot_* query template and return its trimmed output; false if the
    /// template is missing, the command failed, or the engine printed an error.
    bool BotQuery(const std::string& key, const std::map<std::string, std::string>& args, std::string& out);
    /// "x/y/z" (Refractor Vec3) or "x y z" / "x,y,z".
    static bool ParseVec3(const std::string& s, rmm_vec3& out);
    static std::string Vec3Arg(const rmm_vec3& v);

    /// Record the current map (call from the adapter's map-load detection).
    void NoteMap(const std::string& map, const std::string& mode, int32_t size);
    /// Parse "maplist.list"-style output into entries.
    std::vector<rmm_map_info> ParseMaplist(const std::string& output) const;

    /// Override to force a capability on/off regardless of gamedata.
    virtual int CapOverride(int32_t /*cap*/) const { return -1; }  // -1 = no override

    Ini data_;
    std::string shortName_;
    std::string mod_;
    int32_t status_ = RMM_STATUS_UNKNOWN;
    rmm_map_info currentMap_{sizeof(rmm_map_info), {0}, {0}, 0};
};

}  // namespace rmm
