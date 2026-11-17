// Simulated Refractor server. Used by rmm_simhost to run the real core and
// real plugins without a game: players, chat, kills, vehicles and the console
// are all faked in memory. It reports itself as a real game id (default BF2)
// so game-specific plugins load, but uses gamedata/sim.ini for commands.
#pragma once

#include "../common/adapter_base.h"
#include "../../core/bot_control.h"

#include <map>
#include <string>
#include <vector>

namespace rmm {

class SimAdapter : public AdapterBase {
public:
    explicit SimAdapter(rmm_game emulate);

    rmm_game GameId() const override { return emulate_; }

    const char* GameName() const override { return name_.c_str(); }

    const char* Backend() const override { return "sim"; }

    bool Attach(Core& core, std::string& err) override;

    void Detach() override {}

    uint32_t EventFlags(int32_t ev) const override;
    int CapOverride(int32_t cap) const override;

    bool Exec(const std::string& command, std::string* output) override;
    void ConsolePrint(const std::string& line) override;

    /// Same chat line limit as the emulated game (Refractor 1: 63 characters).
    size_t ChatLineLimit() const override { return (emulate_ & RMM_GAME_REFRACTOR1) ? 63 : 0; }

    int32_t MaxPlayers() override { return maxPlayers_; }

    std::vector<rmm_player_t> PlayerIds() override;
    bool GetPlayer(rmm_player_t id, rmm_player_info& out) override;

    bool DamagePlayer(rmm_player_t id, float amount, rmm_player_t attacker) override;
    bool PlayerHealth(rmm_player_t id, float& health, float& maxHealth) override;
    std::vector<VehicleRef> Vehicles() override;

    bool ObjectValid(rmm_object_t obj) override { return objects_.count(obj) != 0; }

    bool GetObjectInfo(rmm_object_t obj, rmm_object_info& out) override;
    std::vector<rmm_player_t> ObjectOccupants(rmm_object_t obj) override;
    bool SetObjectPosition(rmm_object_t obj, const rmm_vec3& pos) override;
    bool SetObjectRotation(rmm_object_t obj, const rmm_vec3& ypr) override;
    bool SetObjectVelocity(rmm_object_t obj, const rmm_vec3& linear, const rmm_vec3& angular) override;
    bool PushPlayer(rmm_player_t id, const rmm_vec3& velocity) override;
    // Terrain is a gentle slope: height = 10 + x / 100.
    std::vector<ObjectRef> WorldObjects() override;
    rmm_object_t SpawnObject(const std::string& tmpl, const rmm_vec3& pos, const rmm_vec3& ypr) override;
    bool RemoveObject(rmm_object_t obj) override;
    bool GroundHeight(float x, float z, float& height) override;
    bool TemplateExists(const std::string& tmpl) override;
    bool SetKit(rmm_player_t id, const std::string& kitTemplate) override;
    bool EndRoundNow(int32_t winner) override;
    // Seats: a vehicle root is seat 0, its .seat objects 1, 2... in the order added.
    bool PutInVehicle(rmm_player_t id, rmm_object_t vehicle, int32_t seat) override;
    bool TakeOutOfVehicle(rmm_player_t id) override;
    // Objects have 1000 hp; destroying a vehicle kills whoever is inside (weapon "explosion").
    bool DamageObject(rmm_object_t obj, float amount, rmm_player_t attacker) override;
    // Items per kit: .kititems, else knife 1, pistol 2, "<kit>_Gun" 3, grenade 4.
    bool GetItems(rmm_player_t id, std::vector<rmm_item_info>& out) override;
    bool SelectItem(rmm_player_t id, int32_t slot) override;
    bool RemoveItem(rmm_player_t id, int32_t slot) override;
    // Spawn timing is only stored (nobody respawns by themselves in the simulator).
    bool GetSpawnTiming(rmm_spawn_timing& out) override;
    bool SetSpawnTiming(const rmm_spawn_timing& timing) override;
    // Control points added with AddControlPoint; a locked or fixed one can't be captured.
    std::vector<rmm_object_t> ControlPoints() override;
    bool GetControlPoint(rmm_object_t cp, rmm_control_point_info& out) override;
    bool SetControlPoint(rmm_object_t cp, int32_t team) override;
    bool LockControlPoint(rmm_object_t cp, bool locked) override;

