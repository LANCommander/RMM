// Refractor 2 adapter - optional native hooks on the Linux server.
//
// The Python bridge provides almost everything on BF2/2142, but the Python
// API has no damage event. The Linux server (bin/ia-32/bf2, bin/amd-64/bf2)
// ships with its full ELF symbol table, so, as on BF1942, engine functions
// are resolved by mangled name from gamedata [linux]. Evidence for every
// symbol, slot and offset is in docs/re/bf2-linux.md.
//
// Portability between the two builds: function names are identical, and
// virtual calls use slot *indices* ([linux_slots]) multiplied by the pointer
// size. The only raw field offset (an object's template) is per build:
// [linux_offsets] (ia-32) / [linux64_offsets] (amd-64).
//
// RMM_LINUX_SYNTAX_CHECK: compiled (never linked) on Windows to type-check.
#if !defined(_WIN32) || defined(RMM_LINUX_SYNTAX_CHECK)

#    include "r2_adapter.h"

#    include "../../core/core.h"
#    include "../common/elf_symbols.h"
#    include "../common/hooks.h"
#    include "../refractor1/gcc3_string.h"

#    include <cstdlib>
#    include <string>
#    include <vector>

namespace rmm {

namespace {

// void GameLogicDamage::serverGiveDamage(IObject* target, float damage, IPlayer* attacker,
//     std::list<WeakPtr<IPlayer>>* assists, IObject* attackerObject, int, IObject*, IObject*,
//     Vec3, Vec3, int, bool, bool, bool, bool)
// Static (no `this`). Called only by ServerGameLogic::giveDamage, after the
// attacker has been resolved and armour applied (calcDamage), so `damage` is
// final. Vec3 has a non-trivial copy, so both are passed by hidden reference.
using ServerGiveDamageFn = void (*)(void* target, float damage, void* attacker, void* assists, void* attackerObject,
                                    int a6, void* a7, void* a8, const void* v1, const void* v2, int a11, bool b12,
                                    bool b13, bool b14, bool b15);
// IObject* world::getRootParent(const IObject*)
using GetRootParentFn = void* (*)(const void* object);

ServerGiveDamageFn g_serverGiveDamage = nullptr;
GetRootParentFn g_getRootParent = nullptr;
uintptr_t g_playerManagerGlobal = 0;  // address of the IPlayerManager* global
uintptr_t g_playerManagerVptr = 0;    // expected vptr (0 = don't check)

constexpr uint32_t kIidPlayerControl = 0xc4c5;  // IID_IPlayerControlObject (same value as BF1942)

// Virtual slot indices (see gamedata [linux_slots]).
struct Slots {
    uint32_t queryInterface = 3;         // IBase::queryInterface(unsigned)
    uint32_t pcoGetPlayer = 4;           // IPlayerControlObject -> IPlayer*
    uint32_t templateGetName = 11;       // ObjectTemplate::getName() const -> const std::string&
    uint32_t playerGetVehicle = 18;      // Player::getVehicle() const
    uint32_t playerGetId = 21;           // Player::getId() const (== Python Player.index)
    uint32_t playerIsAlive = 27;         // Player::getIsAlive() const
    uint32_t playerDefaultVehicle = 41;  // Player::getDefaultVehicle() (the soldier)
    uint32_t playerGetTeam = 58;         // Player::getTeam() const
    uint32_t pmGetPlayerFromId = 8;      // PlayerManager::getPlayerFromId(int)
} g_slot;

uint32_t g_offTemplate = 0;  // Object -> IObjectTemplate*; 0 = unknown on this build
int g_maxPlayerId = 255;

template <class Fn> Fn Virtual(const void* object, uint32_t index) {
    uintptr_t vtable = 0, fn = 0;

    if (!object || !elf::SafeReadT(reinterpret_cast<uintptr_t>(object), vtable) ||
        !elf::SafeReadT(vtable + index * sizeof(void*), fn) || !fn)
        return nullptr;

    return reinterpret_cast<Fn>(fn);
}

// GCC returns bool in %al only.
inline bool LowByte(uintptr_t v) { return (v & 0xff) != 0; }

void* QueryInterface(void* object, uint32_t iid) {
    auto fn = Virtual<void* (*)(void*, unsigned)>(object, g_slot.queryInterface);

    return fn ? fn(object, iid) : nullptr;
}

void* PlayerManager() {
    uintptr_t pm = 0, vptr = 0;

    if (!g_playerManagerGlobal || !elf::SafeReadT(g_playerManagerGlobal, pm) || !pm)
        return nullptr;

    if (g_playerManagerVptr && (!elf::SafeReadT(pm, vptr) || vptr != g_playerManagerVptr))
        return nullptr;

    return reinterpret_cast<void*>(pm);
}

void* PlayerFromId(void* pm, int id) {
    auto fn = Virtual<void* (*)(void*, int)>(pm, g_slot.pmGetPlayerFromId);

    return fn ? fn(pm, id) : nullptr;
}

int PlayerId(void* player) {
    auto fn = Virtual<int (*)(void*)>(player, g_slot.playerGetId);

    return fn ? fn(player) : -1;
}

bool PlayerAlive(void* player) {
    auto fn = Virtual<uintptr_t (*)(void*)>(player, g_slot.playerIsAlive);

    return fn && LowByte(fn(player));
}

void* PlayerSoldier(void* player) {
    auto fn = Virtual<void* (*)(void*)>(player, g_slot.playerDefaultVehicle);

    return fn ? fn(player) : nullptr;
}

void* PlayerVehicle(void* player) {
    auto fn = Virtual<void* (*)(void*)>(player, g_slot.playerGetVehicle);

    return fn ? fn(player) : nullptr;
}

void* RootOf(void* object) { return object && g_getRootParent ? g_getRootParent(object) : object; }

std::string TemplateName(void* object) {
    uintptr_t tmpl = 0;

    if (!object || !g_offTemplate || !elf::SafeReadT(reinterpret_cast<uintptr_t>(object) + g_offTemplate, tmpl) ||
        !tmpl)
        return {};

    auto getName = Virtual<const gcc3::String* (*)(uintptr_t)>(reinterpret_cast<void*>(tmpl), g_slot.templateGetName);

    return getName ? gcc3::Read(getName(tmpl)) : std::string();
}

/// Player whose own soldier is @p target, or -1. A soldier is a
/// player-control object whose player has it as their default vehicle.
int SoldierOwner(void* target) {
    void* pco = QueryInterface(target, kIidPlayerControl);
    auto getPlayer = Virtual<void* (*)(void*)>(pco, g_slot.pcoGetPlayer);
    void* player = getPlayer ? getPlayer(pco) : nullptr;

    return player && PlayerSoldier(player) == target ? PlayerId(player) : -1;
}

/// Living players whose current vehicle (any seat) belongs to @p root.
std::vector<rmm_player_t> OccupantsOf(void* root) {
    std::vector<rmm_player_t> ids;
    void* pm = PlayerManager();

    if (!pm || !root)
        return ids;

    for (int id = 0; id <= g_maxPlayerId; ++id) {
        void* p = PlayerFromId(pm, id);

        if (!p || !PlayerAlive(p))
            continue;

        void* v = PlayerVehicle(p);

        if (v && v != PlayerSoldier(p) && RootOf(v) == root)
            ids.push_back(id);
    }

    return ids;
}

Core* ReadyCore() {
    R2Adapter* a = R2Adapter::Instance();
    Core* c = a ? a->CorePtr() : nullptr;

    return c && c->Ready() ? c : nullptr;
}

// PRE hook: PLAYER_DAMAGE for hits on a player's soldier, VEHICLE_DAMAGE for
// hits on anything player-controllable. Both blockable (skip the original)
// and mutable (the damage argument).
void Hook_ServerGiveDamage(void* target, float damage, void* attacker, void* assists, void* attackerObject, int a6,
                           void* a7, void* a8, const void* v1, const void* v2, int a11, bool b12, bool b13, bool b14,
                           bool b15) {
    Core* core = ReadyCore();

    if (core && target && damage > 0.0f) {
        int attackerId = attacker ? PlayerId(attacker) : -1;
        rmm_player_t att = attackerId >= 0 ? attackerId : RMM_PLAYER_NONE;
        int victim = SoldierOwner(target);

        if (victim >= 0) {
            float amount = damage;
            std::string weapon = TemplateName(attackerObject);
            rmm_ev_player_damage ev{victim, att, &amount, weapon.c_str()};

            if (core->Dispatch(RMM_EV_PLAYER_DAMAGE, &ev) >= RMM_HANDLED)
                return;  // blocked

            if (!(amount >= 0.0f))
                amount = 0.0f;  // also catches NaN

            damage = amount;
        } else if (core->Events().HasHooks(RMM_EV_VEHICLE_DAMAGE)) {
            // Built only when someone listens: this runs for every hit.
            void* root = RootOf(target);

            if (root && QueryInterface(root, kIidPlayerControl)) {
                std::vector<rmm_player_t> occupants = OccupantsOf(root);
                std::string tmpl = TemplateName(root), weapon = TemplateName(attackerObject);
                float amount = damage;
                // Native handles are IObject pointers; they match the bridge's
                // handles only when it could read the object's token (see BridgeObject).
                rmm_ev_vehicle_damage ev{static_cast<rmm_object_t>(reinterpret_cast<uintptr_t>(root)),
                                         tmpl.c_str(),
                                         att,
                                         &amount,
                                         weapon.c_str(),
                                         static_cast<int32_t>(occupants.size()),
                                         occupants.empty() ? nullptr : occupants.data()};

                if (core->Dispatch(RMM_EV_VEHICLE_DAMAGE, &ev) >= RMM_HANDLED)
                    return;

                if (!(amount >= 0.0f))
                    amount = 0.0f;

                damage = amount;
            }
        }
    }

    g_serverGiveDamage(target, damage, attacker, assists, attackerObject, a6, a7, a8, v1, v2, a11, b12, b13, b14, b15);
}

uint32_t Num(const Ini& ini, const char* section, const char* key, uint32_t def) {
    std::string v = ini.Get(section, key);

    return v.empty() ? def : static_cast<uint32_t>(std::strtoul(v.c_str(), nullptr, 0));
}

}  // namespace

bool R2Adapter::AttachNative(std::string& err) {
    if (data_.Get("linux", "native", "0") != "1")
        return false;  // disabled: bridge only

    if (!elf::Self().LoadSelf(err)) {
        err = "native hooks unavailable, cannot read the server's symbol table: " + err;

        return false;
    }

    auto sym = [this](const char* key) -> uintptr_t {
        std::string mangled = data_.Get("linux", key);
        uintptr_t a = mangled.empty() ? 0 : elf::Self().Find(mangled);

        if (!a)
            core_->Log().Warn("%s: native symbol '%s' not found", shortName_.c_str(), key);

        return a;
    };

    const char* s = "linux_slots";
    g_slot.queryInterface = Num(data_, s, "iobject_query_interface", g_slot.queryInterface);
    g_slot.pcoGetPlayer = Num(data_, s, "pco_get_player", g_slot.pcoGetPlayer);
    g_slot.templateGetName = Num(data_, s, "template_get_name", g_slot.templateGetName);
    g_slot.playerGetVehicle = Num(data_, s, "player_get_vehicle", g_slot.playerGetVehicle);
    g_slot.playerGetId = Num(data_, s, "player_get_id", g_slot.playerGetId);
    g_slot.playerIsAlive = Num(data_, s, "player_get_is_alive", g_slot.playerIsAlive);
    g_slot.playerDefaultVehicle = Num(data_, s, "player_get_default_vehicle", g_slot.playerDefaultVehicle);
    g_slot.playerGetTeam = Num(data_, s, "player_get_team", g_slot.playerGetTeam);
    g_slot.pmGetPlayerFromId = Num(data_, s, "playermanager_get_player_from_id", g_slot.pmGetPlayerFromId);
    const char* offs = sizeof(void*) == 8 ? "linux64_offsets" : "linux_offsets";
    g_offTemplate = Num(data_, offs, "object_template", 0);
    g_maxPlayerId = static_cast<int>(Num(data_, "linux", "max_player_id", 255));

    g_getRootParent = reinterpret_cast<GetRootParentFn>(sym("get_root_parent"));
    g_playerManagerGlobal = sym("player_manager");

    if (uintptr_t vt = sym("playermanager_vtable"))
        g_playerManagerVptr = vt + 2 * sizeof(void*);  // GCC vptr = &vtable[2]

    if (!g_offTemplate)
        core_->Log().Warn("%s: no object_template offset for this build; weapon/vehicle names will be empty",
                          shortName_.c_str());

    uintptr_t dmg = sym("server_give_damage");

    if (!dmg || !g_playerManagerGlobal) {
        err = "native damage hook needs server_give_damage and player_manager";

        return false;
    }

    g_serverGiveDamage = reinterpret_cast<ServerGiveDamageFn>(dmg);

    if (!hooks::Detour(reinterpret_cast<void*>(dmg), reinterpret_cast<void*>(&Hook_ServerGiveDamage),
                       reinterpret_cast<void**>(&g_serverGiveDamage))) {
        err = "could not detour GameLogicDamage::serverGiveDamage: " + std::string(hooks::LastDetourError());

        return false;
    }

    nativeDamage_ = true;
    core_->Log().Info("%s: native damage hook installed (PLAYER_DAMAGE / VEHICLE_DAMAGE are blockable)",
                      shortName_.c_str());

    return true;
}

}  // namespace rmm

#endif  // !_WIN32 || RMM_LINUX_SYNTAX_CHECK
