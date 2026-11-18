/**
 * @file rmm.hpp
 * @brief Header-only C++17 convenience layer over the RMM C API.
 *
 * Nothing here is required - it only wraps the C tables in lambdas, RAII-ish
 * handles and std::string. The ABI between plugin and core stays pure C, so a
 * plugin built with any compiler/CRT works with any RMM build.
 *
 * @code
 * #include <rmm/rmm.hpp>
 *
 * class Hello : public rmm::Plugin {
 *     rmm::Var greeting_;
 * public:
 *     bool OnLoad(bool, std::string&) override {
 *         greeting_ = rmm::Var("hello.greeting", "Welcome, %s!", "Join message", RMM_VAR_ARCHIVE);
 *         rmm::On<rmm_ev_player>(RMM_EV_PLAYER_AUTH, [this](rmm_ev_player& e) {
 *             if (auto p = rmm::GetPlayer(e.player))
 *                 rmm::SayAll(rmm::Format(greeting_.Str().c_str(), p->name));
 *             return RMM_CONTINUE;
 *         });
 *         return true;
 *     }
 * };
 * RMM_DECLARE_PLUGIN(Hello, "Hello", "1.0", "me", "Greets players", "", "hello", RMM_GAME_ANY, 0)
 * @endcode
 */
#ifndef RMM_HPP
#define RMM_HPP

#include "rmm.h"

#include <cstdarg>
#include <cstdio>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <algorithm>

namespace rmm {

inline const rmm_api* g_api = nullptr;
inline rmm_plugin_t g_self = nullptr;

inline const rmm_api& Api() { return *g_api; }

/* ---- Formatting & logging ---- */

inline std::string VFormat(const char* fmt, va_list ap) {
    va_list ap2;
    va_copy(ap2, ap);
    int n = std::vsnprintf(nullptr, 0, fmt, ap2);
    va_end(ap2);

    if (n <= 0)
        return {};

    std::string s(static_cast<size_t>(n), '\0');
    std::vsnprintf(&s[0], s.size() + 1, fmt, ap);

    return s;
}

inline std::string Format(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::string s = VFormat(fmt, ap);
    va_end(ap);

    return s;
}

inline void Log(int level, const std::string& msg) { g_api->core->log(g_self, level, msg.c_str()); }

#define RMM_DEF_LOG(fn, lvl)                                                                                           \
    inline void fn(const char* fmt, ...) {                                                                             \
        va_list ap;                                                                                                    \
        va_start(ap, fmt);                                                                                             \
        Log(lvl, VFormat(fmt, ap));                                                                                    \
        va_end(ap);                                                                                                    \
    }
RMM_DEF_LOG(Debug, RMM_LOG_DEBUG)
RMM_DEF_LOG(Info, RMM_LOG_INFO)
RMM_DEF_LOG(Warn, RMM_LOG_WARN)
RMM_DEF_LOG(Error, RMM_LOG_ERROR)
#undef RMM_DEF_LOG

inline std::string Path(rmm_path_kind kind, const std::string& rel = {}) {
    char buf[512];
    g_api->core->build_path(kind, rel.c_str(), buf, sizeof buf);

    return buf;
}

/* ---- Closure storage: freed automatically when the plugin unloads ---- */

namespace detail {
struct ClosureBase {
    virtual ~ClosureBase() = default;
};

template <class F> struct Closure : ClosureBase {
    F fn;

    explicit Closure(F f) : fn(std::move(f)) {}
};

// Deliberately leaked (never destroyed): at process exit the plugin's static
// destructors can run *before* RMM's at-exit shutdown calls RMM_Unload, which
// then clears this list. Clearing an already destroyed vector there crashes
// the server on exit.
inline std::vector<std::unique_ptr<ClosureBase>>& Closures() {
    static auto* v = new std::vector<std::unique_ptr<ClosureBase>>();

    return *v;
}

template <class F> Closure<F>* Keep(F f) {
    auto c = std::make_unique<Closure<F>>(std::move(f));
    auto* raw = c.get();
    Closures().push_back(std::move(c));

    return raw;
}
}  // namespace detail

/* ---- Events ---- */

/**
 * Hook an event with a typed lambda: `rmm_result(T& payload)`.
 * Defaults to the POST phase (observe). Use RMM_PHASE_PRE to block/modify.
 */
template <class T>
rmm_handle_t On(int32_t event_id, std::function<rmm_result(T&)> fn, int32_t phase = RMM_PHASE_POST,
                int32_t priority = RMM_PRIORITY_NORMAL) {
    using Fn = std::function<rmm_result(T&)>;
    auto* c = detail::Keep<Fn>(std::move(fn));

    return g_api->events->hook(
        g_self, event_id, phase, priority,
        [](int32_t, int32_t, void* data, void* user) -> int32_t {
            return static_cast<detail::Closure<Fn>*>(user)->fn(*static_cast<T*>(data));
        },
        c);
}

/** Hook an event that has no payload (e.g. RMM_EV_SERVER_START). */
inline rmm_handle_t On(int32_t event_id, std::function<rmm_result()> fn, int32_t phase = RMM_PHASE_POST,
                       int32_t priority = RMM_PRIORITY_NORMAL) {
    using Fn = std::function<rmm_result()>;
    auto* c = detail::Keep<Fn>(std::move(fn));

    return g_api->events->hook(
        g_self, event_id, phase, priority,
        [](int32_t, int32_t, void*, void* user) -> int32_t { return static_cast<detail::Closure<Fn>*>(user)->fn(); },
        c);
}

inline bool EventSupported(int32_t id) { return (g_api->events->flags(id) & RMM_EVF_SUPPORTED) != 0; }

inline bool EventBlockable(int32_t id) { return (g_api->events->flags(id) & RMM_EVF_BLOCKABLE) != 0; }

/* ---- Commands and chat pacing ---- */

/** rmm.chatInterval between lines (the default). */
inline rmm_msg_opts PaceDefault() {
    rmm_msg_opts o = RMM_MSG_OPTS_INIT;

    return o;
}

/** Leave @p seconds before the next line to the same destination (0 = none). */
inline rmm_msg_opts PaceEvery(float seconds) {
    rmm_msg_opts o = RMM_MSG_OPTS_INIT;
    o.interval = seconds;

    return o;
}

/** Send now, ahead of lines already queued (urgent notices). */
inline rmm_msg_opts PaceImmediate() {
    rmm_msg_opts o = RMM_MSG_OPTS_INIT;
    o.flags = RMM_MSG_IMMEDIATE;

    return o;
}

class CmdArgs {
public:
    explicit CmdArgs(const rmm_cmd_ctx* ctx) : ctx_(ctx) {}

