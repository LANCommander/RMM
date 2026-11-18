// Refractor 1 (Battlefield 1942, Battlefield Vietnam) adapter - "native".
//
// No scripting layer exists, so everything comes from hooks on engine code.
// Two ways to find that code, chosen per platform (r1_native.cpp is shared):
//
//   Linux   - bf1942_lnxded ships with its full ELF symbol table, so gamedata
//             [linux] names functions/globals by mangled C++ name and they are
//             resolved from /proc/self/exe at startup. Struct offsets come from
//             [linux_offsets]. See docs/re/bf1942-linux.md.
//   Windows - BF1942_w32ded.exe has no symbols; gamedata [windows] gives the
//             addresses for the 1.61 build (checked by PE timestamp) and
//             [windows_offsets] the VC++ layouts. See docs/re/bf1942-windows.md.
//
// Every hook is independent. A missing optional symbol only disables the
// matching event (EventFlags reports it); a missing required one makes Attach
// fail cleanly so the server keeps running without RMM.
#pragma once

#include "../common/adapter_base.h"

#include <map>
#include <set>
#include <string>

namespace rmm {

class R1Adapter : public AdapterBase {
public:
    explicit R1Adapter(rmm_game game);

    rmm_game GameId() const override { return game_; }

    const char* GameName() const override;

    const char* Backend() const override { return "native"; }

    bool Attach(Core& core, std::string& err) override;
    void Detach() override;

    uint32_t EventFlags(int32_t ev) const override;
    int CapOverride(int32_t cap) const override;

    bool Exec(const std::string& command, std::string* output) override;
    void ConsolePrint(const std::string& line) override;

    int32_t MaxPlayers() override;
    std::vector<rmm_player_t> PlayerIds() override;
    bool GetPlayer(rmm_player_t id, rmm_player_info& out) override;
    bool SayAll(const std::string& msg) override;

    /// GameServer::say/sayClient carry at most 63 characters (ChatMessage text[64]).
    size_t ChatLineLimit() const override { return 63; }

    bool PrivateMessage(rmm_player_t id, const std::string& msg) override;
    bool KillPlayer(rmm_player_t id) override;

    // Objects and physics (ObjectManager + physics nodes)
    bool DamagePlayer(rmm_player_t id, float amount, rmm_player_t attacker) override;
    bool PlayerHealth(rmm_player_t id, float& health, float& maxHealth) override;
    std::vector<VehicleRef> Vehicles() override;
    bool ObjectValid(rmm_object_t obj) override;
    bool GetObjectInfo(rmm_object_t obj, rmm_object_info& out) override;
    std::vector<rmm_player_t> ObjectOccupants(rmm_object_t obj) override;
    bool SetObjectPosition(rmm_object_t obj, const rmm_vec3& pos) override;
    bool SetObjectVelocity(rmm_object_t obj, const rmm_vec3& linear, const rmm_vec3& angular) override;
    bool PushPlayer(rmm_player_t id, const rmm_vec3& velocity) override;
    // Moves the soldier (or the vehicle they're in) directly; the gamedata has
    // no teleport console command for BF1942.
    bool Teleport(rmm_player_t id, const rmm_vec3& pos) override;
    bool SetObjectRotation(rmm_object_t obj, const rmm_vec3& yawPitchRoll) override;
    // World (Linux: ObjectManager grid maps, GameServer spawn/destroy, terrain)
    std::vector<ObjectRef> WorldObjects() override;
    rmm_object_t SpawnObject(const std::string& tmpl, const rmm_vec3& pos, const rmm_vec3& yawPitchRoll) override;
    bool RemoveObject(rmm_object_t obj) override;
    bool GroundHeight(float x, float z, float& height) override;
    bool TemplateExists(const std::string& tmpl) override;
    // Bots (Linux): native removal = the engine's own bot teardown.
    // Input control is a hook on AIPlayer::addInput feeding Core::BotInput().
    bool BotRemove(const BotRef& bot) override;
    /// Linux: from the player list + BotManager's player id -> bot id table.
    /// ai.getAllBots segfaults once a removed bot's brain sits detached.
    std::vector<BotRef> Bots() override;
    // Linux: the engine's own kit hand-out (createObjectOnAllClients +
    // handlePickup), round end (ScoreManager winner + setGameStatus), and the
    // replicated score / ticket fields.
    bool SetKit(rmm_player_t id, const std::string& kitTemplate) override;
    bool EndRoundNow(int32_t winner) override;
    bool SetScore(rmm_player_t id, int32_t score) override;
    int32_t GetTickets(int32_t team) override;
    bool SetTickets(int32_t team, int32_t tickets) override;
    // Linux: GameServer::enterVehicle / exitVehicle, as a seat switch does.
    bool PutInVehicle(rmm_player_t id, rmm_object_t vehicle, int32_t seat) override;
    bool TakeOutOfVehicle(rmm_player_t id) override;
    // Linux: GameServer::_giveDamage on the root, through the damage hook.
    bool DamageObject(rmm_object_t obj, float amount, rmm_player_t attacker) override;
    bool GetItems(rmm_player_t id, std::vector<rmm_item_info>& out) override;
    bool SelectItem(rmm_player_t id, int32_t slot) override;
    bool RemoveItem(rmm_player_t id, int32_t slot) override;
    // Linux: GameServer spawn fields
    bool GetSpawnTiming(rmm_spawn_timing& out) override;
    bool SetSpawnTiming(const rmm_spawn_timing& timing) override;
    // ControlPoint objects in the grid; set = lostControl + gotControl; lock = skip handleFrameUpdate
    std::vector<rmm_object_t> ControlPoints() override;
    bool GetControlPoint(rmm_object_t cp, rmm_control_point_info& out) override;
    bool SetControlPoint(rmm_object_t cp, int32_t team) override;
    bool LockControlPoint(rmm_object_t cp, bool locked) override;

