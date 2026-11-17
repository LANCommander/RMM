// Implements the C function tables handed to plugins. Every function here is
// a thin, defensive wrapper: validate arguments (plugins are untrusted), then
// forward to the core subsystem.
#include "core.h"
#include "platform.h"
#include "util.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <cstring>

namespace rmm {
namespace {

Core& C() { return Core::Get(); }

bool ValidSelf(rmm_plugin_t self) { return C().Plugins().IsValid(self) && self->state != PluginState::Failed; }

#define REQUIRE_SELF(ret)                                                                                              \
    do {                                                                                                               \
        if (!ValidSelf(self)) {                                                                                        \
            C().Log().Warn("%s called with invalid plugin handle", __func__);                                          \
            return ret;                                                                                                \
        }                                                                                                              \
    } while (0)

std::string S(const char* s) { return s ? s : ""; }

/* =============================== core ===================================== */

const char* RMM_CALL core_version() { return RMM_VERSION_STRING; }

void RMM_CALL core_log(rmm_plugin_t self, int32_t level, const char* msg) {
    std::string tag = C().Plugins().IsValid(self) ? self->logTag : "?";
    C().Log().Write(std::clamp(level, 0, 3), tag, S(msg));
}

size_t RMM_CALL core_build_path(int32_t kind, const char* rel, char* out, size_t cap) {
    return CopyOut(C().BuildPath(kind, S(rel)), out, cap);
}

const char* RMM_CALL core_plugin_name(rmm_plugin_t p) { return C().Plugins().IsValid(p) ? p->name.c_str() : nullptr; }

rmm_plugin_t RMM_CALL core_find_plugin(const char* name) { return C().Plugins().Find(S(name)); }

void RMM_CALL core_request_unload(rmm_plugin_t self, const char* reason) {
    if (!ValidSelf(self))
        return;

    C().Plugins().RequestUnload(self, S(reason));
}

rmm_handle_t RMM_CALL core_timer_create(rmm_plugin_t self, double interval, uint32_t flags, rmm_timer_cb cb,
                                        void* user) {
    REQUIRE_SELF(RMM_INVALID_HANDLE);

    return C().Timers().Create(self, interval, flags, cb, user, C().Time());
}

rmm_bool RMM_CALL core_timer_kill(rmm_plugin_t self, rmm_handle_t t) {
    REQUIRE_SELF(RMM_FALSE);

    return C().Timers().Kill(self, t);
}

rmm_bool RMM_CALL core_iface_register(rmm_plugin_t self, const char* name, uint32_t version, void* iface) {
    REQUIRE_SELF(RMM_FALSE);

    return C().IfaceRegister(self, S(name), version, iface);
}

void* RMM_CALL core_iface_query(const char* name, uint32_t min_version, uint32_t* out_version) {
    return C().IfaceQuery(S(name), min_version, out_version);
}

/* =============================== events =================================== */

rmm_handle_t RMM_CALL ev_hook(rmm_plugin_t self, int32_t id, int32_t phase, int32_t prio, rmm_event_cb cb, void* user) {
    REQUIRE_SELF(RMM_INVALID_HANDLE);
    // Plugins may not use the reserved core priority band.
    prio = std::clamp(prio, -999, 999);

    return C().Events().Hook(self, id, phase, prio, cb, user);
}

rmm_bool RMM_CALL ev_unhook(rmm_plugin_t self, rmm_handle_t h) {
    REQUIRE_SELF(RMM_FALSE);

    return C().Events().Unhook(self, h);
}

uint32_t RMM_CALL ev_flags(int32_t id) { return C().Events().Flags(id); }

const char* RMM_CALL ev_name(int32_t id) { return C().Events().Name(id); }

int32_t RMM_CALL ev_find(const char* name) { return C().Events().Find(S(name)); }

int32_t RMM_CALL ev_register_custom(rmm_plugin_t self, const char* name, uint32_t flags) {
    REQUIRE_SELF(RMM_EV_NONE);

    if (!name || !*name)
        return RMM_EV_NONE;

    return C().Events().RegisterCustom(self, name, flags);
}

int32_t RMM_CALL ev_fire(rmm_plugin_t self, int32_t id, void* data) {
    REQUIRE_SELF(RMM_CONTINUE);

    if (!C().Events().IsCustom(id)) {
        C().Log().Warn("%s tried to fire built-in event %d; only custom events can be fired", self->name.c_str(), id);

        return RMM_CONTINUE;
    }

    return C().Events().Dispatch(id, data);
}

/* =============================== vars ===================================== */

rmm_bool RMM_CALL var_engine_get(const char* name, char* out, size_t cap) {
    std::string v;

    if (!name || !C().Game().GetEngineVar(name, v))
        return RMM_FALSE;

    CopyOut(v, out, cap);

    return RMM_TRUE;
}

rmm_bool RMM_CALL var_engine_set(const char* name, const char* value) {
    if (!name)
        return RMM_FALSE;

    return C().Game().SetEngineVar(name, S(value));
}

rmm_bool RMM_CALL var_engine_watch(rmm_plugin_t self, const char* name) {
    REQUIRE_SELF(RMM_FALSE);

    if (!name || !*name)
        return RMM_FALSE;

    C().Vars().Watch(self, name);

    return RMM_TRUE;
}

const char* RMM_CALL var_setting_var(int32_t setting) {
    // Stable storage per setting so the returned pointer stays valid.
    static std::string names[RMM_SETTING_COUNT];

    if (setting < 0 || setting >= RMM_SETTING_COUNT)
        return nullptr;

    names[setting] = C().Game().SettingVar(setting);

    return names[setting].empty() ? nullptr : names[setting].c_str();
}

rmm_bool RMM_CALL var_setting_get(int32_t setting, char* out, size_t cap) {
    const char* n = var_setting_var(setting);

    return n ? var_engine_get(n, out, cap) : RMM_FALSE;
}

rmm_bool RMM_CALL var_setting_set(int32_t setting, const char* value) {
    const char* n = var_setting_var(setting);

    return n ? var_engine_set(n, value) : RMM_FALSE;
}

rmm_handle_t RMM_CALL var_create(rmm_plugin_t self, const char* name, const char* def, const char* desc,
                                 uint32_t flags) {
    REQUIRE_SELF(RMM_INVALID_HANDLE);
    std::string err;
    rmm_handle_t h = C().Vars().Create(self, S(name), S(def), S(desc), flags, &err);

    if (!h)
        C().Log().Warn("%s: could not create var '%s': %s", self->name.c_str(), S(name).c_str(), err.c_str());

    return h;
}

rmm_handle_t RMM_CALL var_find(const char* name) {
    Var* v = C().Vars().Find(S(name));

    return v ? v->h : RMM_INVALID_HANDLE;
}

rmm_bool RMM_CALL var_set_bounds(rmm_handle_t h, rmm_bool has_min, double mn, rmm_bool has_max, double mx) {
    Var* v = C().Vars().Get(h);

    if (!v)
        return RMM_FALSE;

    v->hasMin = has_min != 0;
    v->min = mn;
    v->hasMax = has_max != 0;
    v->max = mx;

    return RMM_TRUE;
}

size_t RMM_CALL var_get_string(rmm_handle_t h, char* out, size_t cap) {
    Var* v = C().Vars().Get(h);

    return CopyOut(v ? v->value : std::string(), out, cap);
}

int32_t RMM_CALL var_get_int(rmm_handle_t h) {
    Var* v = C().Vars().Get(h);

    return v ? static_cast<int32_t>(std::strtol(v->value.c_str(), nullptr, 10)) : 0;
}

double RMM_CALL var_get_float(rmm_handle_t h) {
    Var* v = C().Vars().Get(h);

    return v ? std::strtod(v->value.c_str(), nullptr) : 0.0;
}

rmm_bool RMM_CALL var_get_bool(rmm_handle_t h) {
    Var* v = C().Vars().Get(h);

    if (!v)
        return RMM_FALSE;

    std::string s = ToLower(Trim(v->value));

    return !(s.empty() || s == "0" || s == "false" || s == "off" || s == "no");
}

rmm_bool RMM_CALL var_set_string(rmm_plugin_t self, rmm_handle_t h, const char* value) {
    REQUIRE_SELF(RMM_FALSE);

    return C().Vars().Set(self, C().Vars().Get(h), S(value));
}

rmm_bool RMM_CALL var_reset(rmm_plugin_t self, rmm_handle_t h) {
    REQUIRE_SELF(RMM_FALSE);
    Var* v = C().Vars().Get(h);

    return v ? C().Vars().Set(self, v, v->def) : RMM_FALSE;
}

rmm_handle_t RMM_CALL var_hook_change(rmm_plugin_t self, rmm_handle_t var, rmm_var_change_cb cb, void* user) {
    REQUIRE_SELF(RMM_INVALID_HANDLE);

    return C().Vars().HookChange(self, var, cb, user);
}

rmm_bool RMM_CALL var_unhook_change(rmm_plugin_t self, rmm_handle_t hook) {
    REQUIRE_SELF(RMM_FALSE);

    return C().Vars().UnhookChange(self, hook);
}

rmm_bool RMM_CALL var_get(const char* name, char* out, size_t cap) {
    if (Var* v = C().Vars().Find(S(name))) {
        CopyOut(v->value, out, cap);

        return RMM_TRUE;
    }

    return var_engine_get(name, out, cap);
}

rmm_bool RMM_CALL var_set(rmm_plugin_t self, const char* name, const char* value) {
    REQUIRE_SELF(RMM_FALSE);

    if (Var* v = C().Vars().Find(S(name)))
        return C().Vars().Set(self, v, S(value));

    return var_engine_set(name, value);
}

/* =============================== commands ================================= */

rmm_handle_t RMM_CALL cmd_add(rmm_plugin_t self, const char* name, uint32_t flags, uint32_t admin_flags,
                              const char* desc, const char* usage, rmm_cmd_cb cb, void* user) {
    REQUIRE_SELF(RMM_INVALID_HANDLE);
    rmm_handle_t h = C().Cmds().Add(self, S(name), flags, admin_flags, S(desc), S(usage), cb, user);

    if (!h)
        C().Log().Warn("%s: could not register command '%s' (name taken?)", self->name.c_str(), S(name).c_str());

    return h;
}

rmm_bool RMM_CALL cmd_remove(rmm_plugin_t self, rmm_handle_t h) {
    REQUIRE_SELF(RMM_FALSE);

    return C().Cmds().Remove(self, h);
}

int32_t RMM_CALL cmd_argc(const rmm_cmd_ctx* c) { return c ? static_cast<int32_t>(c->argv.size()) - 1 : 0; }

const char* RMM_CALL cmd_argv(const rmm_cmd_ctx* c, int32_t i) {
    if (!c || i < 0 || static_cast<size_t>(i) >= c->argv.size())
        return "";

    return c->argv[static_cast<size_t>(i)].c_str();
}

const char* RMM_CALL cmd_args(const rmm_cmd_ctx* c) { return c ? c->raw.c_str() : ""; }

const char* RMM_CALL cmd_args_from(const rmm_cmd_ctx* c, int32_t first) {
    if (!c)
        return "";

    c->scratch = Join(c->argv, static_cast<size_t>(std::max(first, 1)));

    return c->scratch.c_str();
}

rmm_player_t RMM_CALL cmd_caller(const rmm_cmd_ctx* c) { return c ? c->caller : RMM_PLAYER_NONE; }

int32_t RMM_CALL cmd_source(const rmm_cmd_ctx* c) { return c ? c->source : RMM_SRC_CONSOLE; }

// rmm_msg_opts -> interval / immediate; NULL or short struct = defaults.
struct MsgOpts {
    double interval = -1;
    bool immediate = false;
};

MsgOpts ReadOpts(const rmm_msg_opts* o) {
    MsgOpts r;

    if (!o)
        return r;

    if (o->struct_size >= offsetof(rmm_msg_opts, interval) + sizeof o->interval)
        r.interval = o->interval;

    if (o->struct_size >= offsetof(rmm_msg_opts, flags) + sizeof o->flags)
        r.immediate = (o->flags & RMM_MSG_IMMEDIATE) != 0;

    return r;
}

void RMM_CALL cmd_reply(const rmm_cmd_ctx* c, const char* msg) {
    if (c && msg)
        C().Cmds().Reply(c, msg);
}

void RMM_CALL cmd_reply_ex(const rmm_cmd_ctx* c, const char* msg, const rmm_msg_opts* opts) {
    if (!c || !msg)
        return;

    MsgOpts o = ReadOpts(opts);
    C().Cmds().Reply(c, msg, o.interval, o.immediate);
}

rmm_bool RMM_CALL cmd_engine_exec(const char* command, char* out, size_t cap) {
    if (!command)
        return RMM_FALSE;

    std::string o;
    bool ok = C().Game().Exec(command, &o);
    CopyOut(o, out, cap);

    return ok;
}

rmm_bool RMM_CALL cmd_exec(rmm_plugin_t self, const char* line, char* out, size_t cap) {
    REQUIRE_SELF(RMM_FALSE);
    std::string o;
    bool ok = C().ExecLine(RMM_SRC_PLUGIN, RMM_PLAYER_NONE, S(line), &o);
    CopyOut(o, out, cap);

    return ok;
}

/* =============================== players ================================== */

int32_t RMM_CALL pl_max_players() { return C().Game().MaxPlayers(); }

int32_t RMM_CALL pl_count() { return static_cast<int32_t>(C().Game().PlayerIds().size()); }

int32_t RMM_CALL pl_list(rmm_player_t* out, int32_t cap) {
    auto ids = C().Game().PlayerIds();

    for (int32_t i = 0; out && i < cap && static_cast<size_t>(i) < ids.size(); ++i)
        out[i] = ids[static_cast<size_t>(i)];

    return static_cast<int32_t>(ids.size());
}

rmm_bool RMM_CALL pl_is_connected(rmm_player_t p) {
    auto ids = C().Game().PlayerIds();

    return std::find(ids.begin(), ids.end(), p) != ids.end();
}

rmm_bool RMM_CALL pl_get_info(rmm_player_t p, rmm_player_info* out) {
    if (!out || out->struct_size < sizeof(uint32_t))
        return RMM_FALSE;

    rmm_player_info tmp{};
    tmp.struct_size = sizeof tmp;
    tmp.seat = -1;

    if (!C().Game().GetPlayer(p, tmp))
        return RMM_FALSE;

    if (!tmp.vehicle)
        tmp.seat = -1;  // on foot: no seat, whatever the adapter left there

    // Copy only as much as the caller's (possibly older, smaller) struct holds.
    uint32_t callerSize = out->struct_size;
    std::memcpy(out, &tmp, std::min<size_t>(callerSize, sizeof tmp));
    out->struct_size = callerSize;

    return RMM_TRUE;
}

rmm_player_t RMM_CALL pl_find_by_name(const char* name) {
    if (!name || !*name)
        return RMM_PLAYER_NONE;

    rmm_player_t partial = RMM_PLAYER_NONE;
    int partialCount = 0;

    for (rmm_player_t id : C().Game().PlayerIds()) {
        rmm_player_info info{};
        info.struct_size = sizeof info;

        if (!C().Game().GetPlayer(id, info))
            continue;

        if (IEquals(info.name, name))
            return id;

        if (IContains(info.name, name)) {
            partial = id;
            ++partialCount;
        }
    }

    return partialCount > 1 ? RMM_PLAYER_AMBIGUOUS : partial;
}

int32_t RMM_CALL pl_target(const char* pattern, rmm_player_t caller, uint32_t flags, rmm_player_t* out, int32_t cap,
                           char* err, size_t err_cap) {
    std::string pat = Trim(S(pattern));
    auto fail = [&](const std::string& m) {
        CopyOut(m, err, err_cap);

        return 0;
    };

    if (pat.empty())
        return fail("No target given.");

    std::vector<rmm_player_t> found;
    auto& game = C().Game();
    auto all = game.PlayerIds();
    auto info = [&](rmm_player_t id, rmm_player_info& i) {
        i = rmm_player_info{};
        i.struct_size = sizeof i;

        return game.GetPlayer(id, i);
    };

    if (pat[0] == '@') {
        std::string g = ToLower(pat);

        if (!(flags & RMM_TARGET_ALLOW_MULTI) && g != "@me")
            return fail("Group targets are not allowed here.");

        for (rmm_player_t id : all) {
            rmm_player_info i;

            if (!info(id, i))
                continue;

            bool take = g == "@all" || (g == "@t1" && i.team == 1) || (g == "@t2" && i.team == 2) ||
                        (g == "@alive" && i.is_alive) || (g == "@dead" && !i.is_alive) || (g == "@bots" && i.is_bot) ||
                        (g == "@humans" && !i.is_bot) || (g == "@me" && id == caller) || (g == "@!me" && id != caller);

            if (take)
                found.push_back(id);
        }

        if (g != "@all" && g != "@t1" && g != "@t2" && g != "@alive" && g != "@dead" && g != "@bots" &&
            g != "@humans" && g != "@me" && g != "@!me")
            return fail("Unknown target group " + pat + ".");
    } else if (pat[0] == '#' && pat.size() > 1 && std::isdigit(static_cast<unsigned char>(pat[1]))) {
        rmm_player_t id = std::atoi(pat.c_str() + 1);

        if (std::find(all.begin(), all.end(), id) == all.end())
            return fail("No player with id " + pat + ".");

        found.push_back(id);
    } else {
        rmm_player_t id = pl_find_by_name(pat.c_str());

        if (id == RMM_PLAYER_AMBIGUOUS)
            return fail("More than one player matches '" + pat + "'.");

        if (id == RMM_PLAYER_NONE)
            return fail("No player matches '" + pat + "'.");

        found.push_back(id);
    }

    std::vector<rmm_player_t> filtered;
    bool immunityBlocked = false;

    for (rmm_player_t id : found) {
        rmm_player_info i;

        if (!info(id, i))
            continue;

        if ((flags & RMM_TARGET_NO_BOTS) && i.is_bot)
            continue;

        if ((flags & RMM_TARGET_ALIVE_ONLY) && !i.is_alive)
            continue;

        if (!(flags & RMM_TARGET_NO_IMMUNITY) && !C().Admin().CanTarget(caller, id)) {
            immunityBlocked = true;
            continue;
        }

        filtered.push_back(id);
    }

    if (filtered.empty())
        return fail(immunityBlocked ? "Target is immune." : "No matching players.");

    int32_t n = 0;

    for (rmm_player_t id : filtered) {
        if (n >= cap)
            break;

        if (out)
            out[n] = id;

        ++n;
    }

    return n;
}

rmm_bool RMM_CALL pl_kill(rmm_player_t p) { return C().Game().KillPlayer(p); }

rmm_bool RMM_CALL pl_set_team(rmm_player_t p, int32_t team) { return C().Game().SetTeam(p, team); }

rmm_bool RMM_CALL pl_message_ex(rmm_player_t p, const char* m, const rmm_msg_opts* opts) {
    if (!m || !C().Game().HasCap(RMM_CAP_PRIVATE_MESSAGE) || !pl_is_connected(p))
        return RMM_FALSE;

    MsgOpts o = ReadOpts(opts);

    return C().Chat().Send(ChatQueue::Kind::Player, p, m, o.interval, o.immediate);
}

rmm_bool RMM_CALL pl_message(rmm_player_t p, const char* m) { return pl_message_ex(p, m, nullptr); }

rmm_bool RMM_CALL pl_set_score(rmm_player_t p, int32_t s) { return C().Game().SetScore(p, s); }

rmm_bool RMM_CALL pl_teleport(rmm_player_t p, rmm_vec3 pos) { return C().Game().Teleport(p, pos); }

bool Finite(float v) { return v == v && v - v == 0.0f; }  // not NaN, not +-inf

bool Finite(const rmm_vec3& v) { return Finite(v.x) && Finite(v.y) && Finite(v.z); }

rmm_bool RMM_CALL pl_damage(rmm_player_t p, float amount, rmm_player_t attacker) {
    if (!Finite(amount) || amount <= 0.0f)
        return RMM_FALSE;

    return C().Game().DamagePlayer(p, amount, attacker < 0 ? RMM_PLAYER_NONE : attacker);
}

rmm_bool RMM_CALL pl_get_health(rmm_player_t p, float* health, float* max_health) {
    float h = 0, m = -1;

    if (!C().Game().PlayerHealth(p, h, m))
        return RMM_FALSE;

    if (health)
        *health = h;

    if (max_health)
        *max_health = m;

    return RMM_TRUE;
}

rmm_bool RMM_CALL pl_push(rmm_player_t p, rmm_vec3 v) { return Finite(v) && C().Game().PushPlayer(p, v); }

rmm_bool RMM_CALL pl_set_kit(rmm_player_t p, const char* kit) {
    if (!kit || !*kit || !C().Game().HasCap(RMM_CAP_SET_KIT))
        return RMM_FALSE;

    if (std::strpbrk(kit, " \t\r\n;\""))
        return RMM_FALSE;  // a template name, never console syntax

    bool ok = C().Game().SetKit(p, kit);
    C().Log().Debug("set_kit #%d %s -> %s", p, kit, ok ? "ok" : "failed");

    return ok;
}

rmm_bool RMM_CALL pl_enter_vehicle(rmm_player_t p, rmm_object_t v, int32_t seat) {
    if (!v || seat < -1 || !C().Game().HasCap(RMM_CAP_PUT_IN_VEHICLE))
        return RMM_FALSE;

    bool ok = C().Game().PutInVehicle(p, v, seat);
    C().Log().Debug("enter_vehicle #%d 0x%llx seat %d -> %s", p, static_cast<unsigned long long>(v), seat,
                    ok ? "ok" : "failed");

    return ok;
}

rmm_bool RMM_CALL pl_exit_vehicle(rmm_player_t p) {
    return C().Game().HasCap(RMM_CAP_PUT_IN_VEHICLE) && C().Game().TakeOutOfVehicle(p);
}

int32_t RMM_CALL pl_get_items(rmm_player_t p, rmm_item_info* out, int32_t cap) {
    std::vector<rmm_item_info> items;

    if (!C().Game().HasCap(RMM_CAP_ITEMS) || !C().Game().GetItems(p, items))
        return -1;

    for (int32_t i = 0; out && i < cap && i < static_cast<int32_t>(items.size()); ++i)
        out[i] = items[i];

    return static_cast<int32_t>(items.size());
}

rmm_bool RMM_CALL pl_select_item(rmm_player_t p, int32_t slot) {
    return slot >= 0 && C().Game().HasCap(RMM_CAP_ITEMS) && C().Game().SelectItem(p, slot);
}

rmm_bool RMM_CALL pl_remove_item(rmm_player_t p, int32_t slot) {
    bool ok = slot >= 0 && C().Game().HasCap(RMM_CAP_ITEMS) && C().Game().RemoveItem(p, slot);
    C().Log().Debug("remove_item #%d slot %d -> %s", p, slot, ok ? "ok" : "failed");

    return ok;
}

/* =============================== world ==================================== */

int32_t RMM_CALL wd_find_vehicles(const char* pattern, uint32_t flags, rmm_object_t* out, int32_t cap) {
    auto& game = C().Game();
    std::string pat = S(pattern);
    int32_t n = 0;

    for (auto& v : game.Vehicles()) {
        if ((flags & RMM_FIND_ROOTS_ONLY) && !v.isRoot)
            continue;

        if (!GlobMatch(pat, v.templateName))
            continue;

        if ((flags & RMM_FIND_EMPTY_ONLY) && !game.ObjectOccupants(v.handle).empty())
            continue;

        if (out && n < cap)
            out[n] = v.handle;

        ++n;
    }

    return n;
}

rmm_bool RMM_CALL wd_is_valid(rmm_object_t o) { return o && C().Game().ObjectValid(o); }

rmm_bool RMM_CALL wd_get_info(rmm_object_t o, rmm_object_info* out) {
    if (!o || !out || out->struct_size < sizeof(uint32_t))
        return RMM_FALSE;

    rmm_object_info tmp{};
    tmp.struct_size = sizeof tmp;
    tmp.object = o;
    tmp.health = tmp.max_health = -1.0f;
    tmp.seat = -1;
    tmp.seat_occupant = RMM_PLAYER_NONE;

    if (!C().Game().GetObjectInfo(o, tmp))
        return RMM_FALSE;

    uint32_t callerSize = out->struct_size;
    std::memcpy(out, &tmp, std::min<size_t>(callerSize, sizeof tmp));
    out->struct_size = callerSize;

    return RMM_TRUE;
}

int32_t RMM_CALL wd_occupants(rmm_object_t o, rmm_player_t* out, int32_t cap) {
    if (!o)
        return 0;

    auto v = C().Game().ObjectOccupants(o);

    for (int32_t i = 0; out && i < cap && static_cast<size_t>(i) < v.size(); ++i)
        out[i] = v[static_cast<size_t>(i)];

    return static_cast<int32_t>(v.size());
}

rmm_bool RMM_CALL wd_set_position(rmm_object_t o, rmm_vec3 p) {
    return o && Finite(p) && C().Game().SetObjectPosition(o, p);
}

rmm_bool RMM_CALL wd_set_rotation(rmm_object_t o, rmm_vec3 r) {
    return o && Finite(r) && C().Game().SetObjectRotation(o, r);
}

rmm_bool RMM_CALL wd_set_velocity(rmm_object_t o, rmm_vec3 lin, rmm_vec3 ang) {
    return o && Finite(lin) && Finite(ang) && C().Game().SetObjectVelocity(o, lin, ang);
}

int32_t RMM_CALL wd_find_objects(const char* pattern, const rmm_vec3* center, float radius, uint32_t flags,
                                 rmm_object_t* out, int32_t cap) {
    if (center && (!Finite(*center) || !(radius >= 0.0f)))
        return 0;  // also rejects NaN radius

    std::vector<IGameAdapter::ObjectRef> hits =
        FilterObjects(C().Game().WorldObjects(), S(pattern), center, radius, flags);

    for (int32_t i = 0; out && i < cap && static_cast<size_t>(i) < hits.size(); ++i)
        out[i] = hits[size_t(i)].handle;

    return static_cast<int32_t>(hits.size());
}

rmm_object_t RMM_CALL wd_spawn_object(const char* tmpl, rmm_vec3 pos, rmm_vec3 rot, uint32_t flags) {
    if (!tmpl || !*tmpl || !Finite(pos) || !Finite(rot))
        return 0;

    auto& game = C().Game();

    if (flags & RMM_SPAWN_ON_GROUND) {
        float ground = 0;

        if (!game.GroundHeight(pos.x, pos.z, ground))
            return 0;

        pos.y += ground;
    }

    rmm_object_t h = game.SpawnObject(tmpl, pos, rot);
    C().Log().Debug("spawn_object %s at %.1f,%.1f,%.1f -> 0x%llx", tmpl, pos.x, pos.y, pos.z,
                    static_cast<unsigned long long>(h));

    return h;
}

rmm_bool RMM_CALL wd_remove_object(rmm_object_t o) { return o && C().Game().RemoveObject(o); }

rmm_bool RMM_CALL wd_ground_height(float x, float z, float* height) {
    float h = 0;

    if (!Finite(x) || !Finite(z) || !C().Game().GroundHeight(x, z, h))
        return RMM_FALSE;

    if (height)
        *height = h;

    return RMM_TRUE;
}

rmm_bool RMM_CALL wd_template_exists(const char* tmpl) { return tmpl && *tmpl && C().Game().TemplateExists(tmpl); }

rmm_bool RMM_CALL wd_damage_object(rmm_object_t o, float amount, rmm_player_t attacker) {
    if (!o || !Finite(amount) || amount <= 0.0f || !C().Game().HasCap(RMM_CAP_OBJECT_DAMAGE))
        return RMM_FALSE;

    bool ok = C().Game().DamageObject(o, amount, attacker < 0 ? RMM_PLAYER_NONE : attacker);
    C().Log().Debug("damage_object 0x%llx %.1f by #%d -> %s", static_cast<unsigned long long>(o), amount, attacker,
                    ok ? "ok" : "failed");

    return ok;
}

int32_t RMM_CALL wd_control_points(rmm_object_t* out, int32_t cap) {
    if (!C().Game().HasCap(RMM_CAP_CONTROL_POINTS))
        return 0;

    auto v = C().Game().ControlPoints();

    for (int32_t i = 0; out && i < cap && static_cast<size_t>(i) < v.size(); ++i)
        out[i] = v[static_cast<size_t>(i)];

    return static_cast<int32_t>(v.size());
}

rmm_bool RMM_CALL wd_get_control_point(rmm_object_t cp, rmm_control_point_info* out) {
    if (!cp || !out || out->struct_size < sizeof(uint32_t) || !C().Game().HasCap(RMM_CAP_CONTROL_POINTS))
        return RMM_FALSE;

    rmm_control_point_info tmp{};
    tmp.struct_size = sizeof tmp;
    tmp.object = cp;

    if (!C().Game().GetControlPoint(cp, tmp))
        return RMM_FALSE;

    uint32_t callerSize = out->struct_size;
    std::memcpy(out, &tmp, std::min<size_t>(callerSize, sizeof tmp));
    out->struct_size = callerSize;

    return RMM_TRUE;
}

rmm_bool RMM_CALL wd_set_control_point(rmm_object_t cp, int32_t team) {
    if (!cp || team < 0 || team > 2 || !C().Game().HasCap(RMM_CAP_CONTROL_POINT_SET))
        return RMM_FALSE;

    bool ok = C().Game().SetControlPoint(cp, team);
    C().Log().Debug("set_control_point 0x%llx team %d -> %s", static_cast<unsigned long long>(cp), team,
                    ok ? "ok" : "failed");

    return ok;
}

rmm_bool RMM_CALL wd_lock_control_point(rmm_object_t cp, rmm_bool locked) {
    if (!cp || !C().Game().HasCap(RMM_CAP_CONTROL_POINT_LOCK))
        return RMM_FALSE;

    return C().Game().LockControlPoint(cp, locked != 0);
}

/* =============================== bots ===================================== */

// Every bot call first asks the adapter whether the AI runs (BF1942: co-op
// only), because some engine AI commands crash the server otherwise.
bool BotsUp(int32_t cap) { return C().Game().HasCap(cap) && C().Game().BotsAvailable(); }

rmm_bool RMM_CALL bot_available() { return C().Game().HasCap(RMM_CAP_BOT_QUERY) && C().Game().BotsAvailable(); }

int32_t RMM_CALL bot_list(rmm_player_t* out, int32_t cap) {
    if (!BotsUp(RMM_CAP_BOT_QUERY))
        return 0;

    auto bots = C().Game().Bots();

    for (int32_t i = 0; out && i < cap && static_cast<size_t>(i) < bots.size(); ++i)
        out[i] = bots[size_t(i)].player;

    return static_cast<int32_t>(bots.size());
}

int32_t RMM_CALL bot_count() { return bot_list(nullptr, 0); }

rmm_bool RMM_CALL bot_is_bot(rmm_player_t p) {
    IGameAdapter::BotRef b;

    return C().Bots().Resolve(p, b);
}

rmm_bool RMM_CALL bot_get_info(rmm_player_t p, rmm_bot_info* out) {
    if (!out || out->struct_size < sizeof(uint32_t))
        return RMM_FALSE;

    IGameAdapter::BotRef b;

    if (!C().Bots().Resolve(p, b))
        return RMM_FALSE;

    rmm_bot_info tmp{};
    tmp.struct_size = sizeof tmp;
    tmp.player = b.player;
    tmp.bot_id = b.botId;
    CopyOut(b.name, tmp.name, sizeof tmp.name);

    if (!C().Game().GetBotInfo(b, tmp))
        return RMM_FALSE;

    uint32_t callerSize = out->struct_size;
    std::memcpy(out, &tmp, std::min<size_t>(callerSize, sizeof tmp));
    out->struct_size = callerSize;

    return RMM_TRUE;
}

size_t RMM_CALL bot_behaviours(rmm_player_t p, char* out, size_t cap) {
    IGameAdapter::BotRef b;

    if (!C().Bots().Resolve(p, b)) {
        CopyOut("", out, cap);

        return 0;
    }

    std::string all;

    for (auto& n : C().Game().BotBehaviours(b))
        all += (all.empty() ? "" : "\n") + n;

    return CopyOut(all, out, cap);
}

rmm_bool RMM_CALL bot_add(const rmm_bot_spawn* s) {
    if (!s || s->struct_size < offsetof(rmm_bot_spawn, flags) + sizeof s->flags)
        return RMM_FALSE;

    IGameAdapter::BotSpawn spawn;
    spawn.name = S(s->name);
    spawn.team = s->team;
    spawn.kit = S(s->kit);
    spawn.at = (s->flags & RMM_BOT_SPAWN_AT) != 0;
    spawn.pos = s->position;

    if (spawn.at && !Finite(spawn.pos))
        return RMM_FALSE;

    int32_t tag = 0;

    if (s->struct_size >= offsetof(rmm_bot_spawn, personality) + sizeof s->personality)
        spawn.personality = S(s->personality);

    if (s->struct_size >= offsetof(rmm_bot_spawn, tag) + sizeof s->tag)
        tag = s->tag;

    std::string err;

    if (C().Bots().Add(spawn, tag, err))
        return RMM_TRUE;

    C().Log().Debug("bots->add refused: %s", err.c_str());

    return RMM_FALSE;
}

rmm_bool RMM_CALL bot_remove(rmm_player_t p) {
    IGameAdapter::BotRef b;

    return C().Game().HasCap(RMM_CAP_BOT_REMOVE) && C().Bots().Resolve(p, b) && C().Game().BotRemove(b);
}

int32_t RMM_CALL bot_remove_all() {
    if (!BotsUp(RMM_CAP_BOT_REMOVE))
        return 0;

    auto bots = C().Game().Bots();

    if (bots.empty())
        return 0;

    if (C().Game().BotRemoveAll())
        return static_cast<int32_t>(bots.size());

    int32_t n = 0;  // no bulk command: one by one

    for (auto& b : bots)
        n += C().Game().BotRemove(b) ? 1 : 0;

    return n;
}

rmm_bool RMM_CALL bot_move_to(rmm_player_t p, rmm_vec3 pos, float radius) {
    IGameAdapter::BotRef b;

    if (!Finite(pos) || !Finite(radius) || !C().Game().HasCap(RMM_CAP_BOT_ORDERS) || !C().Bots().Resolve(p, b))
        return RMM_FALSE;

    return C().Game().BotMoveTo(b, pos, radius);
}

rmm_bool RMM_CALL bot_patrol(rmm_player_t p, const rmm_vec3* points, int32_t count, float radius) {
    if (!points || count < 1 || count > C().Game().BotMaxPatrolPoints() || !Finite(radius))
        return RMM_FALSE;

    std::vector<rmm_vec3> v(points, points + count);

    for (auto& pt : v)
        if (!Finite(pt))
            return RMM_FALSE;

    IGameAdapter::BotRef b;

    if (!C().Game().HasCap(RMM_CAP_BOT_ORDERS) || !C().Bots().Resolve(p, b))
        return RMM_FALSE;

    return C().Game().BotPatrol(b, v, radius);
}

rmm_bool RMM_CALL bot_order(rmm_player_t p, const char* behaviour, const char* modifiers) {
    IGameAdapter::BotRef b;

    if (!behaviour || !*behaviour || !C().Game().HasCap(RMM_CAP_BOT_ORDERS) || !C().Bots().Resolve(p, b))
        return RMM_FALSE;

    return C().Game().BotOrder(b, behaviour, S(modifiers));
}

rmm_bool RMM_CALL bot_get_strategy(int32_t team, char* out, size_t cap) {
    std::string s;

    if (!BotsUp(RMM_CAP_BOT_STRATEGY) || !C().Game().BotGetStrategy(team, s))
        return RMM_FALSE;

    CopyOut(s, out, cap);

    return RMM_TRUE;
}

size_t RMM_CALL bot_strategies(int32_t team, char* out, size_t cap) {
    if (!BotsUp(RMM_CAP_BOT_STRATEGY)) {
        CopyOut("", out, cap);

        return 0;
    }

    std::string all;

    for (auto& n : C().Game().BotStrategies(team))
        all += (all.empty() ? "" : "\n") + n;

    return CopyOut(all, out, cap);
}

rmm_bool RMM_CALL bot_set_strategy(int32_t team, const char* strategy) {
    if (!strategy || !*strategy || !BotsUp(RMM_CAP_BOT_STRATEGY))
        return RMM_FALSE;

    bool ok = C().Game().BotSetStrategy(team, strategy);

    if (ok)
        C().Log().Info("Team %d strategy set to %s", team, strategy);

    return ok;
}

rmm_bool RMM_CALL bot_get_skill(float* skill) {
    float s = 0;

    if (!BotsUp(RMM_CAP_BOT_SKILL) || !C().Game().BotGetSkill(s))
        return RMM_FALSE;

    if (skill)
        *skill = s;

    return RMM_TRUE;
}

rmm_bool RMM_CALL bot_set_skill(float skill) {
    return Finite(skill) && BotsUp(RMM_CAP_BOT_SKILL) && C().Game().BotSetSkill(skill);
}

// Input control lives in the core (no console traffic), so these never ask the
// AI console anything; an id that isn't a bot simply never gets input ticks.
rmm_bool RMM_CALL bot_set_control(rmm_plugin_t self, rmm_player_t p, int32_t mode) {
    REQUIRE_SELF(RMM_FALSE);

    if (!C().Game().HasCap(RMM_CAP_BOT_INPUT))
        return RMM_FALSE;

    if (mode != RMM_BOT_CONTROL_ENGINE) {  // taking control: must be a bot right now
        rmm_player_info pi{};
        pi.struct_size = sizeof pi;

        if (!C().Game().GetPlayer(p, pi) || !pi.is_bot)
            return RMM_FALSE;
    }

    std::string err;

    if (C().BotInput().SetMode(self, p, mode, &err))
        return RMM_TRUE;

    C().Log().Debug("%s: bots->set_control(#%d, %d) refused: %s", self->name.c_str(), p, mode, err.c_str());

    return RMM_FALSE;
}

int32_t RMM_CALL bot_get_control(rmm_player_t p, rmm_plugin_t* owner) {
    Plugin* o = nullptr;
    int32_t m = C().BotInput().Mode(p, &o);

    if (owner)
        *owner = o;

    return m;
}

rmm_bool RMM_CALL bot_set_input(rmm_plugin_t self, rmm_player_t p, const rmm_bot_input* in) {
    REQUIRE_SELF(RMM_FALSE);

    if (!in || in->struct_size < offsetof(rmm_bot_input, value) + sizeof in->value)
        return RMM_FALSE;

    std::string err;

    if (C().BotInput().SetInput(self, p, *in, C().Time(), &err))
        return RMM_TRUE;

    C().Log().Debug("%s: bots->set_input(#%d) refused: %s", self->name.c_str(), p, err.c_str());

    return RMM_FALSE;
}

rmm_bool RMM_CALL bot_get_input(rmm_player_t p, rmm_bot_input* out) {
    if (!out || out->struct_size < offsetof(rmm_bot_input, value) + sizeof out->value)
        return RMM_FALSE;

    rmm_bot_input tmp{};
    tmp.struct_size = sizeof tmp;

    if (!C().BotInput().EngineInput(p, tmp, C().Time()))
        return RMM_FALSE;

    uint32_t callerSize = out->struct_size;
    std::memcpy(out, &tmp, std::min<size_t>(callerSize, sizeof tmp));
    out->struct_size = callerSize;

    return RMM_TRUE;
}

/* =============================== admin ==================================== */

uint32_t RMM_CALL adm_get_flags(rmm_player_t p) { return C().Admin().GetFlags(p); }

rmm_bool RMM_CALL adm_has_flags(rmm_player_t p, uint32_t f) { return C().Admin().HasFlags(p, f); }

rmm_bool RMM_CALL adm_can_target(rmm_player_t a, rmm_player_t t) { return C().Admin().CanTarget(a, t); }

rmm_bool RMM_CALL adm_grant(rmm_player_t p, uint32_t f, int32_t imm) { return C().Admin().Grant(p, f, imm); }

uint32_t RMM_CALL adm_parse_flags(const char* s) { return AdminManager::ParseFlags(S(s)); }

rmm_bool RMM_CALL adm_kick(rmm_player_t p, const char* r, rmm_player_t by) { return C().Admin().Kick(p, S(r), by); }

rmm_bool RMM_CALL adm_ban(rmm_player_t p, int32_t min, const char* r, rmm_player_t by) {
    return C().Admin().Ban(p, min, S(r), by);
}

rmm_bool RMM_CALL adm_ban_identity(int32_t k, const char* v, int32_t min, const char* r, rmm_player_t by) {
    return C().Admin().BanIdentity(k, S(v), min, S(r), by);
}

rmm_bool RMM_CALL adm_unban(int32_t k, const char* v) { return C().Admin().Unban(k, S(v)); }

rmm_bool RMM_CALL adm_is_banned(int32_t k, const char* v, char* reason, size_t cap) {
    std::string r;

    if (!C().Admin().IsBanned(k, S(v), &r))
        return RMM_FALSE;

    CopyOut(r, reason, cap);

    return RMM_TRUE;
}

void RMM_CALL adm_reload() { C().Admin().Load(); }

/* =============================== game ===================================== */

int32_t RMM_CALL gm_id() { return C().Game().GameId(); }

const char* RMM_CALL gm_name() { return C().Game().GameName(); }

const char* RMM_CALL gm_short_name() { return C().Game().ShortName(); }

const char* RMM_CALL gm_mod() {
    static std::string m;
    m = C().Game().ModName();

    return m.c_str();
}

const char* RMM_CALL gm_backend() { return C().Game().Backend(); }

rmm_bool RMM_CALL gm_has_cap(int32_t cap) { return C().Game().HasCap(cap); }

double RMM_CALL gm_time() { return C().Time(); }

int32_t RMM_CALL gm_status() { return C().Game().Status(); }

rmm_bool RMM_CALL gm_current_map(rmm_map_info* out) {
    if (!out)
        return RMM_FALSE;

    rmm_map_info tmp{};
    tmp.struct_size = sizeof tmp;

    if (!C().Game().CurrentMap(tmp))
        return RMM_FALSE;

    uint32_t sz = out->struct_size ? out->struct_size : sizeof tmp;
    std::memcpy(out, &tmp, std::min<size_t>(sz, sizeof tmp));
    out->struct_size = sz;

    return RMM_TRUE;
}

rmm_bool RMM_CALL gm_change_map(const char* map, const char* mode, int32_t size) {
    if (!map || !*map)
        return RMM_FALSE;

    C().Log().Info("Map change requested: %s %s %d", map, S(mode).c_str(), size);

    return C().Game().ChangeMap(map, S(mode), size);
}

rmm_bool RMM_CALL gm_restart_map() { return C().Game().RestartMap(); }

rmm_bool RMM_CALL gm_run_next_map() { return C().Game().RunNextMap(); }

int32_t RMM_CALL gm_maplist_count() { return C().Game().MaplistCount(); }

rmm_bool RMM_CALL gm_maplist_get(int32_t i, rmm_map_info* out) {
    if (!out)
        return RMM_FALSE;

    rmm_map_info tmp{};
    tmp.struct_size = sizeof tmp;

    if (!C().Game().MaplistGet(i, tmp))
        return RMM_FALSE;

    uint32_t sz = out->struct_size ? out->struct_size : sizeof tmp;
    std::memcpy(out, &tmp, std::min<size_t>(sz, sizeof tmp));
    out->struct_size = sz;

    return RMM_TRUE;
}

rmm_bool RMM_CALL gm_maplist_append(const char* m, const char* mode, int32_t size) {
    return m && *m && C().Game().MaplistAppend(m, S(mode), size);
}

rmm_bool RMM_CALL gm_maplist_clear() { return C().Game().MaplistClear(); }

rmm_bool RMM_CALL gm_set_next_map(int32_t i) { return C().Game().SetNextMap(i); }

rmm_bool RMM_CALL gm_say_all_ex(const char* m, const rmm_msg_opts* opts) {
    if (!m)
        return RMM_FALSE;

    MsgOpts o = ReadOpts(opts);

    return C().Chat().Send(ChatQueue::Kind::All, 0, m, o.interval, o.immediate);
}

rmm_bool RMM_CALL gm_say_team_ex(int32_t t, const char* m, const rmm_msg_opts* opts) {
    if (!m)
        return RMM_FALSE;

    MsgOpts o = ReadOpts(opts);

    return C().Chat().Send(ChatQueue::Kind::Team, t, m, o.interval, o.immediate);
}

rmm_bool RMM_CALL gm_say_all(const char* m) { return gm_say_all_ex(m, nullptr); }

rmm_bool RMM_CALL gm_say_team(int32_t t, const char* m) { return gm_say_team_ex(t, m, nullptr); }

rmm_bool RMM_CALL gm_center_message(const char* m) { return m && C().Game().CenterMessage(m); }

int32_t RMM_CALL gm_get_tickets(int32_t t) { return C().Game().GetTickets(t); }

rmm_bool RMM_CALL gm_set_tickets(int32_t t, int32_t n) { return C().Game().SetTickets(t, n); }

rmm_bool RMM_CALL gm_end_round(int32_t winner) {
    if (winner < 0 || winner > 2 || !C().Game().HasCap(RMM_CAP_END_ROUND))
        return RMM_FALSE;

    C().Log().Info("Round end requested (winner: %s)", winner ? Format("team %d", winner).c_str() : "draw");

    return C().Game().EndRoundNow(winner);
}

rmm_bool RMM_CALL gm_get_spawn_timing(rmm_spawn_timing* out) {
    if (!out || out->struct_size < sizeof(rmm_spawn_timing) || !C().Game().HasCap(RMM_CAP_SPAWN_TIMING))
        return RMM_FALSE;

    return C().Game().GetSpawnTiming(*out);
}

rmm_bool RMM_CALL gm_set_spawn_timing(const rmm_spawn_timing* t) {
    if (!t || t->struct_size < sizeof(rmm_spawn_timing) || !C().Game().HasCap(RMM_CAP_SPAWN_TIMING) ||
        !(t->delay >= 0.0f) || !(t->wave_interval >= 0.0f) || t->delay > 3600.0f || t->wave_interval > 3600.0f)
        return RMM_FALSE;

    bool ok = C().Game().SetSpawnTiming(*t);

    if (ok)
        C().Log().Debug("spawn timing: delay %.1f s, waves %s, wave interval %.1f s", t->delay, t->waves ? "on" : "off",
                        t->wave_interval);

    return ok;
}

rmm_bool RMM_CALL gm_object_template(rmm_object_t o, char* out, size_t cap) {
    std::string s;

    if (!C().Game().ObjectTemplate(o, s))
        return RMM_FALSE;

    CopyOut(s, out, cap);

    return RMM_TRUE;
}

}  // namespace

const rmm_api* BuildApiTables() {
    static rmm_core_api core{sizeof(rmm_core_api), core_version,      core_log,
                             core_build_path,      core_plugin_name,  core_find_plugin,
                             core_request_unload,  core_timer_create, core_timer_kill,
                             core_iface_register,  core_iface_query};
    static rmm_events_api events{sizeof(rmm_events_api), ev_hook, ev_unhook, ev_flags, ev_name, ev_find,
                                 ev_register_custom,     ev_fire};
    static rmm_vars_api vars{sizeof(rmm_vars_api),
                             var_engine_get,
                             var_engine_set,
                             var_engine_watch,
                             var_setting_var,
                             var_setting_get,
                             var_setting_set,
                             var_create,
                             var_find,
                             var_set_bounds,
                             var_get_string,
                             var_get_int,
                             var_get_float,
                             var_get_bool,
                             var_set_string,
                             var_reset,
                             var_hook_change,
                             var_unhook_change,
                             var_get,
                             var_set};
    static rmm_cmds_api cmds{sizeof(rmm_cmds_api), cmd_add,    cmd_remove, cmd_argc,  cmd_argv,        cmd_args,
                             cmd_args_from,        cmd_caller, cmd_source, cmd_reply, cmd_engine_exec, cmd_exec,
                             cmd_reply_ex};
    static rmm_players_api players{sizeof(rmm_players_api),
                                   pl_max_players,
                                   pl_count,
                                   pl_list,
                                   pl_is_connected,
                                   pl_get_info,
                                   pl_find_by_name,
                                   pl_target,
                                   pl_kill,
                                   pl_set_team,
                                   pl_message,
                                   pl_set_score,
                                   pl_teleport,
                                   pl_damage,
                                   pl_get_health,
                                   pl_push,
                                   pl_message_ex,
                                   pl_set_kit,
                                   pl_enter_vehicle,
                                   pl_exit_vehicle,
                                   pl_get_items,
                                   pl_select_item,
                                   pl_remove_item};
    static rmm_admin_api admin{sizeof(rmm_admin_api), adm_get_flags, adm_has_flags, adm_can_target,   adm_grant,
                               adm_parse_flags,       adm_kick,      adm_ban,       adm_ban_identity, adm_unban,
                               adm_is_banned,         adm_reload};
    static rmm_game_api game{sizeof(rmm_game_api),
                             gm_id,
                             gm_name,
                             gm_short_name,
                             gm_mod,
                             gm_backend,
                             gm_has_cap,
                             gm_time,
                             gm_status,
                             gm_current_map,
                             gm_change_map,
                             gm_restart_map,
                             gm_run_next_map,
                             gm_maplist_count,
                             gm_maplist_get,
                             gm_maplist_append,
                             gm_maplist_clear,
                             gm_set_next_map,
                             gm_say_all,
                             gm_say_team,
                             gm_center_message,
                             gm_get_tickets,
                             gm_set_tickets,
                             gm_object_template,
                             gm_say_all_ex,
                             gm_say_team_ex,
                             gm_end_round,
                             gm_get_spawn_timing,
                             gm_set_spawn_timing};
    static rmm_world_api world{sizeof(rmm_world_api), wd_find_vehicles,     wd_is_valid,       wd_get_info,
                               wd_occupants,          wd_set_position,      wd_set_rotation,   wd_set_velocity,
                               wd_find_objects,       wd_spawn_object,      wd_remove_object,  wd_ground_height,
                               wd_template_exists,    wd_damage_object,     wd_control_points, wd_get_control_point,
                               wd_set_control_point,  wd_lock_control_point};
    static rmm_bots_api bots{sizeof(rmm_bots_api), bot_available,  bot_count,     bot_list,         bot_is_bot,
                             bot_get_info,         bot_behaviours, bot_add,       bot_remove,       bot_remove_all,
                             bot_move_to,          bot_patrol,     bot_order,     bot_get_strategy, bot_strategies,
                             bot_set_strategy,     bot_get_skill,  bot_set_skill, bot_set_control,  bot_get_control,
                             bot_set_input,        bot_get_input};
    static rmm_api api{sizeof(rmm_api),
                       RMM_API_VERSION_MAJOR,
                       RMM_API_VERSION_MINOR,
                       &core,
                       &events,
                       &vars,
                       &cmds,
                       &players,
                       &admin,
                       &game,
                       &world,
                       &bots};

    return &api;
}

}  // namespace rmm