    /** Pacing for every following Reply()/Replyf() on this object, e.g.
     *  `a.Pace(rmm::PaceEvery(2.0f));` before a long listing. */
    CmdArgs& Pace(const rmm_msg_opts& o) {
        pace_ = o;

        return *this;
    }

    /** One reply with its own pacing (overrides Pace()). */
    void Reply(const std::string& m, const rmm_msg_opts& o) const { g_api->cmds->reply_ex(ctx_, m.c_str(), &o); }

    int Count() const { return g_api->cmds->argc(ctx_); }

    std::string Arg(int i) const { return g_api->cmds->argv(ctx_, i); }

    std::string Name() const { return Arg(0); }

    std::string All() const { return g_api->cmds->args(ctx_); }

    std::string From(int first) const { return g_api->cmds->args_from(ctx_, first); }

    rmm_player_t Caller() const { return g_api->cmds->caller(ctx_); }

    int32_t Source() const { return g_api->cmds->source(ctx_); }

    void Reply(const std::string& m) const { Reply(m, pace_); }

    void Replyf(const char* fmt, ...) const {
        va_list ap;
        va_start(ap, fmt);
        Reply(VFormat(fmt, ap));
        va_end(ap);
    }

    const rmm_cmd_ctx* Raw() const { return ctx_; }

private:
    const rmm_cmd_ctx* ctx_;
    rmm_msg_opts pace_ = RMM_MSG_OPTS_INIT;
};

using CommandFn = std::function<void(CmdArgs&)>;

inline rmm_handle_t Command(const char* name, uint32_t flags, uint32_t admin_flags, const char* description,
                            const char* usage, CommandFn fn) {
    auto* c = detail::Keep<CommandFn>(std::move(fn));

    return g_api->cmds->add(
        g_self, name, flags, admin_flags, description, usage,
        [](const rmm_cmd_ctx* ctx, void* user) {
            CmdArgs a(ctx);
            static_cast<detail::Closure<CommandFn>*>(user)->fn(a);
        },
        c);
}

inline std::string EngineExec(const std::string& cmd) {
    char out[4096] = {0};
    g_api->cmds->engine_exec(cmd.c_str(), out, sizeof out);

    return out;
}

/* ---- Variables ---- */

class Var {
public:
    Var() = default;