    // ---- simulated client actions (driven by rmm_simhost) -----------------------
    rmm_player_t Join(const std::string& name, int32_t team, bool bot, const std::string& ip);
    void Leave(rmm_player_t id);
    void Chat(rmm_player_t id, int32_t channel, const std::string& text);
    void Spawn(rmm_player_t id);
    void Kill(rmm_player_t victim, rmm_player_t attacker, const std::string& weapon);
    void Damage(rmm_player_t victim, rmm_player_t attacker, float amount);
    void Enter(rmm_player_t id, const std::string& vehicle, int32_t seat);
    void Exit(rmm_player_t id);
    void LoadMap(const std::string& map, const std::string& mode, int32_t size);
    void EndRound(int32_t winner);
    void Capture(const std::string& cp, int32_t team);
    /// A control point of every level loaded from now on; each map load gives it back
    /// to @p team and unlocks it. @p fixed = the map's unableToChangeTeam.
    void AddControlPoint(const std::string& name, const rmm_vec3& pos, int32_t team, bool fixed);
    void ChangeTeamAction(rmm_player_t id, int32_t team);
    /// Place a vehicle (root) or, with @p parent, a seat object under it.
    rmm_object_t AddVehicle(const std::string& tmpl, const rmm_vec3& pos, const rmm_vec3& vel, rmm_object_t parent = 0);
    /// Place a non-vehicle world object (building, prop...).
    rmm_object_t AddStatic(const std::string& tmpl, const rmm_vec3& pos);
    /// Make a template known to the engine (spawnable); @p networked = seen by clients;
    /// @p vehicle = spawn_object makes a vehicle (seat 0 only).
    void AddTemplate(const std::string& tmpl, bool networked, bool vehicle = false);
    void VehicleDamage(rmm_object_t obj, rmm_player_t attacker, float amount);
    /// Advance simple physics: root positions += velocity * seconds.
    void Drift(double seconds);
    /// A bot the game adds by itself (co-op fill), not through RMM.
    rmm_player_t AddEngineBot(const std::string& name, int32_t team);
    /// One engine AI input tick for a bot: @p engine is what the AI proposes.
    void BotInputTick(const std::string& name, const BotControl::Frame& engine);
    /// The player picks up the kit object @p kit lying on the ground (blockable
    /// KIT_PICKUP); their old kit is dropped where they stand.
    void PickupKit(rmm_player_t id, rmm_object_t kit);
    /// F1-F8 inside a vehicle: exit the current seat, enter @p seat.
    void SwitchSeat(rmm_player_t id, int32_t seat);
    /// The player's weapon key for @p slot (blockable WEAPON_SWITCH, reason PLAYER).
    void SwitchWeapon(rmm_player_t id, int32_t slot);
    int32_t HeldItem(rmm_player_t id) const;
    /// What a kit carries from now on (slot -> weapon template).
    void SetKitItems(const std::string& kit, const std::map<int32_t, std::string>& items);
    /// A map object spawner: spawns its vehicle now and whenever
    /// RunSpawners() finds the vehicle gone. Each spawn raises a blockable SPAWNER_SPAWN.
    void AddSpawner(const std::string& tmpl, const rmm_vec3& pos, float yaw, int32_t team);
    void RunSpawners();

private:
    struct Spawner {
        rmm_object_t handle = 0, object = 0;
        std::string tmpl;
        rmm_vec3 pos{};
        float yaw = 0;
        int32_t team = 0;
    };

