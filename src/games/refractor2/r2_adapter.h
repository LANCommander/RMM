// Refractor 2 (Battlefield 2, Battlefield 2142) adapter - "python-bridge".
//
// Both games embed Python 2.3 and expose an official server-side event API
// (host.registerHandler, bf2.playerManager, host.rcon_invoke). Rather than
// reverse-engineer what DICE already exposes, this backend:
//
//   1. Hooks Py_Initialize (IAT patch on Windows, symbol interposition on
//      Linux) and registers a builtin module `_rmm` right after it.
//   2. Ships python/rmm/rmm_bridge.py, imported from the server's admin
//      script, which registers every host handler and forwards events into
//      `_rmm.*`, and offers helpers (exec, player_info, kill...) that the
//      native side calls back into.
//
// Optional native hooks (r2_linux.cpp) cover what the Python API cannot do,
// such as the damage events.
#pragma once

#include "../common/adapter_base.h"
#include "pyapi.h"

namespace rmm {

class R2Adapter : public AdapterBase {
public:
    explicit R2Adapter(rmm_game game);

    rmm_game GameId() const override { return game_; }

    const char* GameName() const override;

    const char* Backend() const override { return "python-bridge"; }

    bool Attach(Core& core, std::string& err) override;
    void Detach() override;

    uint32_t EventFlags(int32_t ev) const override;
    int CapOverride(int32_t cap) const override;

    bool Exec(const std::string& command, std::string* output) override;
    void ConsolePrint(const std::string& line) override;

    int32_t MaxPlayers() override;
    std::vector<rmm_player_t> PlayerIds() override;
    bool GetPlayer(rmm_player_t id, rmm_player_info& out) override;
    bool KillPlayer(rmm_player_t id) override;
    bool SetTeam(rmm_player_t id, int32_t team) override;
    bool SetScore(rmm_player_t id, int32_t score) override;
    bool Teleport(rmm_player_t id, const rmm_vec3& pos) override;
    bool ChangeMap(const std::string& map, const std::string& mode, int32_t size) override;
    int32_t GetTickets(int32_t team) override;
    bool SetTickets(int32_t team, int32_t tickets) override;

    // Bridge helpers (no velocity access in the Python API).
    bool DamagePlayer(rmm_player_t id, float amount, rmm_player_t attacker) override;
    bool PlayerHealth(rmm_player_t id, float& health, float& maxHealth) override;
    std::vector<VehicleRef> Vehicles() override;
    bool ObjectValid(rmm_object_t obj) override;
    bool GetObjectInfo(rmm_object_t obj, rmm_object_info& out) override;
    std::vector<rmm_player_t> ObjectOccupants(rmm_object_t obj) override;
    bool SetObjectPosition(rmm_object_t obj, const rmm_vec3& pos) override;
    bool SetObjectRotation(rmm_object_t obj, const rmm_vec3& ypr) override;

    /// Optional native hooks on the Linux server (r2_linux.cpp): the damage
    /// event the Python API lacks. @return false (with @p err set on failure)
    /// if not installed; the bridge keeps working either way.
    bool AttachNative(std::string& err);

    // Called from the _rmm module (see r2_module.cpp).
    void BridgeReady();

    void SetStatusFromBridge(int32_t status) { status_ = status; }

    void NoteMapFromBridge(const std::string& map, const std::string& mode, int32_t size) { NoteMap(map, mode, size); }

    Core* CorePtr() { return core_; }

    /// Called after the real Py_Initialize.
    void OnPythonInitialized();

    static R2Adapter* Instance() { return instance_; }

private:
    /// Call rmm_bridge.<fn>(*args). Steals @p args (a tuple from
    /// Py_BuildValue("(...)")). Returns a new reference or nullptr on error.
    py::PyObject* CallBridge(const char* fn, py::PyObject* args);
    /// Same, but returns truthiness of an int result and drops the object.
    bool CallBridgeBool(const char* fn, py::PyObject* args);

    rmm_game game_;
    Core* core_ = nullptr;
    void* pyModule_ = nullptr;
    py::PyObject* bridge_ = nullptr;  // imported rmm_bridge module
    bool bridgeReady_ = false;
    bool nativeDamage_ = false;  // serverGiveDamage detoured (Linux)
    static R2Adapter* instance_;
};

/// Register the `_rmm` builtin module. r2_module.cpp
bool RegisterRmmPythonModule();

// Object handles cross the bridge as Python ints (C long): the engine IObject*
// read from the object's `token`, so they equal the pointers native hooks
// report. (A small counter is used if a token can't be read.)
inline rmm_object_t BridgeObject(long h) { return static_cast<rmm_object_t>(static_cast<uintptr_t>(h)); }

inline bool BridgeLong(rmm_object_t obj, long& out) {
    if (!obj || ((obj >> (sizeof(uintptr_t) * 8 - 1)) >> 1) != 0)
        return false;  // must fit a pointer

    out = static_cast<long>(static_cast<uintptr_t>(obj));

    return true;
}

}  // namespace rmm