    Var(const char* name, const char* def, const char* desc, uint32_t flags = 0)
        : h_(g_api->vars->create(g_self, name, def, desc, flags)) {}

    explicit Var(rmm_handle_t h) : h_(h) {}

    bool Valid() const { return h_ != RMM_INVALID_HANDLE; }

    rmm_handle_t Handle() const { return h_; }

    std::string Str() const {
        char buf[1024];
        g_api->vars->get_string(h_, buf, sizeof buf);

        return buf;
    }

    int32_t Int() const { return g_api->vars->get_int(h_); }

    double Float() const { return g_api->vars->get_float(h_); }

    bool Bool() const { return g_api->vars->get_bool(h_) != 0; }

    bool Set(const std::string& v) const { return g_api->vars->set_string(g_self, h_, v.c_str()) != 0; }

    Var& Bounds(std::optional<double> lo, std::optional<double> hi) {
        g_api->vars->set_bounds(h_, lo.has_value(), lo.value_or(0), hi.has_value(), hi.value_or(0));

        return *this;
    }

    using ChangeFn = std::function<void(const std::string& old_v, const std::string& new_v)>;

    Var& OnChange(ChangeFn fn) {
        auto* c = detail::Keep<ChangeFn>(std::move(fn));
        g_api->vars->hook_change(
            g_self, h_,
            [](rmm_handle_t, const char* o, const char* n, void* user) {
                static_cast<detail::Closure<ChangeFn>*>(user)->fn(o ? o : "", n ? n : "");
            },
            c);

        return *this;
    }

private:
    rmm_handle_t h_ = RMM_INVALID_HANDLE;
};

inline std::optional<std::string> GetSetting(rmm_setting s) {
    char buf[512];

    if (!g_api->vars->setting_get(s, buf, sizeof buf))
        return std::nullopt;

    return std::string(buf);
}

inline bool SetSetting(rmm_setting s, const std::string& v) { return g_api->vars->setting_set(s, v.c_str()) != 0; }

/* ---- Timers ---- */

/** Repeat @p fn every @p seconds while it returns true. */
inline rmm_handle_t Every(double seconds, std::function<bool()> fn, uint32_t extra_flags = 0) {
    using Fn = std::function<bool()>;
    auto* c = detail::Keep<Fn>(std::move(fn));

    return g_api->core->timer_create(
        g_self, seconds, RMM_TIMER_REPEAT | extra_flags,
        [](rmm_handle_t, void* user) -> rmm_bool {
            return static_cast<detail::Closure<Fn>*>(user)->fn() ? RMM_TRUE : RMM_FALSE;
        },
        c);
}

/** Run @p fn once after @p seconds. */
inline rmm_handle_t After(double seconds, std::function<void()> fn, uint32_t extra_flags = 0) {
    using Fn = std::function<void()>;
    auto* c = detail::Keep<Fn>(std::move(fn));

    return g_api->core->timer_create(
        g_self, seconds, extra_flags,
        [](rmm_handle_t, void* user) -> rmm_bool {
            static_cast<detail::Closure<Fn>*>(user)->fn();

            return RMM_FALSE;
        },
        c);
}

/* ---- Players, admin, game ---- */

inline std::optional<rmm_player_info> GetPlayer(rmm_player_t id) {
    rmm_player_info info{};
    info.struct_size = sizeof info;

    if (!g_api->players->get_info(id, &info))
        return std::nullopt;

    return info;
}

inline std::vector<rmm_player_t> Players() {
    std::vector<rmm_player_t> v(
        static_cast<size_t>(g_api->players->max_players() > 0 ? g_api->players->max_players() : 256));
    int32_t n = g_api->players->list(v.data(), static_cast<int32_t>(v.size()));
    v.resize(
        static_cast<size_t>(n < 0 ? 0 : (n > static_cast<int32_t>(v.size()) ? static_cast<int32_t>(v.size()) : n)));

    return v;
}

/** Resolve a target string; on failure replies the error to @p args and
 *  returns an empty vector. */
inline std::vector<rmm_player_t> Target(const CmdArgs& args, const std::string& pattern, uint32_t flags = 0) {
    rmm_player_t buf[256];
    char err[256] = {0};
    int32_t n = g_api->players->target(pattern.c_str(), args.Caller(), flags, buf, 256, err, sizeof err);

    if (n <= 0) {
        args.Reply(err[0] ? err : "No matching player.");

        return {};
    }

    return std::vector<rmm_player_t>(buf, buf + n);
}

inline std::string PlayerName(rmm_player_t id) {
    if (id == RMM_PLAYER_NONE)
        return "Console";

    auto p = GetPlayer(id);

    return p ? p->name : "#" + std::to_string(id);
}

inline bool HasCap(rmm_cap c) { return g_api->game->has_cap(c) != 0; }

inline bool SayAll(const std::string& m) { return g_api->game->say_all(m.c_str()) != 0; }

/** Chat to everyone / one team / one player with pacing options (see ::rmm_msg_opts). */
inline bool SayAll(const std::string& m, const rmm_msg_opts& o) { return g_api->game->say_all_ex(m.c_str(), &o) != 0; }

inline bool SayTeam(int32_t team, const std::string& m, const rmm_msg_opts& o = PaceDefault()) {
    return g_api->game->say_team_ex(team, m.c_str(), &o) != 0;
}

inline bool Message(rmm_player_t p, const std::string& m, const rmm_msg_opts& o = PaceDefault()) {
    return g_api->players->message_ex(p, m.c_str(), &o) != 0;
}

inline bool HasFlags(rmm_player_t p, uint32_t f) { return g_api->admin->has_flags(p, f) != 0; }

/** Damage a player (RMM_CAP_DAMAGE_PLAYER). Doesn't raise RMM_EV_PLAYER_DAMAGE. */
inline bool DamagePlayer(rmm_player_t p, float amount, rmm_player_t attacker = RMM_PLAYER_NONE) {
    return RMM_HAS(g_api->players, rmm_players_api, damage) && g_api->players->damage(p, amount, attacker) != 0;
}

struct Health {
    float current;
    float max;
};

/** Current/max hit points of a living player (RMM_CAP_PLAYER_HEALTH). */
inline std::optional<Health> GetHealth(rmm_player_t p) {
    Health h{0, -1};

    if (!RMM_HAS(g_api->players, rmm_players_api, get_health) || !g_api->players->get_health(p, &h.current, &h.max))
        return std::nullopt;

    return h;
}

/** Add @p velocity (m/s, Y up) to a living player, or to their whole vehicle
 *  if they're in one (RMM_CAP_PLAYER_PUSH). */
inline bool PushPlayer(rmm_player_t p, rmm_vec3 velocity) {
    return RMM_HAS(g_api->players, rmm_players_api, push) && g_api->players->push(p, velocity) != 0;
}

/** Replace a living on-foot player's kit (RMM_CAP_SET_KIT). */
inline bool SetKit(rmm_player_t p, const std::string& kitTemplate) {
    return RMM_HAS(g_api->players, rmm_players_api, set_kit) && g_api->players->set_kit(p, kitTemplate.c_str()) != 0;
}

/** Put a living player into @p vehicle at @p seat (-1 = first free; RMM_CAP_PUT_IN_VEHICLE). */
inline bool EnterVehicle(rmm_player_t p, rmm_object_t vehicle, int32_t seat = -1) {
    return RMM_HAS(g_api->players, rmm_players_api, enter_vehicle) &&
           g_api->players->enter_vehicle(p, vehicle, seat) != 0;
}

/** Take a living player out of their vehicle (RMM_CAP_PUT_IN_VEHICLE). */
inline bool ExitVehicle(rmm_player_t p) {
    return RMM_HAS(g_api->players, rmm_players_api, exit_vehicle) && g_api->players->exit_vehicle(p) != 0;
}

/** The items a living on-foot player carries, in slot order; empty if none
 *  or unknown (RMM_CAP_ITEMS). */
inline std::vector<rmm_item_info> Items(rmm_player_t p) {
    std::vector<rmm_item_info> v;

    if (!RMM_HAS(g_api->players, rmm_players_api, get_items))
        return v;

    v.resize(16);
    int32_t n = g_api->players->get_items(p, v.data(), static_cast<int32_t>(v.size()));

    if (n > static_cast<int32_t>(v.size())) {
        v.resize(static_cast<size_t>(n));
        n = g_api->players->get_items(p, v.data(), n);
    }

    v.resize(n > 0 ? static_cast<size_t>(std::min<int32_t>(n, static_cast<int32_t>(v.size()))) : 0);

    return v;
}

/** Make a player hold the item in @p slot (RMM_CAP_ITEMS). */
inline bool SelectItem(rmm_player_t p, int32_t slot) {
    return RMM_HAS(g_api->players, rmm_players_api, select_item) && g_api->players->select_item(p, slot) != 0;
}

/** Take the item in @p slot away until their next kit (RMM_CAP_ITEMS). */
inline bool RemoveItem(rmm_player_t p, int32_t slot) {
    return RMM_HAS(g_api->players, rmm_players_api, remove_item) && g_api->players->remove_item(p, slot) != 0;
}

/** End the round now with @p winner (1, 2, or RMM_TEAM_NONE for a draw; RMM_CAP_END_ROUND). */
inline bool EndRound(int32_t winner) {
    return RMM_HAS(g_api->game, rmm_game_api, end_round) && g_api->game->end_round(winner) != 0;
}

/** How soon dead players spawn again (RMM_CAP_SPAWN_TIMING). */
inline std::optional<rmm_spawn_timing> GetSpawnTiming() {
    rmm_spawn_timing t{sizeof(rmm_spawn_timing), 0, 0, 0};

    if (!RMM_HAS(g_api->game, rmm_game_api, get_spawn_timing) || !g_api->game->get_spawn_timing(&t))
        return std::nullopt;

    return t;
}

inline bool SetSpawnTiming(const rmm_spawn_timing& t) {
    return RMM_HAS(g_api->game, rmm_game_api, set_spawn_timing) && g_api->game->set_spawn_timing(&t) != 0;
}

/** Add `\key\value` to the server's query replies (RMM_CAP_QUERY_RULES). */
inline bool SetQueryRule(const std::string& key, const std::string& value) {
    return RMM_HAS(g_api->game, rmm_game_api, set_query_rule) &&
           g_api->game->set_query_rule(g_self, key.c_str(), value.c_str()) != 0;
}

/** Take @p key back out of the server's query replies. */
inline bool RemoveQueryRule(const std::string& key) {
    return RMM_HAS(g_api->game, rmm_game_api, set_query_rule) &&
           g_api->game->set_query_rule(g_self, key.c_str(), nullptr) != 0;
}

/* ---- World ---- */

/** The world table, or nullptr if the core doesn't provide one. */
inline const rmm_world_api* World() { return RMM_HAS(g_api, rmm_api, world) ? g_api->world : nullptr; }

/** Vehicles whose template matches @p pattern (see rmm_world_api::find_vehicles). */
inline std::vector<rmm_object_t> FindVehicles(const std::string& pattern = {}, uint32_t flags = 0) {
    const rmm_world_api* w = World();

    if (!w)
        return {};

    std::vector<rmm_object_t> v(64);
    int32_t n = w->find_vehicles(pattern.c_str(), flags, v.data(), static_cast<int32_t>(v.size()));

    if (n > static_cast<int32_t>(v.size())) {
        v.resize(static_cast<size_t>(n));
        n = w->find_vehicles(pattern.c_str(), flags, v.data(), n);
    }

    v.resize(
        static_cast<size_t>(n < 0 ? 0 : (n > static_cast<int32_t>(v.size()) ? static_cast<int32_t>(v.size()) : n)));

    return v;
}

inline std::optional<rmm_object_info> ObjectInfo(rmm_object_t obj) {
    const rmm_world_api* w = World();
    rmm_object_info info{};
    info.struct_size = sizeof info;

    if (!w || !w->get_info(obj, &info))
        return std::nullopt;

    return info;
}

/** Players anywhere in @p obj's root vehicle. */
inline std::vector<rmm_player_t> Occupants(rmm_object_t obj) {
    const rmm_world_api* w = World();

    if (!w)
        return {};

    rmm_player_t buf[256];
    int32_t n = w->occupants(obj, buf, 256);

    return std::vector<rmm_player_t>(buf, buf + (n < 0 ? 0 : (n > 256 ? 256 : n)));
}

/** Zero the root's linear and angular velocity. */
inline bool StopObject(rmm_object_t obj) {
    const rmm_world_api* w = World();

    return w && w->set_velocity(obj, rmm_vec3{0, 0, 0}, rmm_vec3{0, 0, 0}) != 0;
}

/** Top-level world objects of any kind matching @p pattern on the whole map
 *  (see rmm_world_api::find_objects). Cache the result; don't call per frame. */
inline std::vector<rmm_object_t> FindObjects(const std::string& pattern = {}, uint32_t flags = 0) {
    const rmm_world_api* w = World();

    if (!RMM_HAS(w, rmm_world_api, find_objects))
        return {};

    int32_t n = w->find_objects(pattern.c_str(), nullptr, 0.0f, flags, nullptr, 0);
    std::vector<rmm_object_t> v(static_cast<size_t>(n > 0 ? n : 0));

    if (n > 0)
        n = w->find_objects(pattern.c_str(), nullptr, 0.0f, flags, v.data(), n);

    v.resize(
        static_cast<size_t>(n < 0 ? 0 : (n > static_cast<int32_t>(v.size()) ? static_cast<int32_t>(v.size()) : n)));

    return v;
}

/** As FindObjects, within @p radius metres of @p center, nearest first. */
inline std::vector<rmm_object_t> FindObjectsNear(rmm_vec3 center, float radius, const std::string& pattern = {},
                                                 uint32_t flags = 0) {
    const rmm_world_api* w = World();

    if (!RMM_HAS(w, rmm_world_api, find_objects))
        return {};

    std::vector<rmm_object_t> v(64);
    int32_t n = w->find_objects(pattern.c_str(), &center, radius, flags, v.data(), static_cast<int32_t>(v.size()));

    if (n > static_cast<int32_t>(v.size())) {
        v.resize(static_cast<size_t>(n));
        n = w->find_objects(pattern.c_str(), &center, radius, flags, v.data(), n);
    }

    v.resize(
        static_cast<size_t>(n < 0 ? 0 : (n > static_cast<int32_t>(v.size()) ? static_cast<int32_t>(v.size()) : n)));

    return v;
}

/** Create an object from a template (rmm_world_api::spawn_object). 0 on failure.
 *  @p rotation is yaw/pitch/roll in degrees; RMM_SPAWN_ON_GROUND makes
 *  @p position.y a height above the terrain. */
inline rmm_object_t SpawnObject(const std::string& tmpl, rmm_vec3 position, rmm_vec3 rotation = rmm_vec3{0, 0, 0},
                                uint32_t flags = 0) {
    const rmm_world_api* w = World();

    return RMM_HAS(w, rmm_world_api, spawn_object) ? w->spawn_object(tmpl.c_str(), position, rotation, flags) : 0;
}

/** Remove an object from the server and every client. */
inline bool RemoveObject(rmm_object_t obj) {
    const rmm_world_api* w = World();

    return RMM_HAS(w, rmm_world_api, remove_object) && w->remove_object(obj) != 0;
}

/** Terrain height at (x, z), if known. */
inline std::optional<float> GroundHeight(float x, float z) {
    const rmm_world_api* w = World();
    float h = 0;

    if (!RMM_HAS(w, rmm_world_api, ground_height) || !w->ground_height(x, z, &h))
        return std::nullopt;

    return h;
}

inline bool TemplateExists(const std::string& tmpl) {
    const rmm_world_api* w = World();

    return RMM_HAS(w, rmm_world_api, template_exists) && w->template_exists(tmpl.c_str()) != 0;
}

/** Damage (or, with enough damage, destroy/explode) a vehicle or other armoured
 *  object (RMM_CAP_OBJECT_DAMAGE). */
inline bool DamageObject(rmm_object_t obj, float amount, rmm_player_t attacker = RMM_PLAYER_NONE) {
    const rmm_world_api* w = World();

    return RMM_HAS(w, rmm_world_api, damage_object) && w->damage_object(obj, amount, attacker) != 0;
}

/** Every control point of the running map (RMM_CAP_CONTROL_POINTS). */
inline std::vector<rmm_object_t> ControlPoints() {
    const rmm_world_api* w = World();

    if (!RMM_HAS(w, rmm_world_api, control_points))
        return {};

    int32_t n = w->control_points(nullptr, 0);
    std::vector<rmm_object_t> v(static_cast<size_t>(n > 0 ? n : 0));

    if (n > 0)
        n = w->control_points(v.data(), n);

    v.resize(
        static_cast<size_t>(n < 0 ? 0 : (n > static_cast<int32_t>(v.size()) ? static_cast<int32_t>(v.size()) : n)));

    return v;
}

inline std::optional<rmm_control_point_info> ControlPointInfo(rmm_object_t cp) {
    const rmm_world_api* w = World();
    rmm_control_point_info info{};
    info.struct_size = sizeof info;

    if (!RMM_HAS(w, rmm_world_api, get_control_point) || !w->get_control_point(cp, &info))
        return std::nullopt;

    return info;
}

/** Give a control point to @p team, or 0 = neutral (RMM_CAP_CONTROL_POINT_SET). */
inline bool SetControlPoint(rmm_object_t cp, int32_t team) {
    const rmm_world_api* w = World();

    return RMM_HAS(w, rmm_world_api, set_control_point) && w->set_control_point(cp, team) != 0;
}

/** Make a control point uncapturable, or capturable again (RMM_CAP_CONTROL_POINT_LOCK). */
inline bool LockControlPoint(rmm_object_t cp, bool locked) {
    const rmm_world_api* w = World();

    return RMM_HAS(w, rmm_world_api, lock_control_point) &&
           w->lock_control_point(cp, locked ? RMM_TRUE : RMM_FALSE) != 0;
}

/* ---- Bots ---- */

/** The bots table, or nullptr if the core doesn't provide one. */
inline const rmm_bots_api* Bots() { return RMM_HAS(g_api, rmm_api, bots) ? g_api->bots : nullptr; }

/** Is the AI running right now (BF1942/BFV: co-op)? */
inline bool BotsAvailable() {
    const rmm_bots_api* b = Bots();

    return b && b->available() != 0;
}

/** Player ids of every bot. */
inline std::vector<rmm_player_t> BotList() {
    const rmm_bots_api* b = Bots();

    if (!b)
        return {};

    std::vector<rmm_player_t> v(64);
    int32_t n = b->list(v.data(), static_cast<int32_t>(v.size()));

    if (n > static_cast<int32_t>(v.size())) {
        v.resize(static_cast<size_t>(n));
        n = b->list(v.data(), n);
    }

    v.resize(
        static_cast<size_t>(n < 0 ? 0 : (n > static_cast<int32_t>(v.size()) ? static_cast<int32_t>(v.size()) : n)));

    return v;
}

inline std::optional<rmm_bot_info> BotInfo(rmm_player_t p) {
    const rmm_bots_api* b = Bots();
    rmm_bot_info info{};
    info.struct_size = sizeof info;

    if (!b || !b->get_info(p, &info))
        return std::nullopt;

    return info;
}

namespace detail {
inline std::vector<std::string> SplitLines(const std::string& s) {
    std::vector<std::string> v;
    size_t start = 0;

    while (start < s.size()) {
        size_t nl = s.find('\n', start);

        if (nl == std::string::npos)
            nl = s.size();

        if (nl > start)
            v.push_back(s.substr(start, nl - start));

        start = nl + 1;
    }

    return v;
}

template <class F> inline std::string GrowString(F&& fill) {  // fill(buf, cap) -> full length
    std::string s(256, '\0');
    size_t n = fill(&s[0], s.size());

    if (n >= s.size()) {
        s.assign(n + 1, '\0');
        n = fill(&s[0], s.size());
    }

    s.resize(n < s.size() ? n : s.size() - 1);

    return s;
}
}  // namespace detail

/** Behaviour names a bot can be ordered into. */
inline std::vector<std::string> BotBehaviours(rmm_player_t p) {
    const rmm_bots_api* b = Bots();

    if (!b)
        return {};

    return detail::SplitLines(detail::GrowString([&](char* buf, size_t cap) { return b->behaviours(p, buf, cap); }));
}

/** Strategies @p team's strategic AI can use. */
inline std::vector<std::string> BotStrategies(int32_t team) {
    const rmm_bots_api* b = Bots();

    if (!b)
        return {};

    return detail::SplitLines(detail::GrowString([&](char* buf, size_t cap) { return b->strategies(team, buf, cap); }));
}

inline std::optional<std::string> BotStrategy(int32_t team) {
    const rmm_bots_api* b = Bots();
    char buf[128];

    if (!b || !b->get_strategy(team, buf, sizeof buf))
        return std::nullopt;

    return std::string(buf);
}

/** Start adding a bot; the result arrives as RMM_EV_BOT_ADDED / RMM_EV_BOT_ADD_FAILED with @p tag. */
inline bool AddBot(const std::string& name, int32_t team, const std::string& kit = {}, int32_t tag = 0,
                   const rmm_vec3* at = nullptr) {
    const rmm_bots_api* b = Bots();

    if (!b)
        return false;

    rmm_bot_spawn s = RMM_BOT_SPAWN_INIT;
    s.name = name.c_str();
    s.team = team;
    s.kit = kit.empty() ? nullptr : kit.c_str();
    s.tag = tag;

    if (at) {
        s.position = *at;
        s.flags |= RMM_BOT_SPAWN_AT;
    }

    return b->add(&s) != 0;
}

/* ---- Bot input control ---- */

/** Take over a bot's controls (RMM_BOT_CONTROL_OVERRIDE / _EXCLUSIVE) or hand
 *  them back to the engine AI (RMM_BOT_CONTROL_ENGINE). */
inline bool SetBotControl(rmm_player_t p, int32_t mode) {
    const rmm_bots_api* b = Bots();

    return RMM_HAS(b, rmm_bots_api, set_control) && b->set_control(g_self, p, mode) != 0;
}

/** Builder for rmm_bot_input: `BotInput().Set(RMM_BI_THROTTLE, 1).Press(RMM_BI_FIRE).Hold(200)`. */
struct BotInput {
    rmm_bot_input in{};