    std::vector<Spawner> spawners_;
    rmm_spawn_timing spawnTiming_{sizeof(rmm_spawn_timing), 3.0f, RMM_TRUE, 20.0f};  // BF1942's defaults
    rmm_object_t SeatObject(rmm_object_t root, int32_t seat) const;
    rmm_player_t SeatTakenBy(rmm_object_t obj) const;
    bool DoEnter(rmm_player_t id, rmm_object_t obj, int32_t reason);
    bool DoExit(rmm_player_t id, int32_t reason);
    /// KIT_DROP for the player's current kit, then KIT_PICKUP for @p kit (the
    /// player's kit is already switched when POST handlers run).
    void SwapKit(rmm_player_t id, const std::string& kit, rmm_object_t kitObject, int32_t reason);
    void GiveKitItems(rmm_player_t id, const std::string& kit);
    bool DoSwitch(rmm_player_t id, int32_t slot, int32_t reason);
    std::map<std::string, std::map<int32_t, std::string>> kitItems_;  // lower-case kit -> slot -> weapon
    std::map<rmm_player_t, std::map<int32_t, std::string>> items_;
    std::map<rmm_player_t, int32_t> activeItem_;

    // ---- fake AI console ("simai.*", modelled on BF1942's ai.* - see gamedata/sim.ini) ----
    struct SimBot {
        int32_t id;
        rmm_player_t player;
        std::string name, kit, behaviour = "Idle";
        rmm_vec3 goal{};
    };

    bool AiRunning() const;  // co-op maps only, like BF1942
    bool AiExec(const std::string& cmd, const std::vector<std::string>& t, std::string* output, bool& ok);
    SimBot* BotById(int32_t id);
    std::vector<SimBot> bots_;
    int32_t nextBotId_ = 0;
    std::map<std::string, int> addAttempts_;  // createAIPlayer'd names -> ai.addBot calls so far
    float botSkill_ = 0.75f;
    std::string strategy_[3] = {"", "finalPush", "breakOut"};

    struct SimObject {
        std::string tmpl;
        rmm_object_t root = 0;  // == own handle for roots
        rmm_vec3 pos{}, rot{}, vel{}, ang{};
        float health = 1000.f;
        bool vehicle = true;  // listed by Vehicles(); false for statics/props
        bool networked = true;
        int32_t seat = -1;  // vehicles: 0 = root (driver), seats 1, 2...
    };

    std::map<std::string, bool> templates_;         // lower-case name -> networked
    std::map<std::string, bool> vehicleTemplates_;  // lower-case names spawn_object makes vehicles of
    void Remove(rmm_player_t id, const std::string& why);
    void SetStatus(int32_t s);
    rmm_object_t RootOf(rmm_object_t obj) const;
    SimObject* Obj(rmm_object_t h);

    std::map<rmm_object_t, SimObject> objects_;
    std::map<rmm_player_t, float> health_;  // living players' soldier hit points

    rmm_game emulate_;
    std::string name_;
    Core* core_ = nullptr;
    int32_t maxPlayers_ = 64;
    std::map<rmm_player_t, rmm_player_info> players_;
    std::map<std::string, std::string> vars_;  // lower-case engine var -> value
    std::vector<rmm_map_info> maplist_;
    int32_t nextMapIndex_ = 0;
    int32_t tickets_[3] = {0, 200, 200};
    std::map<std::string, int32_t> cpOwner_;  // .capture of names that aren't control points

    struct SimCp {
        rmm_object_t handle = 0;
        std::string name;
        rmm_vec3 pos{};
        int32_t team = 0, startTeam = 0;
        bool locked = false, fixed = false;
    };

    std::vector<SimCp> cps_;
    SimCp* Cp(rmm_object_t h);
    void CpChanged(SimCp& cp, int32_t old, const std::vector<rmm_player_t>& who);
    rmm_object_t nextObject_ = 1;
};

}  // namespace rmm
