// The `_rmm` builtin Python module: the entry points rmm_bridge.py calls to
// feed game events into the RMM core. Argument formats here and the calls in
// python/rmm/rmm_bridge.py must be kept in sync.
#include "r2_adapter.h"

#include "../../core/core.h"

namespace rmm {
namespace {

using py::PyObject;

py::Api& P() { return py::Get(); }

Core& C() { return Core::Get(); }

PyObject* None() { return P().ReturnNone(); }

PyObject* M_ready(PyObject*, PyObject*) {
    if (R2Adapter* a = R2Adapter::Instance())
        a->BridgeReady();

    return None();
}

PyObject* M_frame(PyObject*, PyObject*) {
    C().Frame();

    return None();
}

PyObject* M_player_connect(PyObject*, PyObject* args) {
    int pid;
    const char* name;
    const char* addr;

    if (!P().PyArg_ParseTuple(args, "iss", &pid, &name, &addr))
        return nullptr;

    std::string reject;
    // Not blockable on R2: the core kicks banned players itself.
    C().OnPlayerConnect(pid, name, addr, reject);

    return None();
}

PyObject* M_player_auth(PyObject*, PyObject* args) {
    int pid;

    if (!P().PyArg_ParseTuple(args, "i", &pid))
        return nullptr;

    C().OnPlayerAuth(pid);

    return None();
}

PyObject* M_player_disconnect(PyObject*, PyObject* args) {
    int pid;

    if (!P().PyArg_ParseTuple(args, "i", &pid))
        return nullptr;

    C().OnPlayerDisconnect(pid);

    return None();
}

PyObject* M_player_spawn(PyObject*, PyObject* args) {
    int pid;
    const char* kit;
    float x, y, z;

    if (!P().PyArg_ParseTuple(args, "isfff", &pid, &kit, &x, &y, &z))
        return nullptr;

    rmm_ev_player_spawn ev{pid, rmm_vec3{x, y, z}, kit};
    C().Dispatch(RMM_EV_PLAYER_SPAWN, &ev);

    return None();
}

PyObject* M_player_killed(PyObject*, PyObject* args) {
    int victim, attacker, tk;
    const char* weapon;
    const char* vehicle;

    if (!P().PyArg_ParseTuple(args, "iissi", &victim, &attacker, &weapon, &vehicle, &tk))
        return nullptr;

    rmm_ev_player_killed ev{
        victim, attacker, weapon, vehicle, tk ? RMM_TRUE : RMM_FALSE, attacker == victim ? RMM_TRUE : RMM_FALSE};
    C().Dispatch(RMM_EV_PLAYER_KILLED, &ev);

    return None();
}

PyObject* M_player_revived(PyObject*, PyObject* args) {
    int pid, medic;

    if (!P().PyArg_ParseTuple(args, "ii", &pid, &medic))
        return nullptr;

    rmm_ev_player_killed ev{pid, medic, "", "", RMM_FALSE, RMM_FALSE};
    C().Dispatch(RMM_EV_PLAYER_REVIVED, &ev);

    return None();
}

PyObject* M_player_change(PyObject*, PyObject* args) {
    int ev_id, pid, oldv, newv;

    if (!P().PyArg_ParseTuple(args, "iiii", &ev_id, &pid, &oldv, &newv))
        return nullptr;

    if (ev_id != RMM_EV_PLAYER_TEAM && ev_id != RMM_EV_PLAYER_SQUAD)
        return None();

    rmm_ev_player_change ev{pid, oldv, newv};
    C().Dispatch(ev_id, &ev);

    return None();
}

PyObject* M_player_score(PyObject*, PyObject* args) {
    int pid, delta, total;

    if (!P().PyArg_ParseTuple(args, "iii", &pid, &delta, &total))
        return nullptr;

    rmm_ev_player_score ev{pid, delta, total};
    C().Dispatch(RMM_EV_PLAYER_SCORE, &ev);

    return None();
}

PyObject* M_player_kit(PyObject*, PyObject* args) {
    int ev_id, pid;
    const char* kit;

    if (!P().PyArg_ParseTuple(args, "iis", &ev_id, &pid, &kit))
        return nullptr;

    if (ev_id != RMM_EV_PLAYER_KIT_PICKUP && ev_id != RMM_EV_PLAYER_KIT_DROP)
        return None();

    rmm_ev_player_kit ev{pid, kit, 0, RMM_KIT_REASON_PICKUP};  // BF2's Python events don't say why
    C().Dispatch(ev_id, &ev);

    return None();
}

PyObject* M_chat(PyObject*, PyObject* args) {
    int pid, channel;
    const char* text;

    if (!P().PyArg_ParseTuple(args, "iis", &pid, &channel, &text))
        return nullptr;

    std::string msg = text;
    C().OnChat(pid < 0 ? RMM_PLAYER_NONE : pid, channel, msg);  // runs chat commands too

    return None();
}

/// rcon(source, player, command) -> str output if RMM handled it, else None.
PyObject* M_rcon(PyObject*, PyObject* args) {
    int source, pid;
    const char* cmd;

    if (!P().PyArg_ParseTuple(args, "iis", &source, &pid, &cmd))
        return nullptr;

    std::string out;
    rmm_player_t player = pid < 0 ? RMM_PLAYER_NONE : pid;

    // In-game rcon users must be RMM admins to reach RMM; individual commands
    // still check their own flags.
    if (player != RMM_PLAYER_NONE && !C().Admin().HasFlags(player, RMM_ADMFLAG_GENERIC))
        return None();

    if (!C().OnConsoleCommand(source, player, cmd, &out))
        return None();

    return P().Py_BuildValue("s", out.c_str());
}

/// object_ptr(token) -> the engine IObject* inside a host.Object `token`
/// (a PyCObject), as a Python int, or None. The bridge uses it as the object
/// handle, so handles match the ones native hooks report.
PyObject* M_object_ptr(PyObject*, PyObject* args) {
    PyObject* token = nullptr;

    if (!P().PyArg_ParseTuple(args, "O", &token))
        return nullptr;

    void* ptr = P().CObjectPtr(token);

    if (!ptr)
        return None();

    return P().Py_BuildValue("l", static_cast<long>(reinterpret_cast<intptr_t>(ptr)));
}

/// vehicle(event, player, handle, template, seat, is_driver, root_handle, root_template)
PyObject* M_vehicle(PyObject*, PyObject* args) {
    int ev_id, pid, seat, driver;
    long vid, rootId;
    const char* tmpl;
    const char* rootTmpl;

    if (!P().PyArg_ParseTuple(args, "iilsiils", &ev_id, &pid, &vid, &tmpl, &seat, &driver, &rootId, &rootTmpl))
        return nullptr;

    if (ev_id != RMM_EV_VEHICLE_ENTER && ev_id != RMM_EV_VEHICLE_EXIT)
        return None();

    rmm_ev_vehicle ev{pid,
                      BridgeObject(vid),
                      tmpl,
                      seat,
                      driver ? RMM_TRUE : RMM_FALSE,
                      BridgeObject(rootId ? rootId : vid),
                      rootTmpl,
                      RMM_VEHICLE_REASON_PLAYER};  // BF2's Python events don't say why
    C().Dispatch(ev_id, &ev);

    return None();
}

PyObject* M_vehicle_destroyed(PyObject*, PyObject* args) {
    int attacker;
    long vid;
    const char* tmpl;

    if (!P().PyArg_ParseTuple(args, "lsi", &vid, &tmpl, &attacker))
        return nullptr;

    rmm_ev_vehicle ev{attacker,  BridgeObject(vid), tmpl, -1,
                      RMM_FALSE, BridgeObject(vid), tmpl, RMM_VEHICLE_REASON_GAME};
    C().Dispatch(RMM_EV_VEHICLE_DESTROYED, &ev);

    return None();
}

PyObject* M_control_point(PyObject*, PyObject* args) {
    int oldTeam, newTeam;
    long cpid;
    const char* name;

    if (!P().PyArg_ParseTuple(args, "lsii", &cpid, &name, &oldTeam, &newTeam))
        return nullptr;

    rmm_ev_control_point ev{BridgeObject(cpid), name, oldTeam, newTeam, 0, nullptr};  // capturers: TODO (bridge)
    C().Dispatch(RMM_EV_CONTROL_POINT, &ev);

    return None();
}

PyObject* M_map_load(PyObject*, PyObject* args) {
    const char *map, *mode, *mod;
    int size;

    if (!P().PyArg_ParseTuple(args, "sssi", &map, &mode, &mod, &size))
        return nullptr;

    if (R2Adapter* a = R2Adapter::Instance())
        a->NoteMapFromBridge(map, mode, size);

    C().OnMapLoad(map, mode, mod, size);

    return None();
}

PyObject* M_round_start(PyObject*, PyObject* args) {
    const char *map, *mode, *mod;
    int size;

    if (!P().PyArg_ParseTuple(args, "sssi", &map, &mode, &mod, &size))
        return nullptr;

    rmm_ev_map ev{map, mode, mod, size};
    C().Dispatch(RMM_EV_ROUND_START, &ev);

    return None();
}

PyObject* M_round_end(PyObject*, PyObject* args) {
    int winner, reason;

    if (!P().PyArg_ParseTuple(args, "ii", &winner, &reason))
        return nullptr;

    rmm_ev_round_end ev{winner, reason};
    C().Dispatch(RMM_EV_ROUND_END, &ev);

    return None();
}

PyObject* M_game_status(PyObject*, PyObject* args) {
    int oldS, newS;

    if (!P().PyArg_ParseTuple(args, "ii", &oldS, &newS))
        return nullptr;

    if (R2Adapter* a = R2Adapter::Instance())
        a->SetStatusFromBridge(newS);

    rmm_ev_game_status ev{oldS, newS};
    C().Dispatch(RMM_EV_GAME_STATUS, &ev);

    return None();
}

PyObject* M_log(PyObject*, PyObject* args) {
    const char* msg;

    if (!P().PyArg_ParseTuple(args, "s", &msg))
        return nullptr;

    C().Log().Write(RMM_LOG_INFO, "bridge", msg);

    return None();
}

// Event ids are exported as constants by the bridge itself (it mirrors
// rmm_events.h), so the table only needs functions.
py::PyMethodDef g_methods[] = {
    {"ready", M_ready, py::METH_VARARGS, "Bridge initialised; RMM loads plugins."},
    {"frame", M_frame, py::METH_VARARGS, "Server tick."},
    {"player_connect", M_player_connect, py::METH_VARARGS, nullptr},
    {"player_auth", M_player_auth, py::METH_VARARGS, nullptr},
    {"player_disconnect", M_player_disconnect, py::METH_VARARGS, nullptr},
    {"player_spawn", M_player_spawn, py::METH_VARARGS, nullptr},
    {"player_killed", M_player_killed, py::METH_VARARGS, nullptr},
    {"player_revived", M_player_revived, py::METH_VARARGS, nullptr},
    {"player_change", M_player_change, py::METH_VARARGS, nullptr},
    {"player_score", M_player_score, py::METH_VARARGS, nullptr},
    {"player_kit", M_player_kit, py::METH_VARARGS, nullptr},
    {"chat", M_chat, py::METH_VARARGS, nullptr},
    {"rcon", M_rcon, py::METH_VARARGS, nullptr},
    {"object_ptr", M_object_ptr, py::METH_VARARGS, "Engine pointer inside a host.Object token."},
    {"vehicle", M_vehicle, py::METH_VARARGS, nullptr},
    {"vehicle_destroyed", M_vehicle_destroyed, py::METH_VARARGS, nullptr},
    {"control_point", M_control_point, py::METH_VARARGS, nullptr},
    {"map_load", M_map_load, py::METH_VARARGS, nullptr},
    {"round_start", M_round_start, py::METH_VARARGS, nullptr},
    {"round_end", M_round_end, py::METH_VARARGS, nullptr},
    {"game_status", M_game_status, py::METH_VARARGS, nullptr},
    {"log", M_log, py::METH_VARARGS, nullptr},
    {nullptr, nullptr, 0, nullptr},
};

}  // namespace

bool RegisterRmmPythonModule() {
    auto& py = py::Get();

    if (!py.ok)
        return false;

    return py.Py_InitModule4("_rmm", g_methods, "Refractor MultiMod native bridge", nullptr,
                             py::PYTHON_API_VERSION_23) != nullptr;
}

}  // namespace rmm