    BotInput() { in.struct_size = sizeof in; }

    BotInput& Set(int slot, float v) {
        if (slot >= 0 && slot < RMM_BOT_INPUT_SLOTS) {
            in.value[slot] = v;
            in.mask |= RMM_BI_BIT(slot);
        }

        return *this;
    }

    BotInput& Press(int slot, bool down = true) { return Set(slot, down ? 1.0f : 0.0f); }

    BotInput& Hold(uint32_t ms) {
        in.hold_ms = ms;

        return *this;
    }
};

/** Latch input for a bot you control (see rmm_bots_api::set_input). */
inline bool SetBotInput(rmm_player_t p, const BotInput& input) {
    const rmm_bots_api* b = Bots();

    return RMM_HAS(b, rmm_bots_api, set_input) && b->set_input(g_self, p, &input.in) != 0;
}

/** What the engine's AI sent on its latest tick for this bot. */
inline std::optional<rmm_bot_input> BotEngineInput(rmm_player_t p) {
    const rmm_bots_api* b = Bots();
    rmm_bot_input in{};
    in.struct_size = sizeof in;

    if (!RMM_HAS(b, rmm_bots_api, get_input) || !b->get_input(p, &in))
        return std::nullopt;

    return in;
}

/* ---- Plugin base & export macro ---- */

class Plugin {
public:
    virtual ~Plugin() = default;
    /** Return false and set @p error to refuse loading. */
    virtual bool OnLoad(bool late, std::string& error) = 0;

