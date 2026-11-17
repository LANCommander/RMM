#include "r2_adapter.h"

#include "../../core/core.h"
#include "../../core/platform.h"
#include "../common/hooks.h"

#include <cstdlib>

#ifdef _WIN32
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    include <windows.h>
#else
#    include <dlfcn.h>
#endif

namespace rmm {

R2Adapter* R2Adapter::instance_ = nullptr;

// Py_Initialize interception.
#ifdef _WIN32
// Windows: installed over bf2_w32ded.exe's dice_py.dll import (see Attach).
static void (*g_realPyInitialize)() = nullptr;

static void HookedPyInitialize() {
    g_realPyInitialize();

    if (R2Adapter* a = R2Adapter::Instance())
        a->OnPythonInitialized();
}
#endif

}  // namespace rmm

#ifndef _WIN32
// Linux: librmm.so is LD_PRELOADed, so this definition interposes the one in
// the game's Python library. Forward to the real one, then register _rmm.
extern "C" RMM_DLLEXPORT void Py_Initialize() {
    using Fn = void (*)();
    static Fn real = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "Py_Initialize"));

    if (real)
        real();

    if (rmm::R2Adapter* a = rmm::R2Adapter::Instance())
        a->OnPythonInitialized();
}
#endif

namespace rmm {

R2Adapter::R2Adapter(rmm_game game) : game_(game) { shortName_ = game == RMM_GAME_BF2142 ? "bf2142" : "bf2"; }

const char* R2Adapter::GameName() const { return game_ == RMM_GAME_BF2142 ? "Battlefield 2142" : "Battlefield 2"; }

bool R2Adapter::Attach(Core& core, std::string& err) {
    core_ = &core;
    instance_ = this;

    if (!LoadGameData(core.BuildPath(RMM_PATH_GAMEDATA, ""), err))
        return false;
#ifndef _WIN32
    // Optional native hooks (damage); independent of the Python bridge.
    std::string nativeErr;

    if (!AttachNative(nativeErr) && !nativeErr.empty())
        core.Log().Warn("%s: %s", shortName_.c_str(), nativeErr.c_str());
#endif

#ifdef _WIN32
    std::string pyDll = data_.Get("python", "module_windows", "dice_py.dll");
    pyModule_ = GetModuleHandleA(pyDll.c_str());

    if (!pyModule_)
        pyModule_ = LoadLibraryA(pyDll.c_str());
#else
    std::string pyDll = data_.Get("python", "module_linux", "libdice_py.so");
    pyModule_ = dlopen(pyDll.c_str(), RTLD_NOW | RTLD_NOLOAD);

    if (!pyModule_)
        pyModule_ = dlopen(nullptr, RTLD_NOW);  // statically linked into the binary
#endif
    if (!pyModule_) {
        err = "Python runtime '" + pyDll + "' not found";

        return false;
    }

    std::string missing;

    if (!py::Get().Resolve(pyModule_, missing)) {
        err = "Python runtime is missing exports: " + missing;

        return false;
    }

    if (py::Get().Py_IsInitialized()) {
        OnPythonInitialized();

        return true;
    }
#ifdef _WIN32
    void* orig = nullptr;

    if (!hooks::PatchImport(nullptr, pyDll.c_str(), "Py_Initialize", reinterpret_cast<void*>(&HookedPyInitialize),
                            &orig)) {
        err = "could not hook " + pyDll + "!Py_Initialize in the server executable's import table";

        return false;
    }

    g_realPyInitialize = reinterpret_cast<void (*)()>(orig);
#endif
    core.Log().Info("Waiting for the game to initialise Python...");

    return true;
}

void R2Adapter::Detach() {
    if (bridge_)
        py::Get().DecRef(bridge_);

    bridge_ = nullptr;
    bridgeReady_ = false;
    instance_ = nullptr;
}

void R2Adapter::OnPythonInitialized() {
    auto& py = py::Get();

    if (!RegisterRmmPythonModule()) {
        core_->Log().Error("Failed to register the _rmm Python module");

        return;
    }

    // Make rmm/python importable so admin scripts can `import rmm_bridge`.
    std::string pyDir = core_->BuildPath(RMM_PATH_ROOT, "python");

    for (auto& c : pyDir)
        if (c == '\\')
            c = '/';

    std::string code = "import sys\nif '" + pyDir + "' not in sys.path: sys.path.insert(0, '" + pyDir + "')\n";
    py.PyRun_SimpleString(code.c_str());
    core_->Log().Info("_rmm module registered; waiting for rmm_bridge.init() from the admin script");
}

void R2Adapter::BridgeReady() {
    if (bridgeReady_)
        return;

    auto& py = py::Get();
    bridge_ = py.PyImport_ImportModule("rmm_bridge");

    if (!bridge_) {
        py.PyErr_Print();
        core_->Log().Error("rmm_bridge could not be imported");

        return;
    }

    bridgeReady_ = true;
    core_->OnGameReady();
}

py::PyObject* R2Adapter::CallBridge(const char* fn, py::PyObject* args) {
    auto& py = py::Get();

    if (!bridgeReady_ || !bridge_) {
        py.DecRef(args);

        return nullptr;
    }

    py::PyObject* callable = py.PyObject_GetAttrString(bridge_, fn);

    if (!callable) {
        py.PyErr_Clear();
        py.DecRef(args);

        return nullptr;
    }

    py::PyObject* res = py.PyObject_CallObject(callable, args);
    py.DecRef(callable);
    py.DecRef(args);

    if (!res) {
        py.PyErr_Print();

        return nullptr;
    }

    return res;
}

bool R2Adapter::CallBridgeBool(const char* fn, py::PyObject* args) {
    auto& py = py::Get();
    py::PyObject* r = CallBridge(fn, args);

    if (!r)
        return false;

    long v = r == py.None ? 0 : py.PyInt_AsLong(r);

    if (py.PyErr_Occurred()) {
        py.PyErr_Clear();
        v = 0;
    }

    py.DecRef(r);

    return v != 0;
}

uint32_t R2Adapter::EventFlags(int32_t ev) const {
    // Events come from Python host handlers, which fire after the fact.
    switch (ev) {
        case RMM_EV_MAP_LOAD:
        case RMM_EV_ROUND_START:
        case RMM_EV_ROUND_END:
        case RMM_EV_GAME_STATUS:
        case RMM_EV_PLAYER_CONNECT:
        case RMM_EV_PLAYER_AUTH:
        case RMM_EV_PLAYER_DISCONNECT:
        case RMM_EV_PLAYER_SPAWN:
        case RMM_EV_PLAYER_KILLED:
        case RMM_EV_PLAYER_TEAM:
        case RMM_EV_PLAYER_SQUAD:
        case RMM_EV_PLAYER_SCORE:
        case RMM_EV_PLAYER_KIT_PICKUP:
        case RMM_EV_PLAYER_KIT_DROP:
        case RMM_EV_PLAYER_REVIVED:
        case RMM_EV_CHAT:
        case RMM_EV_VEHICLE_ENTER:
        case RMM_EV_VEHICLE_EXIT:
        case RMM_EV_VEHICLE_DESTROYED:
        case RMM_EV_CONTROL_POINT:
            return RMM_EVF_SUPPORTED;
        case RMM_EV_RCON_COMMAND:
            return RMM_EVF_SUPPORTED | RMM_EVF_BLOCKABLE;  // the bridge decides whether to pass it on
        case RMM_EV_PLAYER_DAMAGE:
        case RMM_EV_VEHICLE_DAMAGE:
            // Not in the Python API; only from the native hook (Linux).
            return nativeDamage_ ? RMM_EVF_SUPPORTED | RMM_EVF_BLOCKABLE | RMM_EVF_MUTABLE : 0;
        default:
            return 0;
    }
}

int R2Adapter::CapOverride(int32_t cap) const {
    switch (cap) {
        case RMM_CAP_CONSOLE_EXEC:
        case RMM_CAP_CONSOLE_OUTPUT:
        case RMM_CAP_CONSOLE_HOOK:
        case RMM_CAP_KILL_PLAYER:
        case RMM_CAP_SET_TEAM:
        case RMM_CAP_SET_SCORE:
        case RMM_CAP_TELEPORT:
        case RMM_CAP_PLAYER_POSITION:
        case RMM_CAP_AUTH_ID:
        case RMM_CAP_CHANGE_MAP:
        case RMM_CAP_TICKETS:
        case RMM_CAP_DAMAGE_PLAYER:
        case RMM_CAP_PLAYER_HEALTH:
        case RMM_CAP_WORLD_OBJECTS:
        case RMM_CAP_OBJECT_MOVE:
        case RMM_CAP_OBJECT_ROTATION:
            return bridgeReady_ ? 1 : 0;
        case RMM_CAP_OBJECT_VELOCITY:
            return 0;  // the BF2 Python API exposes no velocity
        default:
            return -1;
    }
}

static std::vector<rmm_player_t> ParseIds(const char* s) {
    std::vector<rmm_player_t> ids;

    if (s)
        for (auto& tok : SplitArgs(s))
            ids.push_back(std::atoi(tok.c_str()));

    return ids;
}

bool R2Adapter::DamagePlayer(rmm_player_t id, float amount, rmm_player_t attacker) {
    return CallBridgeBool("damage_player", py::Get().Py_BuildValue("(idi)", id, double(amount), attacker));
}

bool R2Adapter::PlayerHealth(rmm_player_t id, float& health, float& maxHealth) {
    auto& py = py::Get();
    py::PyObject* r = CallBridge("player_health", py.Py_BuildValue("(i)", id));

    if (!r)
        return false;

    bool ok = r != py.None && py.PyArg_ParseTuple(r, "ff", &health, &maxHealth);

    if (!ok)
        py.PyErr_Clear();

    py.DecRef(r);

    return ok;
}

std::vector<IGameAdapter::VehicleRef> R2Adapter::Vehicles() {
    auto& py = py::Get();
    std::vector<VehicleRef> v;
    py::PyObject* r = CallBridge("vehicles", py.Py_BuildValue("()"));

    if (!r)
        return v;

    if (const char* s = py.PyString_AsString(r)) {
        for (auto& tok : SplitArgs(s)) {  // handle|template|isroot
            size_t a = tok.find('|'), b = tok.rfind('|');

            if (a == std::string::npos || b == a)
                continue;

            v.push_back(VehicleRef{BridgeObject(static_cast<long>(std::strtoll(tok.c_str(), nullptr, 10))),
                                   tok.substr(a + 1, b - a - 1), tok.substr(b + 1) == "1"});
        }
    } else {
        py.PyErr_Clear();
    }

    py.DecRef(r);

    return v;
}

bool R2Adapter::ObjectValid(rmm_object_t obj) {
    long h;

    return BridgeLong(obj, h) && CallBridgeBool("object_valid", py::Get().Py_BuildValue("(l)", h));
}

bool R2Adapter::GetObjectInfo(rmm_object_t obj, rmm_object_info& out) {
    auto& py = py::Get();
    long h;

    if (!BridgeLong(obj, h))
        return false;

    py::PyObject* r = CallBridge("object_info", py.Py_BuildValue("(l)", h));

    if (!r)
        return false;

    if (r == py.None) {
        py.DecRef(r);

        return false;
    }

    const char *tmpl = "", *rootTmpl = "";
    long root = 0;
    int occupants = 0, isRoot = 0;
    float x = 0, y = 0, z = 0, rx = 0, ry = 0, rz = 0, health = -1;

    // Tuple layout is defined by rmm_bridge.object_info().
    if (!py.PyArg_ParseTuple(r, "slsfffffffii", &tmpl, &root, &rootTmpl, &x, &y, &z, &rx, &ry, &rz, &health, &occupants,
                             &isRoot)) {
        py.PyErr_Print();
        py.DecRef(r);

        return false;
    }

    out.object = obj;
    out.root = root ? BridgeObject(root) : obj;
    CopyOut(tmpl, out.template_name, sizeof out.template_name);
    CopyOut(rootTmpl, out.root_template, sizeof out.root_template);
    out.position = rmm_vec3{x, y, z};
    out.rotation = rmm_vec3{rx, ry, rz};
    out.health = health;
    out.max_health = -1;
    out.occupant_count = occupants;
    out.is_vehicle = RMM_TRUE;  // the bridge only hands out handles for vehicles and control points
    out.is_root = isRoot ? RMM_TRUE : RMM_FALSE;
    out.is_destroyed = health == 0.0f ? RMM_TRUE : RMM_FALSE;
    py.DecRef(r);  // strings were copied above

    return true;
}

std::vector<rmm_player_t> R2Adapter::ObjectOccupants(rmm_object_t obj) {
    auto& py = py::Get();
    long h;

    if (!BridgeLong(obj, h))
        return {};

    py::PyObject* r = CallBridge("object_occupants", py.Py_BuildValue("(l)", h));

    if (!r)
        return {};

    const char* s = py.PyString_AsString(r);

    if (!s)
        py.PyErr_Clear();

    auto ids = ParseIds(s);
    py.DecRef(r);

    return ids;
}

bool R2Adapter::SetObjectPosition(rmm_object_t obj, const rmm_vec3& p) {
    long h;

    return BridgeLong(obj, h) &&
           CallBridgeBool("set_object_position",
                          py::Get().Py_BuildValue("(lddd)", h, double(p.x), double(p.y), double(p.z)));
}

bool R2Adapter::SetObjectRotation(rmm_object_t obj, const rmm_vec3& r) {
    long h;

    return BridgeLong(obj, h) &&
           CallBridgeBool("set_object_rotation",
                          py::Get().Py_BuildValue("(lddd)", h, double(r.x), double(r.y), double(r.z)));
}

bool R2Adapter::Exec(const std::string& command, std::string* output) {
    auto& py = py::Get();
    py::PyObject* r = CallBridge("exec", py.Py_BuildValue("(s)", command.c_str()));

    if (!r)
        return false;

    if (output && r != py.None) {
        const char* s = py.PyString_AsString(r);

        if (s)
            *output += s;
        else
            py.PyErr_Clear();
    }

    py.DecRef(r);

    return true;
}

void R2Adapter::ConsolePrint(const std::string& line) {
    auto& py = py::Get();

    if (bridgeReady_) {
        py::PyObject* r = CallBridge("log", py.Py_BuildValue("(s)", line.c_str()));

        if (r) {
            py.DecRef(r);

            return;
        }
    }

    plat::DebugOut(line);
}

int32_t R2Adapter::MaxPlayers() {
    auto& py = py::Get();
    py::PyObject* r = CallBridge("max_players", py.Py_BuildValue("()"));

    if (!r)
        return 64;

    long v = py.PyInt_AsLong(r);
    py.DecRef(r);

    return v > 0 ? static_cast<int32_t>(v) : 64;
}

std::vector<rmm_player_t> R2Adapter::PlayerIds() {
    auto& py = py::Get();
    std::vector<rmm_player_t> ids;
    py::PyObject* r = CallBridge("player_ids", py.Py_BuildValue("()"));

    if (!r)
        return ids;

    if (const char* s = py.PyString_AsString(r)) {
        for (auto& tok : SplitArgs(std::string(s)))
            ids.push_back(std::atoi(tok.c_str()));
    } else {
        py.PyErr_Clear();
    }

    py.DecRef(r);

    return ids;
}

bool R2Adapter::GetPlayer(rmm_player_t id, rmm_player_info& out) {
    auto& py = py::Get();
    py::PyObject* r = CallBridge("player_info", py.Py_BuildValue("(i)", id));

    if (!r)
        return false;

    if (r == py.None) {
        py.DecRef(r);

        return false;
    }

    const char *name = "", *addr = "", *auth = "", *kit = "", *veh = "";
    int team = 0, squad = 0, score = 0, kills = 0, deaths = 0, ping = 0, bot = 0, alive = 0, cmdr = 0, sl = 0;
    long vehId = 0;
    float x = 0, y = 0, z = 0;
    // Tuple layout is defined by rmm_bridge.player_info().
    int ok = py.PyArg_ParseTuple(r, "sssiiiiiiiiiislsfff", &name, &addr, &auth, &team, &squad, &score, &kills, &deaths,
                                 &ping, &bot, &alive, &cmdr, &sl, &kit, &vehId, &veh, &x, &y, &z);

    if (!ok) {
        py.PyErr_Print();
        py.DecRef(r);

        return false;
    }

    uint32_t size = out.struct_size;
    out = rmm_player_info{};
    out.struct_size = size;
    out.id = id;
    CopyOut(name, out.name, sizeof out.name);
    CopyOut(addr, out.address, sizeof out.address);
    CopyOut(auth, out.auth_id, sizeof out.auth_id);
    out.team = team;
    out.squad = squad;
    out.score = score;
    out.kills = kills;
    out.deaths = deaths;
    out.ping = ping;
    out.is_bot = bot;
    out.is_alive = alive;
    out.is_commander = cmdr;
    out.is_squad_leader = sl;
    CopyOut(kit, out.kit, sizeof out.kit);
    out.vehicle = vehId ? BridgeObject(vehId) : 0;
    CopyOut(veh, out.vehicle_template, sizeof out.vehicle_template);
    out.position = rmm_vec3{x, y, z};
    out.seat = -1;  // the bridge doesn't report seats
    py.DecRef(r);   // strings were copied above

    return true;
}

bool R2Adapter::KillPlayer(rmm_player_t id) {
    return CallBridgeBool("kill_player", py::Get().Py_BuildValue("(i)", id));
}

bool R2Adapter::SetTeam(rmm_player_t id, int32_t team) {
    return CallBridgeBool("set_team", py::Get().Py_BuildValue("(ii)", id, team));
}

bool R2Adapter::SetScore(rmm_player_t id, int32_t score) {
    return CallBridgeBool("set_score", py::Get().Py_BuildValue("(ii)", id, score));
}

bool R2Adapter::Teleport(rmm_player_t id, const rmm_vec3& p) {
    return CallBridgeBool("teleport", py::Get().Py_BuildValue("(iddd)", id, double(p.x), double(p.y), double(p.z)));
}

// BF2/2142 have no single "change map now" command: the bridge finds/appends
// the map in the maplist, sets it as next level and runs it.
bool R2Adapter::ChangeMap(const std::string& map, const std::string& mode, int32_t size) {
    std::string m = mode.empty() ? std::string(currentMap_.mode) : mode;
    int32_t s = size > 0 ? size : (currentMap_.size > 0 ? currentMap_.size : 64);

    return CallBridgeBool("change_map", py::Get().Py_BuildValue("(ssi)", map.c_str(), m.c_str(), s));
}

int32_t R2Adapter::GetTickets(int32_t team) {
    auto& py = py::Get();
    py::PyObject* r = CallBridge("get_tickets", py.Py_BuildValue("(i)", team));

    if (!r)
        return -1;

    long v = py.PyInt_AsLong(r);

    if (py.PyErr_Occurred()) {
        py.PyErr_Clear();
        v = -1;
    }

    py.DecRef(r);

    return static_cast<int32_t>(v);
}

bool R2Adapter::SetTickets(int32_t team, int32_t tickets) {
    return CallBridgeBool("set_tickets", py::Get().Py_BuildValue("(ii)", team, tickets));
}

}  // namespace rmm