    static R1Adapter* Instance() { return instance_; }

    Core* CorePtr() { return core_; }

    /// Called from the per-frame engine hook (both platforms).
    void OnEngineFrame();
    /// Called from the game-status hooks (Linux): dispatches GAME_STATUS and
    /// ROUND_START/ROUND_END on a change.
    void OnGameStatus(int32_t status);

private:
    // r1_native.cpp (Linux: ELF symbols; Windows: addresses for one build)
    static const char* Plat();  // gamedata section: "linux" / "windows"
    bool AttachNative(std::string& err);
    bool NativeExec(const std::string& command, std::string* output);
    void NativeFrame();  // player polling + deferred events
    bool NativeConsolePrint(const std::string& line);
    uintptr_t mainConsoleGlobal_ = 0, oldConsoleVptr_ = 0;
    void PollPlayers();
    bool ReadPlayerAt(uintptr_t player, rmm_player_info& out);
    std::vector<uintptr_t> WalkPlayerList();
    uintptr_t Sym(const std::string& key, bool required, std::string& err);
    uint32_t Off(const std::string& key, uint32_t def) const;
    std::string PlayerAddress(rmm_player_t id);
    bool NativeBotRemoveReady() const;
    bool RemoveBotNow(const BotRef& bot, bool quiet = false);
    void ProcessPendingBotRemovals();

    struct PendingBotRemoval {
        BotRef bot;
        double deadline;
    };

    std::vector<PendingBotRemoval> pendingBotRemovals_;  // dead bots, removed once they respawn
    int32_t pendingEndRound_ = -1;  // end_round winner, applied at the next engine frame (-1 = none)
    void ProcessPendingEndRound();
    bool ApplyKit(rmm_player_t id, const std::string& kitTemplate);  // SetKit, applied immediately
    void FlushPendingKits();  // set_kit / enter/exit_vehicle made inside damage code
    bool ApplyPutIn(rmm_player_t id, rmm_object_t vehicle, int32_t seat);
    bool ApplyTakeOut(rmm_player_t id);

    struct Offsets {
        uint32_t list = 0x0c, name = 0x44, vehicle = 0x4c, id = 0x58, soldier = 0x68, isAI = 0x78, team = 0x7c,
                 vptr = 8;
    } off_;

    uintptr_t playerManagerGlobal_ = 0;  // address of the PlayerManager* global
    uintptr_t bfPlayerVptr_ = 0;         // expected vptr of BFPlayer objects
    bool listBroken_ = false;

    // Players seen by the last poll, for connect/disconnect/team diffing.
    std::map<rmm_player_t, rmm_player_info> known_;
    rmm_player_t leaving_ = RMM_PLAYER_NONE;   // player being reported as disconnected
    std::set<rmm_player_t> authed_;            // players RMM_EV_PLAYER_AUTH was fired for
    std::set<rmm_player_t> keyedAuth_;         // ... with a CD-key hash available
    std::map<rmm_player_t, double> joinedAt_;  // core time each player was first seen
    bool authHooked_ = false;
    bool destroyHooked_ = false;  // GameServer::destroyPlayer detoured
    double authTimeout_ = 20;     // [game] auth_timeout: seconds to wait for the CD-key callback
    double nextPoll_ = 0;

    // Map tracking (Setup's current CampaignEntry)
    void CheckMap();
    int32_t lastGpm_ = -1;
    std::string lastMod_;
    bool outputHooked_ = false;  // OldConsole::output detoured (engine output capture)

    // Common
    rmm_game game_;
    Core* core_ = nullptr;
    std::map<int32_t, uint32_t> eventFlags_;  // events backed by an installed hook
    bool consoleHooked_ = false;
    bool queryHooked_ = false;  // the server's sendto rewrites query replies (queryhook)
    static R1Adapter* instance_;
};

}  // namespace rmm