    virtual void OnAllLoaded() {}

    virtual void OnPause(bool /*paused*/) {}

    virtual void OnUnload() {}
};

}  // namespace rmm

/**
 * Declare the plugin class and generate the exported C entry points.
 * Use exactly once per plugin, at namespace scope.
 */
#define RMM_DECLARE_PLUGIN(Class, Name, Version, Author, Desc, Url, Tag, Games, Flags)                                 \
    static const rmm_plugin_info rmm_g_info = {                                                                        \
        RMM_PLUGIN_INFO_HEADER, Name, Version, Author, Desc, Url, Tag, (uint32_t)(Games), (uint32_t)(Flags)};          \
    static Class* rmm_g_instance = nullptr;                                                                            \
    RMM_PLUGIN_EXPORT const rmm_plugin_info* RMM_CALL RMM_Query(void) { return &rmm_g_info; }                          \
    RMM_PLUGIN_EXPORT rmm_bool RMM_CALL RMM_Load(const rmm_api* api, rmm_plugin_t self, rmm_bool late, char* error,    \
                                                 size_t cap) {                                                         \
        ::rmm::g_api = api;                                                                                            \
        ::rmm::g_self = self;                                                                                          \
        rmm_g_instance = new Class();                                                                                  \
        std::string err;                                                                                               \
        if (!rmm_g_instance->OnLoad(late != 0, err)) {                                                                 \
            std::snprintf(error, cap, "%s", err.empty() ? "OnLoad failed" : err.c_str());                              \
            delete rmm_g_instance;                                                                                     \
            rmm_g_instance = nullptr;                                                                                  \
            ::rmm::detail::Closures().clear();                                                                         \
            return RMM_FALSE;                                                                                          \
        }                                                                                                              \
        return RMM_TRUE;                                                                                               \
    }                                                                                                                  \
    RMM_PLUGIN_EXPORT void RMM_CALL RMM_AllLoaded(void) {                                                              \
        if (rmm_g_instance)                                                                                            \
            rmm_g_instance->OnAllLoaded();                                                                             \
    }                                                                                                                  \
    RMM_PLUGIN_EXPORT void RMM_CALL RMM_Pause(rmm_bool p) {                                                            \
        if (rmm_g_instance)                                                                                            \
            rmm_g_instance->OnPause(p != 0);                                                                           \
    }                                                                                                                  \
    RMM_PLUGIN_EXPORT void RMM_CALL RMM_Unload(void) {                                                                 \
        if (rmm_g_instance) {                                                                                          \
            rmm_g_instance->OnUnload();                                                                                \
            delete rmm_g_instance;                                                                                     \
            rmm_g_instance = nullptr;                                                                                  \
        }                                                                                                              \
        ::rmm::detail::Closures().clear();                                                                             \
    }

#endif /* RMM_HPP */
