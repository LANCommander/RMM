// Built-in "rmm" management command and "help". Equivalent to Metamod's
// "meta" and SourceMod's "sm" commands.
#include "core.h"
#include "platform.h"
#include "util.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <map>
#include <set>
#include <sstream>

namespace rmm {

static const char* kSettingNames[RMM_SETTING_COUNT] = {
    "server_name",     "password",     "max_players",   "time_limit",     "score_limit",    "ticket_ratio",
    "rounds_per_map",  "spawn_time",   "man_down_time", "friendly_fire",  "vehicle_ff",     "team_ratio",
    "autobalance",     "gravity",      "game_speed",    "bot_count",      "bot_ratio",      "bot_skill",
    "welcome_message", "voip_enabled", "punkbuster",    "allow_nose_cam", "external_views", "kick_tk_count",
};

static const char* kCapNames[RMM_CAP_COUNT] = {
    "console_exec",   "console_output",  "console_hook",      "kick",
    "engine_ban",     "kill_player",     "set_team",          "private_message",
    "set_score",      "teleport",        "change_map",        "maplist",
    "tickets",        "player_position", "auth_id",           "bots",
    "center_message", "damage_player",   "player_health",     "world_objects",
    "object_move",    "object_rotation", "object_velocity",   "player_push",
    "object_query",   "object_spawn",    "object_remove",     "ground_height",
    "bot_query",      "bot_add",         "bot_remove",        "bot_orders",
    "bot_strategy",   "bot_skill",       "bot_input",         "set_kit",
    "end_round",      "put_in_vehicle",  "object_damage",     "items",
    "spawn_timing",   "control_points",  "control_point_set", "control_point_lock",
};

static_assert(sizeof(kCapNames) / sizeof(kCapNames[0]) == RMM_CAP_COUNT, "kCapNames out of sync with rmm_cap");

const char* SettingKey(int32_t s) { return (s >= 0 && s < RMM_SETTING_COUNT) ? kSettingNames[s] : nullptr; }

const char* CapKey(int32_t c) { return (c >= 0 && c < RMM_CAP_COUNT) ? kCapNames[c] : nullptr; }

static float DistSq(const rmm_vec3& a, const rmm_vec3& b) {
    float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;

    return dx * dx + dy * dy + dz * dz;
}

std::vector<IGameAdapter::ObjectRef> FilterObjects(std::vector<IGameAdapter::ObjectRef> all, const std::string& pattern,
                                                   const rmm_vec3* center, float radius, uint32_t flags) {
    const float r2 = radius * radius;
    auto drop = [&](const IGameAdapter::ObjectRef& o) {
        if ((flags & RMM_FIND_NO_VEHICLES) && o.isVehicle)
            return true;

        if ((flags & RMM_FIND_NO_SOLDIERS) && o.isSoldier)
            return true;

        if (center && DistSq(o.position, *center) > r2)
            return true;

        return !GlobMatch(pattern, o.templateName);
    };

    all.erase(std::remove_if(all.begin(), all.end(), drop), all.end());

    if (center)
        std::stable_sort(all.begin(), all.end(),
                         [&](const IGameAdapter::ObjectRef& a, const IGameAdapter::ObjectRef& b) {
                             return DistSq(a.position, *center) < DistSq(b.position, *center);
                         });

    return all;
}

namespace {

void Reply(const rmm_cmd_ctx* ctx, const std::string& m) { Core::Get().Cmds().Reply(ctx, m); }

bool Allowed(const rmm_cmd_ctx* ctx, uint32_t flags) {
    if (Core::Get().Admin().HasFlags(ctx->caller, flags))
        return true;

    Reply(ctx, "You do not have access to this command.");

    return false;
}

bool Deferred() { return Core::Get().CallDepth() > 0 || Core::Get().Events().Depth() > 0; }

void CmdPlugins(const rmm_cmd_ctx* ctx) {
    Core& core = Core::Get();
    auto& pm = core.Plugins();
    std::string sub = ctx->argv.size() > 2 ? ToLower(ctx->argv[2]) : "list";
    std::string target = ctx->argv.size() > 3 ? ctx->argv[3] : "";

    if (sub == "list") {
        if (pm.All().empty()) {
            Reply(ctx, "No plugins loaded.");

            return;
        }

        for (auto& p : pm.All())
            Reply(ctx, Format("#%u %-20s %-10s %-8s %s", p->id, p->name.c_str(), p->version.c_str(),
                              PluginStateName(p->state), p->file.c_str()));

        return;
    }

    if (sub == "info") {
        Plugin* p = pm.Find(target);

        if (!p) {
            Reply(ctx, "No such plugin.");

            return;
        }

        Reply(ctx, Format("#%u %s %s by %s [%s]", p->id, p->name.c_str(), p->version.c_str(), p->author.c_str(),
                          PluginStateName(p->state)));

        if (!p->description.empty())
            Reply(ctx, "  " + p->description);

        if (!p->url.empty())
            Reply(ctx, "  " + p->url);

        Reply(ctx, Format("  file %s, API %u.%u", p->path.c_str(), p->apiMajor, p->apiMinor));

        if (!p->error.empty())
            Reply(ctx, "  last error: " + p->error);

        return;
    }

    if (!Allowed(ctx, RMM_ADMFLAG_PLUGINS))
        return;

    if (target.empty()) {
        Reply(ctx, "Usage: rmm plugins <load|unload|reload|pause|resume> <plugin>");

        return;
    }

    if (sub == "load") {
        if (Deferred()) {
            pm.RequestLoad(target);
            Reply(ctx, "Load queued for end of frame.");

            return;
        }

        std::string err;
        Plugin* p = pm.Load(target, true, err);
        Reply(ctx, p ? Format("Loaded %s %s.", p->name.c_str(), p->version.c_str()) : "Load failed: " + err);

        return;
    }

    Plugin* p = pm.Find(target);

    if (!p) {
        Reply(ctx, "No such plugin: " + target);

        return;
    }

    std::string err;

    if (sub == "unload") {
        if (Deferred()) {
            pm.RequestUnload(p, "admin request");
            Reply(ctx, "Unload queued.");

            return;
        }

        std::string name = p->name;
        Reply(ctx, pm.Unload(p, false, err) ? "Unloaded " + name + "." : "Unload failed: " + err);
    } else if (sub == "reload") {
        if (Deferred()) {
            pm.RequestReload(p);
            Reply(ctx, "Reload queued.");

            return;
        }

        Reply(ctx, pm.Reload(p, err) ? "Reloaded." : "Reload failed: " + err);
    } else if (sub == "pause" || sub == "resume") {
        bool ok = pm.SetPaused(p, sub == "pause", err);
        Reply(ctx, ok ? p->name + " is now " + PluginStateName(p->state) + "." : "Failed: " + err);
    } else {
        Reply(ctx, "Unknown subcommand. Use list|info|load|unload|reload|pause|resume.");
    }
}

void CmdVar(const rmm_cmd_ctx* ctx) {
    Core& core = Core::Get();

    if (ctx->argv.size() < 3) {
        Reply(ctx, "Usage: rmm var <name> [value]");

        return;
    }

    const std::string& name = ctx->argv[2];
    Var* v = core.Vars().Find(name);

    if (ctx->argv.size() == 3) {
        if (v) {
            bool hide = (v->flags & RMM_VAR_PROTECTED) && !core.Admin().HasFlags(ctx->caller, RMM_ADMFLAG_ROOT);
            Reply(ctx, Format("%s = \"%s\" (default \"%s\") - %s", v->name.c_str(), hide ? "***" : v->value.c_str(),
                              v->def.c_str(), v->desc.c_str()));
        } else {
            std::string val;

            if (core.Game().GetEngineVar(name, val))
                Reply(ctx, name + " = \"" + val + "\" (engine)");
            else
                Reply(ctx, "Unknown variable " + name + ".");
        }

        return;
    }

    if (!Allowed(ctx, RMM_ADMFLAG_CONFIG))
        return;

    std::string value = Join(ctx->argv, 3);

    if (v) {
        std::string err;
        Reply(ctx,
              core.Vars().Set(nullptr, v, value, &err) ? v->name + " set to \"" + value + "\"." : "Rejected: " + err);
    } else if (StartsWith(ToLower(name), "rmm.") || ctx->source == RMM_SRC_PLUGIN) {
        core.Vars().SetPending(name, value);
        Reply(ctx, name + " stored; it will apply when a plugin registers it.");
    } else if (core.Game().SetEngineVar(name, value)) {
        Reply(ctx, name + " set to \"" + value + "\" (engine).");
    } else {
        core.Vars().SetPending(name, value);
        Reply(ctx, name + " is not known yet; value stored as pending.");
    }
}

std::string Vec(const rmm_vec3& v) { return Format("%.1f,%.1f,%.1f", v.x, v.y, v.z); }

bool ParseVec(const std::vector<std::string>& a, size_t first, rmm_vec3& out) {
    double x, y, z;

    if (a.size() < first + 3 || !ParseDouble(a[first], x) || !ParseDouble(a[first + 1], y) ||
        !ParseDouble(a[first + 2], z))
        return false;

    out = rmm_vec3{static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)};

    return true;
}

// rmm objects [pattern] [roots|empty]
void CmdObjects(const rmm_cmd_ctx* ctx) {
    IGameAdapter& game = Core::Get().Game();

    if (!game.HasCap(RMM_CAP_WORLD_OBJECTS)) {
        Reply(ctx, "World objects are not available on this game/backend (yet).");

        return;
    }

    std::string pattern;
    bool rootsOnly = false, emptyOnly = false;

    for (size_t i = 2; i < ctx->argv.size(); ++i) {
        std::string a = ToLower(ctx->argv[i]);

        if (a == "roots")
            rootsOnly = true;
        else if (a == "empty")
            emptyOnly = true;
        else
            pattern = ctx->argv[i];
    }

    int shown = 0;

    for (auto& v : game.Vehicles()) {
        if ((rootsOnly && !v.isRoot) || !GlobMatch(pattern, v.templateName))
            continue;

        auto occ = game.ObjectOccupants(v.handle);

        if (emptyOnly && !occ.empty())
            continue;

        rmm_object_info info{};
        info.struct_size = sizeof info;
        info.health = info.max_health = -1;
        info.seat = -1;
        game.GetObjectInfo(v.handle, info);
        std::string who;

        for (rmm_player_t p : occ)
            who += (who.empty() ? "" : ",") + std::to_string(p);

        std::string kind = v.isRoot ? "root" : "seat";

        if (info.seat >= 0)
            kind += Format(" %d", info.seat);

        Reply(ctx, Format("0x%llx %-24s %s pos %s vel %s hp %.0f/%.0f%s%s", static_cast<unsigned long long>(v.handle),
                          v.templateName.c_str(), kind.c_str(), Vec(info.position).c_str(), Vec(info.velocity).c_str(),
                          info.health, info.max_health, who.empty() ? "" : " occupants ", who.c_str()));

        if (++shown >= 200) {
            Reply(ctx, "(truncated at 200)");
            break;
        }
    }

    if (!shown)
        Reply(ctx, "No matching vehicles.");
}

// rmm object <handle> [stop | pos x y z | vel x y z [ax ay az] | rot yaw pitch roll | damage <amount>]
void CmdObject(const rmm_cmd_ctx* ctx) {
    IGameAdapter& game = Core::Get().Game();
    const auto& a = ctx->argv;

    if (a.size() < 3) {
        Reply(ctx, "Usage: rmm object <handle> [stop | pos x y z | vel x y z [ax ay az] | rot yaw pitch roll | damage "
                   "<amount>]");

        return;
    }

    rmm_object_t h = static_cast<rmm_object_t>(std::strtoull(a[2].c_str(), nullptr, 0));

    if (!h || !game.ObjectValid(h)) {
        Reply(ctx, "No such object (stale handle?).");

        return;
    }

    std::string sub = a.size() > 3 ? ToLower(a[3]) : "";

    if (sub.empty()) {
        rmm_object_info i{};
        i.struct_size = sizeof i;
        i.health = i.max_health = -1;
        i.seat = -1;
        i.seat_occupant = RMM_PLAYER_NONE;

        if (!game.GetObjectInfo(h, i)) {
            Reply(ctx, "Could not read object.");

            return;
        }

        Reply(ctx, Format("%s (root 0x%llx %s)%s%s%s", i.template_name, static_cast<unsigned long long>(i.root),
                          i.root_template, i.is_vehicle ? " vehicle" : "",
                          i.is_networked ? " networked" : " server-only", i.is_destroyed ? " DESTROYED" : ""));

        if (i.seat >= 0)
            Reply(ctx, i.seat_occupant != RMM_PLAYER_NONE ? Format("  seat %d, taken by #%d", i.seat, i.seat_occupant)
                                                          : Format("  seat %d, free", i.seat));

        Reply(ctx, Format("  pos %s rot %s vel %s ang %s hp %.0f/%.0f occupants %d", Vec(i.position).c_str(),
                          Vec(i.rotation).c_str(), Vec(i.velocity).c_str(), Vec(i.angular_velocity).c_str(), i.health,
                          i.max_health, i.occupant_count));

        return;
    }

    if (!Allowed(ctx, RMM_ADMFLAG_CHEATS))
        return;

    rmm_vec3 v{}, w{};
    bool ok = false;

    if (sub == "stop")
        ok = game.SetObjectVelocity(h, rmm_vec3{0, 0, 0}, rmm_vec3{0, 0, 0});
    else if (sub == "pos" && ParseVec(a, 4, v))
        ok = game.SetObjectPosition(h, v);
    else if (sub == "rot" && ParseVec(a, 4, v))
        ok = game.SetObjectRotation(h, v);
    else if (sub == "vel" && ParseVec(a, 4, v)) {
        ParseVec(a, 7, w);
        ok = game.SetObjectVelocity(h, v, w);
    } else if (sub == "damage" && a.size() > 4) {  // through the API, so the checks match plugins'
        double amount = 0;

        if (!ParseDouble(a[4], amount)) {
            Reply(ctx, "Usage: rmm object <handle> damage <amount>");

            return;
        }

        if (!game.HasCap(RMM_CAP_OBJECT_DAMAGE)) {
            Reply(ctx, "Damaging objects is not available on this game/backend.");

            return;
        }

        ok = Core::Get().Api()->world->damage_object(h, static_cast<float>(amount), ctx->caller) != 0;
    } else {
        Reply(ctx, "Unknown or incomplete subcommand.");

        return;
    }

    Reply(ctx, ok ? "Done." : "Not supported on this game, or the call failed.");
}

// A position on the command line: "x y z", or "@<player>" (#id, name, @me) for
// where that player is. @p used = number of arguments consumed.
bool ParsePlace(const rmm_cmd_ctx* ctx, size_t first, rmm_vec3& out, size_t& used) {
    const auto& a = ctx->argv;

    if (a.size() > first && !a[first].empty() && a[first][0] == '@') {
        std::string who = a[first].substr(1);
        rmm_player_t id = RMM_PLAYER_NONE;

        if (ToLower(who) == "me") {
            id = ctx->caller;
        } else if (!who.empty() && who[0] == '#') {
            id = static_cast<rmm_player_t>(std::atoi(who.c_str() + 1));
        } else if (!who.empty()) {
            for (rmm_player_t p : Core::Get().Game().PlayerIds()) {  // first partial name match
                rmm_player_info pi{};
                pi.struct_size = sizeof pi;

                if (Core::Get().Game().GetPlayer(p, pi) && IContains(pi.name, who)) {
                    id = p;
                    break;
                }
            }
        }

        rmm_player_info info{};
        info.struct_size = sizeof info;

        if (id == RMM_PLAYER_NONE || !Core::Get().Game().GetPlayer(id, info))
            return false;

        out = info.position;
        used = 1;

        return true;
    }

    used = 3;

    return ParseVec(a, first, out);
}

// rmm find [pattern] [radius [x y z | @player]] [novehicles] [nosoldiers] [summary]
void CmdFind(const rmm_cmd_ctx* ctx) {
    IGameAdapter& game = Core::Get().Game();

    if (!game.HasCap(RMM_CAP_OBJECT_QUERY)) {
        Reply(ctx, "Object queries are not available on this game/backend (yet).");

        return;
    }

    const auto& a = ctx->argv;
    std::string pattern;
    uint32_t flags = 0;
    double radius = -1;
    rmm_vec3 center{};
    bool haveCenter = false, summary = false;

    for (size_t i = 2; i < a.size(); ++i) {
        std::string l = ToLower(a[i]);
        double d;
        size_t used = 0;

        if (l == "novehicles")
            flags |= RMM_FIND_NO_VEHICLES;
        else if (l == "nosoldiers")
            flags |= RMM_FIND_NO_SOLDIERS;
        else if (l == "summary")
            summary = true;
        else if (radius < 0 && ParseDouble(a[i], d) && !pattern.empty()) {
            radius = d;

            if (ParsePlace(ctx, i + 1, center, used)) {
                haveCenter = true;
                i += used;
            } else if (ctx->caller != RMM_PLAYER_NONE) {
                rmm_player_info me{};
                me.struct_size = sizeof me;

                if (game.GetPlayer(ctx->caller, me)) {
                    center = me.position;
                    haveCenter = true;
                }
            }

            if (!haveCenter) {
                Reply(ctx, "A radius needs a center: x y z or @player.");

                return;
            }
        } else if (pattern.empty())
            pattern = a[i];
        else {
            Reply(ctx, "Usage: rmm find [pattern] [radius [x y z | @player]] [novehicles] [nosoldiers] [summary]");

            return;
        }
    }

    if (pattern == "*")
        pattern.clear();

    auto hits =
        FilterObjects(game.WorldObjects(), pattern, haveCenter ? &center : nullptr, static_cast<float>(radius), flags);

    if (summary) {  // one line per template, most common first
        std::map<std::string, int> count;

        for (auto& o : hits)
            ++count[o.templateName];

        std::vector<std::pair<std::string, int>> rows(count.begin(), count.end());
        std::stable_sort(rows.begin(), rows.end(), [](auto& x, auto& y) { return x.second > y.second; });
        std::string line;

        for (auto& r : rows) {  // several per line (80-column consoles): big maps have hundreds of templates
            std::string item = Format("%s x%d", r.first.c_str(), r.second);

            if (!line.empty() && line.size() + item.size() > 74) {
                Reply(ctx, line);
                line.clear();
            }

            line += (line.empty() ? "" : ", ") + item;
        }

        if (!line.empty())
            Reply(ctx, line);

        Reply(ctx, Format("%zu object(s), %zu template(s).", hits.size(), rows.size()));

        return;
    }

    int shown = 0;

    for (auto& o : hits) {
        float dx = o.position.x - center.x, dz = o.position.z - center.z;
        Reply(ctx, Format("0x%llx %-28s pos %s%s%s", static_cast<unsigned long long>(o.handle), o.templateName.c_str(),
                          Vec(o.position).c_str(),
                          o.isVehicle   ? " vehicle"
                          : o.isSoldier ? " soldier"
                                        : "",
                          haveCenter ? Format(" (%.0f m)", std::sqrt(dx * dx + dz * dz)).c_str() : ""));

        if (++shown >= 200)
            break;
    }

    Reply(ctx, Format("%zu object(s)%s.", hits.size(), hits.size() > 200 ? ", first 200 shown" : ""));
}

// rmm spawn <template> [x y z | @player] [yaw] [ground]
void CmdSpawn(const rmm_cmd_ctx* ctx) {
    IGameAdapter& game = Core::Get().Game();
    const auto& a = ctx->argv;

    if (a.size() < 3) {
        Reply(ctx, "Usage: rmm spawn <template> [x y z | @player] [yaw] [ground]");

        return;
    }

    if (!Allowed(ctx, RMM_ADMFLAG_CHEATS))
        return;

    if (!game.HasCap(RMM_CAP_OBJECT_SPAWN)) {
        Reply(ctx, "Spawning objects is not available on this game/backend (yet).");

        return;
    }

    if (!game.TemplateExists(a[2])) {
        Reply(ctx, "Unknown template '" + a[2] + "'.");

        return;
    }

    rmm_vec3 pos{}, rot{};
    size_t used = 0, next = 3;
    bool nearPlayer = false;

    if (a.size() > 3 && ParsePlace(ctx, 3, pos, used)) {
        next = 3 + used;
        nearPlayer = a[3][0] == '@';
    } else if (a.size() > 3 && a[3][0] == '@') {
        Reply(ctx, "No such player: " + a[3].substr(1));

        return;
    } else {  // no position: next to whoever typed it
        rmm_player_info me{};
        me.struct_size = sizeof me;

        if (ctx->caller == RMM_PLAYER_NONE || !game.GetPlayer(ctx->caller, me)) {
            Reply(ctx, "Give a position: x y z or @player.");

            return;
        }

        pos = me.position;
        nearPlayer = true;
    }

    bool ground = false;

    for (size_t i = next; i < a.size(); ++i) {
        double d;

        if (ToLower(a[i]) == "ground")
            ground = true;
        else if (ParseDouble(a[i], d))
            rot.x = static_cast<float>(d);  // yaw
    }

    float h = 0;

    if (nearPlayer) {
        // 5 m beside the player (vehicles refuse to spawn within 2 m of a soldier), on the ground
        pos.x += 5.0f;

        if (game.GroundHeight(pos.x, pos.z, h))
            pos.y = h + 0.5f;
    } else if (ground) {
        if (!game.GroundHeight(pos.x, pos.z, h)) {
            Reply(ctx, "Terrain height is not available.");

            return;
        }

        pos.y += h;  // y was a height above the ground
    }

    rmm_object_t obj = game.SpawnObject(a[2], pos, rot);

    if (!obj) {
        Reply(ctx, "Spawn failed (blocked by a nearby soldier, or the engine refused).");

        return;
    }

    rmm_object_info i{};
    i.struct_size = sizeof i;
    i.health = i.max_health = -1;
    game.GetObjectInfo(obj, i);
    Reply(ctx, Format("Spawned %s 0x%llx at %s (%s).", a[2].c_str(), static_cast<unsigned long long>(obj),
                      Vec(pos).c_str(), i.is_networked ? "networked" : "server-only: players won't see it"));
}

// rmm remove <handle>
void CmdRemove(const rmm_cmd_ctx* ctx) {
    IGameAdapter& game = Core::Get().Game();

    if (ctx->argv.size() < 3) {
        Reply(ctx, "Usage: rmm remove <handle>");

        return;
    }

    if (!Allowed(ctx, RMM_ADMFLAG_CHEATS))
        return;

    rmm_object_t h = static_cast<rmm_object_t>(std::strtoull(ctx->argv[2].c_str(), nullptr, 0));

    if (!h || !game.ObjectValid(h)) {
        Reply(ctx, "No such object (stale handle?).");

        return;
    }

    Reply(ctx, game.RemoveObject(h) ? "Removed." : "Not supported on this game, or the engine refused.");
}

// rmm ground <x z | @player>
void CmdGround(const rmm_cmd_ctx* ctx) {
    IGameAdapter& game = Core::Get().Game();
    const auto& a = ctx->argv;
    rmm_vec3 p{};
    size_t used = 0;
    double x, z;

    if (a.size() > 2 && a[2][0] == '@') {
        if (!ParsePlace(ctx, 2, p, used)) {
            Reply(ctx, "No such player.");

            return;
        }
    } else if (a.size() > 3 && ParseDouble(a[2], x) && ParseDouble(a[3], z))
        p = rmm_vec3{float(x), 0, float(z)};
    else {
        Reply(ctx, "Usage: rmm ground <x z | @player>");

        return;
    }

    float h;

    if (!game.GroundHeight(p.x, p.z, h)) {
        Reply(ctx, "Terrain height is not available (no map, or unsupported).");

        return;
    }

    Reply(ctx, Format("Ground at %.1f,%.1f is %.2f%s", p.x, p.z, h,
                      used ? Format(" (player at %.2f, %.2f above)", p.y, p.y - h).c_str() : ""));
}

// rmm bots [list] | info|behaviours <target> | add <team> [kit] [name] [at x y z|@player]
//          remove <target> | removeall | move <target> <x y z|@player> [radius]
//          patrol <target> <x y z|@player> [<x y z|@player>] [radius] | order <target> <behaviour> [modifiers]
//          strategy <team> [name] | skill [0..1]
//          control <target> [engine|override|exclusive] | input <target> [<slot>=<value> ... [hold <ms>]]
// Goes through the plugin API table, so it exercises exactly what plugins get.
void CmdBots(const rmm_cmd_ctx* ctx) {
    Core& core = Core::Get();
    const rmm_api* api = core.Api();
    const rmm_bots_api* bots = api->bots;
    const auto& a = ctx->argv;
    std::string sub = a.size() > 2 ? ToLower(a[2]) : "list";
    auto usage = [&] {
        Reply(ctx,
              "rmm bots [list] | info <target> | behaviours <target> | add <team> [kit] [name] [at x y z|@player]");
        Reply(ctx, "  remove <target> | removeall | move <target> <x y z|@player> [radius]");
        Reply(ctx,
              "  patrol <target> <x y z|@player> [<x y z|@player>] [radius] | order <target> <behaviour> [modifiers]");
        Reply(ctx, "  strategy <team> [name] | skill [0..1]");
        Reply(ctx, "  control <target> [engine|override|exclusive] | input <target> [<slot>=<value> ... [hold <ms>]]");
    };

    if (sub == "help") {
        usage();

        return;
    }

    if (!core.Game().HasCap(RMM_CAP_BOT_QUERY)) {
        Reply(ctx, "Bot control is not available on this game/backend.");

        return;
    }

    if (!bots->available()) {
        Reply(ctx, "The AI is not running (Battlefield 1942/Vietnam: co-op rounds only).");

        return;
    }

    // Bot targets: #id, name, @bots/@t1... (players that aren't bots are skipped).
    auto targets = [&](size_t i, std::vector<rmm_player_t>& out) {
        out.clear();

        if (a.size() <= i) {
            Reply(ctx, "Missing target.");

            return false;
        }

        rmm_player_t buf[256];
        char err[128] = "";
        int32_t n = api->players->target(a[i].c_str(), ctx->caller, RMM_TARGET_ALLOW_MULTI | RMM_TARGET_NO_IMMUNITY,
                                         buf, 256, err, sizeof err);

        for (int32_t k = 0; k < n && k < 256; ++k)
            if (bots->is_bot(buf[k]))
                out.push_back(buf[k]);

        if (out.empty())
            Reply(ctx, n ? "No bots among the targets." : std::string(err));

        return !out.empty();
    };

    auto botLine = [&](rmm_player_t p) {
        rmm_bot_info b{};
        b.struct_size = sizeof b;

        if (!bots->get_info(p, &b))
            return Format("#%d (gone)", p);

        return Format("#%-3d bot %-3d %-20s team %d %-10s %-8s pos %s goal %s%s%s", b.player, b.bot_id, b.name, b.team,
                      b.kit, b.behaviour, Vec(b.position).c_str(), Vec(b.goal).c_str(), b.vehicle[0] ? " in " : "",
                      b.vehicle);
    };

    std::vector<rmm_player_t> who;

    if (sub == "list") {
        rmm_player_t buf[256];
        int32_t n = bots->list(buf, 256);

        // get_info is several console round trips per bot: keep the plain list cheap.
        for (int32_t i = 0; i < n && i < 256; ++i) {
            rmm_player_info pi{};
            pi.struct_size = sizeof pi;
            api->players->get_info(buf[i], &pi);
            Reply(ctx, Format("#%-3d %-24s team %d %s", buf[i], pi.name, pi.team, pi.is_alive ? "alive" : "dead"));
        }

        char strat[64] = "?", strat2[64] = "?";
        bots->get_strategy(1, strat, sizeof strat);
        bots->get_strategy(2, strat2, sizeof strat2);
        float skill = -1;
        bots->get_skill(&skill);
        Reply(ctx, Format("%d bot(s). Strategy: team 1 %s, team 2 %s. Skill %.2f.", n, strat, strat2, skill));
    } else if (sub == "info") {
        if (!targets(3, who))
            return;

        for (rmm_player_t p : who)
            Reply(ctx, botLine(p));
    } else if (sub == "behaviours") {
        if (!targets(3, who))
            return;

        char buf[512];
        bots->behaviours(who[0], buf, sizeof buf);
        std::string s = buf;
        std::replace(s.begin(), s.end(), '\n', ' ');
        Reply(ctx, "Behaviours: " + s);
    } else if (sub == "add") {
        if (!Allowed(ctx, RMM_ADMFLAG_CONFIG))
            return;

        if (a.size() < 4) {
            usage();

            return;
        }

        if (!core.Game().HasCap(RMM_CAP_BOT_ADD)) {
            Reply(ctx, "Adding bots is off on this server (gamedata [capabilities] bot_add).");

            return;
        }

        rmm_bot_spawn s = RMM_BOT_SPAWN_INIT;
        s.team = std::atoi(a[3].c_str());
        std::string kit, name;
        size_t i = 4;

        for (; i < a.size() && ToLower(a[i]) != "at"; ++i)
            (kit.empty() ? kit : name) = a[i];

        if (i < a.size()) {
            size_t used = 0;

            if (!ParsePlace(ctx, i + 1, s.position, used)) {
                Reply(ctx, "Bad position after 'at'.");

                return;
            }

            s.flags |= RMM_BOT_SPAWN_AT;
        }

        s.kit = kit.empty() ? nullptr : kit.c_str();
        s.name = name.empty() ? nullptr : name.c_str();
        s.tag = 1;
        Reply(ctx, bots->add(&s)
                       ? "Adding bot (watch for BOT_ADDED / BOT_ADD_FAILED)."
                       : "Refused (team must be 1 or 2; server full; or a request is stuck). See the RMM log.");
    } else if ((sub == "remove" || sub == "removeall") && !core.Game().HasCap(RMM_CAP_BOT_REMOVE)) {
        Reply(ctx, "Removing bots is not supported on this game (yet). 'kill' works on bots like on any player.");
    } else if (sub == "remove") {
        if (!Allowed(ctx, RMM_ADMFLAG_CONFIG) || !targets(3, who))
            return;

        int ok = 0;

        for (rmm_player_t p : who)
            ok += bots->remove(p) ? 1 : 0;

        Reply(ctx, Format("Removed %d of %zu bot(s).", ok, who.size()));
    } else if (sub == "removeall") {
        if (!Allowed(ctx, RMM_ADMFLAG_CONFIG))
            return;

        Reply(ctx, Format("Removed %d bot(s).", bots->remove_all()));
    } else if (sub == "move" || sub == "patrol") {
        if (!Allowed(ctx, RMM_ADMFLAG_CHEATS) || !targets(3, who))
            return;

        std::vector<rmm_vec3> pts;
        size_t i = 4, used = 0;
        rmm_vec3 v;

        while (pts.size() < (sub == "move" ? 1u : 2u) && ParsePlace(ctx, i, v, used)) {
            pts.push_back(v);
            i += used;
        }

        if (pts.empty()) {
            Reply(ctx, "Give a position: x y z or @player.");

            return;
        }

        double radius = 0;

        if (i < a.size())
            ParseDouble(a[i], radius);

        int ok = 0;

        for (rmm_player_t p : who)
            ok += (sub == "move" ? bots->move_to(p, pts[0], float(radius))
                                 : bots->patrol(p, pts.data(), int32_t(pts.size()), float(radius)))
                      ? 1
                      : 0;

        Reply(ctx, Format("Ordered %d of %zu bot(s) to %s %s (the strategic AI takes over again after a while).", ok,
                          who.size(), sub == "move" ? "move to" : "patrol from", Vec(pts[0]).c_str()));
    } else if (sub == "order") {
        if (!Allowed(ctx, RMM_ADMFLAG_CHEATS) || !targets(3, who))
            return;

        if (a.size() < 5) {
            Reply(ctx, "Usage: rmm bots order <target> <behaviour> [modifiers]");

            return;
        }

        const char* mods = a.size() > 5 ? a[5].c_str() : nullptr;
        int ok = 0;

        for (rmm_player_t p : who)
            ok += bots->order(p, a[4].c_str(), mods) ? 1 : 0;

        Reply(ctx, Format("Sent '%s' to %d of %zu bot(s).", a[4].c_str(), ok, who.size()));
    } else if (sub == "strategy") {
        if (a.size() < 4) {
            Reply(ctx, "Usage: rmm bots strategy <team> [name]");

            return;
        }

        int32_t team = std::atoi(a[3].c_str());

        if (a.size() > 4) {
            if (!Allowed(ctx, RMM_ADMFLAG_CONFIG))
                return;

            if (!bots->set_strategy(team, a[4].c_str())) {
                Reply(ctx, "Unknown strategy for that team (or unsupported).");

                return;
            }
        }

        char cur[64] = "?", all[1024] = "";
        bots->get_strategy(team, cur, sizeof cur);
        bots->strategies(team, all, sizeof all);
        std::string list = all;
        std::replace(list.begin(), list.end(), '\n', ' ');
        Reply(ctx, Format("Team %d strategy: %s (available: %s)", team, cur, list.c_str()));
    } else if (sub == "skill") {
        double v;

        if (a.size() > 3) {
            if (!Allowed(ctx, RMM_ADMFLAG_CONFIG))
                return;

            if (!ParseDouble(a[3], v) || v < 0 || v > 1) {
                Reply(ctx, "Skill is 0.0 - 1.0.");

                return;
            }

            if (!bots->set_skill(float(v))) {
                Reply(ctx, "Could not set the skill.");

                return;
            }
        }

        float s = -1;
        Reply(ctx, bots->get_skill(&s) ? Format("Bot skill %.2f.", s) : std::string("Bot skill unknown."));
    } else if (sub == "control" || sub == "input") {
        // The console acts as owner nullptr: it may take over from any plugin.
        if (!core.Game().HasCap(RMM_CAP_BOT_INPUT)) {
            Reply(ctx, "Bot input control is not available on this game/backend.");

            return;
        }

        if (!targets(3, who))
            return;

        BotControl& bc = core.BotInput();
        auto name = [&](rmm_player_t p) {
            rmm_player_info pi{};
            pi.struct_size = sizeof pi;
            api->players->get_info(p, &pi);

            return Format("#%d %s", p, pi.name);
        };

        if (sub == "control") {
            static const char* kModes[] = {"engine", "override", "exclusive"};

            if (a.size() > 4) {
                if (!Allowed(ctx, RMM_ADMFLAG_CHEATS))
                    return;

                int mode = -1;

                for (int m = 0; m < 3; ++m)
                    if (ToLower(a[4]) == kModes[m])
                        mode = m;

                if (mode < 0) {
                    Reply(ctx, "Mode is engine, override or exclusive.");

                    return;
                }

                for (rmm_player_t p : who) {
                    std::string err;

                    if (!bc.SetMode(nullptr, p, mode, &err))
                        Reply(ctx, name(p) + ": " + err);
                }
            }

            for (rmm_player_t p : who) {
                Plugin* owner = nullptr;
                int32_t m = bc.Mode(p, &owner);
                Reply(ctx, Format("%s: %s%s%s", name(p).c_str(), kModes[std::clamp(m, 0, 2)],
                                  m == RMM_BOT_CONTROL_ENGINE ? ""
                                  : owner                     ? " by "
                                                              : " by the console",
                                  m != RMM_BOT_CONTROL_ENGINE && owner ? owner->name.c_str() : ""));
            }

            return;
        }

        if (a.size() == 4) {  // show what the engine's AI sends
            for (rmm_player_t p : who) {
                rmm_bot_input in{};
                in.struct_size = sizeof in;

                if (!bc.EngineInput(p, in, core.Time())) {
                    Reply(ctx, name(p) + ": no input seen yet");
                    continue;
                }

                std::string s;

                for (int i = 0; i < RMM_BOT_INPUT_SLOTS; ++i)
                    if (((in.mask >> i) & 1u) && in.value[i] != 0.0f)
                        s += Format(" %s=%.2f",
                                    BotInputSlotName(i).empty() ? std::to_string(i).c_str()
                                                                : BotInputSlotName(i).c_str(),
                                    in.value[i]);

                Reply(ctx, Format("%s: engine input tick %u, %u ms ago:%s", name(p).c_str(), in.tick, in.hold_ms,
                                  s.empty() ? " (all idle)" : s.c_str()));
            }

            return;
        }

        if (!Allowed(ctx, RMM_ADMFLAG_CHEATS))
            return;

        rmm_bot_input in{};
        in.struct_size = sizeof in;

        for (size_t i = 4; i < a.size(); ++i) {
            if (ToLower(a[i]) == "hold" && i + 1 < a.size()) {
                in.hold_ms = uint32_t(std::max(0, std::atoi(a[++i].c_str())));
                continue;
            }

            size_t eq = a[i].find('=');
            int slot = BotInputSlotFromName(a[i].substr(0, eq));
            double v = 1.0;

            if (slot < 0 || (eq != std::string::npos && !ParseDouble(a[i].substr(eq + 1), v))) {
                Reply(ctx, "Unknown input '" + a[i] +
                               "'. Use <slot>=<value> (throttle=1, fire=1, mouse_look_x=-0.5, 8=1 ...) [hold <ms>].");

                return;
            }

            in.mask |= RMM_BI_BIT(slot);
            in.value[slot] = float(v);
        }

        int ok = 0;

        for (rmm_player_t p : who) {
            std::string err;

            if (bc.SetInput(nullptr, p, in, core.Time(), &err))
                ++ok;
            else
                Reply(ctx, name(p) + ": " + err);
        }

        Reply(ctx, Format("Input latched for %d of %zu bot(s)%s.", ok, who.size(),
                          in.hold_ms ? Format(" for %u ms", in.hold_ms).c_str() : ""));
    } else {
        usage();
    }
}

// rmm kit <target> <kit template> | rmm score <target> <score> | rmm endround [1|2|draw]
// Goes through the plugin API table, like `rmm bots`.
void CmdGameMode(const rmm_cmd_ctx* ctx, const std::string& sub) {
    Core& core = Core::Get();
    const rmm_api* api = core.Api();
    const auto& a = ctx->argv;

    if (sub == "endround") {
        if (!Allowed(ctx, RMM_ADMFLAG_CHANGEMAP))
            return;

        if (!core.Game().HasCap(RMM_CAP_END_ROUND)) {
            Reply(ctx, "Ending the round is not available on this game/backend.");

            return;
        }

        std::string w = a.size() > 2 ? ToLower(a[2]) : "draw";
        int32_t winner = w == "draw" || w == "0" ? RMM_TEAM_NONE : std::atoi(w.c_str());

        if (winner < 0 || winner > 2) {
            Reply(ctx, "Usage: rmm endround [1|2|draw]");

            return;
        }

        Reply(ctx, api->game->end_round(winner) ? "Round ended." : "The game refused (no round being played?).");

        return;
    }

    bool kit = sub == "kit";

    if (a.size() < 4) {
        Reply(ctx, kit ? "Usage: rmm kit <target> <kit template>" : "Usage: rmm score <target> <score>");

        return;
    }

    if (!Allowed(ctx, RMM_ADMFLAG_CHEATS))
        return;

    if (!core.Game().HasCap(kit ? RMM_CAP_SET_KIT : RMM_CAP_SET_SCORE)) {
        Reply(ctx, kit ? "Giving kits is not available on this game/backend."
                       : "Setting scores is not available on this game/backend.");

        return;
    }

    rmm_player_t buf[256];
    char err[128] = "";
    int32_t n = api->players->target(a[2].c_str(), ctx->caller, RMM_TARGET_ALLOW_MULTI, buf, 256, err, sizeof err);

    if (n <= 0) {
        Reply(ctx, err);

        return;
    }

    double score = 0;

    if (!kit && !ParseDouble(a[3], score)) {
        Reply(ctx, "Score must be a number.");

        return;
    }

    if (kit && !core.Game().TemplateExists(a[3]) && core.Game().HasCap(RMM_CAP_OBJECT_QUERY)) {
        Reply(ctx, "Unknown template '" + a[3] + "'.");

        return;
    }

    int ok = 0;

    for (int32_t i = 0; i < n && i < 256; ++i)
        ok += (kit ? api->players->set_kit(buf[i], a[3].c_str()) : api->players->set_score(buf[i], int32_t(score))) ? 1
                                                                                                                    : 0;

    if (kit)
        Reply(ctx, Format("Gave %s to %d of %d player(s)%s.", a[3].c_str(), ok, n,
                          ok < n ? " (dead, in a vehicle, or not a kit)" : ""));
    else
        Reply(ctx, Format("Score set for %d of %d player(s).", ok, n));
}

// rmm putin <target> <vehicle name|nearest|handle> [seat] | rmm eject <target>
void CmdPutIn(const rmm_cmd_ctx* ctx, const std::string& sub) {
    Core& core = Core::Get();
    const rmm_api* api = core.Api();
    const auto& a = ctx->argv;
    bool in = sub == "putin";

    if (a.size() < (in ? 4u : 3u)) {
        Reply(ctx, in ? "Usage: rmm putin <target> <vehicle name|nearest|handle> [seat]" : "Usage: rmm eject <target>");

        return;
    }

    if (!Allowed(ctx, RMM_ADMFLAG_SLAY))
        return;

    if (!core.Game().HasCap(RMM_CAP_PUT_IN_VEHICLE)) {
        Reply(ctx, "Moving players in and out of vehicles is not available on this game/backend.");

        return;
    }

    rmm_player_t buf[64];
    char err[128] = "";
    int32_t n =
        api->players->target(a[2].c_str(), ctx->caller, in ? 0 : RMM_TARGET_ALLOW_MULTI, buf, 64, err, sizeof err);

    if (n <= 0) {
        Reply(ctx, err);

        return;
    }

    if (!in) {
        int ok = 0;

        for (int32_t i = 0; i < n && i < 64; ++i)
            ok += api->players->exit_vehicle(buf[i]) ? 1 : 0;

        Reply(ctx, Format("Took %d of %d player(s) out of their vehicle.", ok, n));

        return;
    }

    // A handle (0x...), or a vehicle name / glob ("T34", "*", "nearest"): the nearest such vehicle.
    rmm_object_t v = 0;
    std::string what = a[3];

    if (!what.empty() && std::isdigit(static_cast<unsigned char>(what[0]))) {
        v = static_cast<rmm_object_t>(std::strtoull(what.c_str(), nullptr, 0));

        if (!v || !core.Game().ObjectValid(v)) {
            Reply(ctx, "No such object (stale handle?). See rmm objects.");

            return;
        }
    } else {
        rmm_player_info me{};
        me.struct_size = sizeof me;
        core.Game().GetPlayer(buf[0], me);
        std::string pattern = ToLower(what) == "nearest" ? "" : what;
        float best = -1;

        for (auto& r : core.Game().Vehicles()) {
            if (!r.isRoot || !GlobMatch(pattern, r.templateName))
                continue;

            rmm_object_info oi{};
            oi.struct_size = sizeof oi;

            if (!core.Game().GetObjectInfo(r.handle, oi) || oi.is_destroyed)
                continue;

            float d = DistSq(oi.position, me.position);

            if (best < 0 || d < best) {
                best = d;
                v = r.handle;
            }
        }

        if (!v) {
            Reply(ctx, "No vehicle named '" + what + "' on this map (rmm objects lists them).");

            return;
        }
    }

    int32_t seat = a.size() > 4 ? std::atoi(a[4].c_str()) : -1;
    std::string name;
    core.Game().ObjectTemplate(v, name);
    Reply(ctx, api->players->enter_vehicle(buf[0], v, seat)
                   ? Format("Put in %s 0x%llx.", name.c_str(), static_cast<unsigned long long>(v))
                   : "Refused (dead, seat taken or no such seat, or not a vehicle).");
}

// rmm items <target> [select <slot> | remove <slot> | lock <slot,slot...> | unlock]
// "lock" is a diagnostic tool that needs no plugin: it blocks the
// target's own switches (reasons PLAYER and NETWORK) to any slot not listed,
// until "unlock" or they leave.
namespace {
std::map<rmm_player_t, std::set<int32_t>> g_itemLocks;
rmm_handle_t g_itemLockHook = RMM_INVALID_HANDLE;

int32_t RMM_CALL ItemLockHook(int32_t ev, int32_t, void* data, void*) {
    if (ev == RMM_EV_PLAYER_DISCONNECT) {
        g_itemLocks.erase(static_cast<rmm_ev_player*>(data)->player);

        return RMM_CONTINUE;
    }

    auto* e = static_cast<rmm_ev_weapon_switch*>(data);
    auto it = g_itemLocks.find(e->player);

    if (it == g_itemLocks.end() || it->second.count(e->to_slot))
        return RMM_CONTINUE;

    bool blockable = e->reason == RMM_WEAPON_REASON_PLAYER || e->reason == RMM_WEAPON_REASON_NETWORK;
    Core::Get().Log().Info("items lock: #%d %s %d:%s -> %d:%s", e->player, blockable ? "blocked" : "can't block",
                           e->from_slot, e->from_weapon, e->to_slot, e->to_weapon);

    return blockable ? RMM_HANDLED : RMM_CONTINUE;
}
}  // namespace

void CmdItems(const rmm_cmd_ctx* ctx) {
    Core& core = Core::Get();
    const rmm_api* api = core.Api();
    const auto& a = ctx->argv;

    if (a.size() < 3) {
        Reply(ctx, "Usage: rmm items <target> [select <slot> | remove <slot> | lock <slot,...> | unlock]");

        return;
    }

    if (!core.Game().HasCap(RMM_CAP_ITEMS)) {
        Reply(ctx, "Item control is not available on this game/backend.");

        return;
    }

    std::string sub = a.size() > 3 ? ToLower(a[3]) : "";

    if (!sub.empty() && !Allowed(ctx, RMM_ADMFLAG_CHEATS))
        return;

    rmm_player_t buf[64];
    char err[128] = "";
    int32_t n = api->players->target(a[2].c_str(), ctx->caller, RMM_TARGET_ALLOW_MULTI, buf, 64, err, sizeof err);

    if (n <= 0) {
        Reply(ctx, err);

        return;
    }

    if (sub == "unlock") {
        for (int32_t i = 0; i < n && i < 64; ++i)
            g_itemLocks.erase(buf[i]);

        Reply(ctx, Format("Unlocked %d player(s).", n));

        return;
    }

    if (sub == "lock") {
        std::set<int32_t> slots;

        for (size_t i = 4; i < a.size(); ++i) {
            std::stringstream ss(a[i]);
            std::string s;

            while (std::getline(ss, s, ','))
                if (!Trim(s).empty())
                    slots.insert(std::atoi(Trim(s).c_str()));
        }

        if (slots.empty()) {
            Reply(ctx, "Usage: rmm items <target> lock <slot,slot...> (the slots they may hold)");

            return;
        }

        if (g_itemLockHook == RMM_INVALID_HANDLE) {
            g_itemLockHook =
                core.Events().Hook(nullptr, RMM_EV_PLAYER_WEAPON_SWITCH, RMM_PHASE_PRE, 0, ItemLockHook, nullptr);
            core.Events().Hook(nullptr, RMM_EV_PLAYER_DISCONNECT, RMM_PHASE_POST, 0, ItemLockHook, nullptr);
        }

        for (int32_t i = 0; i < n && i < 64; ++i) {
            g_itemLocks[buf[i]] = slots;
            // Holding a locked item now: switch to the first allowed one.
            rmm_item_info items[32];
            int32_t c = api->players->get_items(buf[i], items, 32);
            bool holdsAllowed = false;

            for (int32_t k = 0; k < c && k < 32; ++k)
                holdsAllowed |= items[k].active && slots.count(items[k].slot) > 0;

            for (int32_t k = 0; k < c && k < 32 && !holdsAllowed; ++k)
                if (slots.count(items[k].slot))
                    holdsAllowed = api->players->select_item(buf[i], items[k].slot) != 0;
        }

        Reply(ctx, Format("Locked %d player(s) to slot(s) %s. Watch the log for blocked switches.", n, a[4].c_str()));

        return;
    }

    if (sub == "select" || sub == "remove") {
        if (a.size() < 5) {
            Reply(ctx, "Usage: rmm items <target> " + sub + " <slot>");

            return;
        }

        int32_t slot = std::atoi(a[4].c_str());
        int ok = 0;

        for (int32_t i = 0; i < n && i < 64; ++i)
            ok += (sub == "select" ? api->players->select_item(buf[i], slot) : api->players->remove_item(buf[i], slot))
                      ? 1
                      : 0;

        Reply(ctx, Format("%s slot %d for %d of %d player(s)%s.", sub == "select" ? "Selected" : "Removed", slot, ok, n,
                          ok < n ? " (dead, in a vehicle, or nothing in that slot)" : ""));

        return;
    }

    if (!sub.empty()) {
        Reply(ctx, "Usage: rmm items <target> [select <slot> | remove <slot> | lock <slot,...> | unlock]");

        return;
    }

    for (int32_t i = 0; i < n && i < 64; ++i) {
        rmm_item_info items[32];
        int32_t c = api->players->get_items(buf[i], items, 32);
        std::string line = core.PlayerLabel(buf[i]) + ":";

        if (c < 0)
            line += " (dead or in a vehicle)";

        for (int32_t k = 0; k < c && k < 32; ++k)
            line += Format(" %s%d=%s%s", items[k].active ? "[" : "", items[k].slot, items[k].weapon,
                           items[k].active ? "]" : "");

        auto lk = g_itemLocks.find(buf[i]);

        if (lk != g_itemLocks.end()) {
            line += "  locked to";

            for (int32_t s : lk->second)
                line += Format(" %d", s);
        }

        Reply(ctx, line);
    }
}

// rmm cps [list] | team <cp> <0|1|2> | lock <cp> | unlock <cp>     (<cp> = name or handle)
void CmdControlPoints(const rmm_cmd_ctx* ctx) {
    IGameAdapter& game = Core::Get().Game();
    const auto& a = ctx->argv;
    std::string sub = a.size() > 2 ? ToLower(a[2]) : "list";

    if (!game.HasCap(RMM_CAP_CONTROL_POINTS)) {
        Reply(ctx, "Control points are not available on this game.");

        return;
    }

    auto info = [&](rmm_object_t cp, rmm_control_point_info& i) {
        i = rmm_control_point_info{};
        i.struct_size = sizeof i;
        i.object = cp;

        return game.GetControlPoint(cp, i);
    };

    if (sub == "list") {
        static const char* kStates[] = {"?", "held", "losing", "taking"};
        int n = 0;

        for (rmm_object_t cp : game.ControlPoints()) {
            rmm_control_point_info i;

            if (!info(cp, i))
                continue;

            ++n;
            std::string extra;

            if (i.state == RMM_CP_STATE_TAKING)
                extra += Format(" by team %d", i.capturing_team);

            if (i.locked)
                extra += " LOCKED";

            if (i.fixed)
                extra += " fixed";

            if (i.only_team)
                extra += Format(" only-team %d", i.only_team);

            Reply(ctx, Format("0x%llx %-24s team %d %s%s  at %s r %.0f", static_cast<unsigned long long>(cp), i.name,
                              i.team, kStates[i.state >= 0 && i.state <= 3 ? i.state : 0], extra.c_str(),
                              Vec(i.position).c_str(), i.radius));
        }

        Reply(ctx, Format("%d control point(s).", n));

        return;
    }

    if (sub != "team" && sub != "lock" && sub != "unlock") {
        Reply(ctx, "Usage: rmm cps [list] | team <cp> <0|1|2> | lock <cp> | unlock <cp>");

        return;
    }

    if (!Allowed(ctx, RMM_ADMFLAG_CHEATS))
        return;

    if (a.size() < 4 || (sub == "team" && a.size() < 5)) {
        Reply(ctx, "Usage: rmm cps team <cp> <0|1|2> | lock <cp> | unlock <cp>");

        return;
    }

    rmm_object_t target = static_cast<rmm_object_t>(std::strtoull(a[3].c_str(), nullptr, 0));
    std::string name;

    for (rmm_object_t cp : game.ControlPoints()) {
        rmm_control_point_info i;

        if (!info(cp, i))
            continue;

        if (cp == target || ToLower(i.name) == ToLower(a[3])) {
            target = cp;
            name = i.name;
            break;
        }
    }

    if (name.empty()) {
        Reply(ctx, "No such control point (rmm cps lists them).");

        return;
    }

    if (sub == "team") {
        int team = std::atoi(a[4].c_str());

        if (team < 0 || team > 2 || !game.HasCap(RMM_CAP_CONTROL_POINT_SET)) {
            Reply(ctx, team < 0 || team > 2 ? "Team is 0 (neutral), 1 or 2." : "Not supported on this game.");

            return;
        }

        Reply(ctx, game.SetControlPoint(target, team) ? Format("%s now belongs to team %d.", name.c_str(), team)
                                                      : "The engine refused.");

        return;
    }

    if (!game.HasCap(RMM_CAP_CONTROL_POINT_LOCK)) {
        Reply(ctx, "Not supported on this game.");

        return;
    }

    bool lock = sub == "lock";
    Reply(ctx, game.LockControlPoint(target, lock) ? name + (lock ? " locked." : " unlocked.") : "The engine refused.");
}

void RMM_CALL RmmCommand(const rmm_cmd_ctx* ctx, void*) {
    Core& core = Core::Get();
    std::string sub = ctx->argv.size() > 1 ? ToLower(ctx->argv[1]) : "";

    if (sub.empty() || sub == "help") {
        Reply(ctx, "Refractor MultiMod " RMM_VERSION_STRING " - rmm <subcommand>:");
        Reply(ctx, "  version | plugins [list|info|load|unload|reload|pause|resume] | cmds");
        Reply(ctx, "  vars [filter] | var <name> [value] | settings | events | caps");
        Reply(ctx, "  admins reload | bans | exec <cfg file>");
        Reply(ctx, "  objects [pattern] [roots] [empty] | object <handle> [stop|pos|vel|rot|damage ...]");
        Reply(ctx, "  find [pattern] [radius [x y z|@player]] | spawn <template> [x y z|@player] [yaw] [ground]");
        Reply(ctx, "  remove <handle> | ground <x z|@player>");
        Reply(ctx, "  bots [list|info|add|remove|removeall|move|patrol|order|strategy|skill|control|input ...] (rmm "
                   "bots help)");
        Reply(ctx, "  kit <target> <kit template> | score <target> <score> | endround [1|2|draw]");
        Reply(ctx, "  putin <target> <vehicle name|nearest|handle> [seat] | eject <target>");
        Reply(ctx, "  items <target> [select <slot> | remove <slot> | lock <slot,...> | unlock]");
        Reply(ctx, "  cps [list] | cps team <cp> <0|1|2> | cps lock|unlock <cp>");
    } else if (sub == "version" || sub == "info") {
        rmm_map_info m{};
        m.struct_size = sizeof m;
        core.Game().CurrentMap(m);
        Reply(ctx, Format("Refractor MultiMod %s, API %d.%d.%d", RMM_VERSION_STRING, RMM_API_VERSION_MAJOR,
                          RMM_API_VERSION_MINOR, RMM_API_VERSION_PATCH));
        Reply(ctx, Format("Game: %s (%s), backend %s, mod %s, map %s", core.Game().GameName(), core.Game().ShortName(),
                          core.Game().Backend(), core.Game().ModName().c_str(), m.map));
    } else if (sub == "plugins") {
        CmdPlugins(ctx);
    } else if (sub == "cmds") {
        for (const Command* c : core.Cmds().List())
            Reply(ctx, Format("%-14s %-8s %s %s", c->name.c_str(), c->owner ? c->owner->logTag.c_str() : "core",
                              c->usage.c_str(), c->description.c_str()));
    } else if (sub == "vars") {
        std::string filter = ctx->argv.size() > 2 ? ctx->argv[2] : "";

        for (const Var* v : core.Vars().List()) {
            if (!filter.empty() && !IContains(v->name, filter))
                continue;

            bool hide = (v->flags & RMM_VAR_PROTECTED) && !core.Admin().HasFlags(ctx->caller, RMM_ADMFLAG_ROOT);
            Reply(ctx, Format("%-28s \"%s\"  %s", v->name.c_str(), hide ? "***" : v->value.c_str(), v->desc.c_str()));
        }
    } else if (sub == "var") {
        CmdVar(ctx);
    } else if (sub == "settings") {
        for (int i = 0; i < RMM_SETTING_COUNT; ++i) {
            std::string var = core.Game().SettingVar(i);
            Reply(ctx, Format("%-16s -> %s", kSettingNames[i], var.empty() ? "(unsupported)" : var.c_str()));
        }
    } else if (sub == "events") {
        for (int32_t id : core.Events().AllEvents()) {
            uint32_t f = core.Events().Flags(id);
            Reply(ctx, Format("%4d %-22s %s%s%s%s", id, core.Events().Name(id),
                              (f & RMM_EVF_SUPPORTED) ? "supported" : "-", (f & RMM_EVF_BLOCKABLE) ? " blockable" : "",
                              (f & RMM_EVF_MUTABLE) ? " mutable" : "", (f & RMM_EVF_CUSTOM) ? " custom" : ""));
        }
    } else if (sub == "caps") {
        for (int i = 0; i < RMM_CAP_COUNT; ++i)
            Reply(ctx, Format("%-16s %s", kCapNames[i], core.Game().HasCap(i) ? "yes" : "no"));
    } else if (sub == "objects") {
        CmdObjects(ctx);
    } else if (sub == "object") {
        CmdObject(ctx);
    } else if (sub == "find") {
        CmdFind(ctx);
    } else if (sub == "spawn") {
        CmdSpawn(ctx);
    } else if (sub == "remove") {
        CmdRemove(ctx);
    } else if (sub == "ground") {
        CmdGround(ctx);
    } else if (sub == "bots" || sub == "bot") {
        CmdBots(ctx);
    } else if (sub == "kit" || sub == "score" || sub == "endround") {
        CmdGameMode(ctx, sub);
    } else if (sub == "items") {
        CmdItems(ctx);
    } else if (sub == "cps" || sub == "cp") {
        CmdControlPoints(ctx);
    } else if (sub == "putin" || sub == "eject") {
        CmdPutIn(ctx, sub);
    } else if (sub == "admins") {
        if (!Allowed(ctx, RMM_ADMFLAG_ROOT))
            return;

        core.Admin().Load();
        Reply(ctx, "Admins and bans reloaded.");
    } else if (sub == "bans") {
        if (!Allowed(ctx, RMM_ADMFLAG_BAN))
            return;

        const auto& bans = core.Admin().Bans();

        if (bans.empty())
            Reply(ctx, "No active bans.");

        for (auto& b : bans)
            Reply(ctx,
                  Format("%s %s (%s) %s - %s [by %s]",
                         b.kind == RMM_ID_AUTH ? "auth"
                         : b.kind == RMM_ID_IP ? "ip"
                                               : "name",
                         b.value.c_str(), b.name.c_str(),
                         b.expires
                             ? Format("expires in %lld min", (long long)((b.expires - plat::UnixTime()) / 60)).c_str()
                             : "permanent",
                         b.reason.c_str(), b.by.c_str()));
    } else if (sub == "exec") {
        if (!Allowed(ctx, RMM_ADMFLAG_RCON))
            return;

        if (ctx->argv.size() < 3) {
            Reply(ctx, "Usage: rmm exec <file in rmm/configs>");

            return;
        }

        std::string path = core.BuildPath(RMM_PATH_CONFIGS, ctx->argv[2]);

        if (ctx->argv[2].find("..") != std::string::npos) {
            Reply(ctx, "Invalid path.");

            return;
        }

        Reply(ctx, core.ExecConfig(path) ? "Executed " + ctx->argv[2] + "." : "Could not open " + path + ".");
    } else {
        Reply(ctx, "Unknown subcommand '" + sub + "'. Try 'rmm help'.");
    }
}

void RMM_CALL HelpCommand(const rmm_cmd_ctx* ctx, void*) {
    Core& core = Core::Get();
    std::string trig = ctx->source == RMM_SRC_CHAT ? core.CoreVarString("rmm.chatTrigger") : "";
    int shown = 0;

    for (const Command* c : core.Cmds().List()) {
        if (ctx->source == RMM_SRC_CHAT && !(c->flags & RMM_CMD_CHAT))
            continue;

        if (c->adminFlags && !core.Admin().HasFlags(ctx->caller, c->adminFlags))
            continue;

        Reply(ctx, Format("%s%s %s - %s", trig.c_str(), c->name.c_str(), c->usage.c_str(), c->description.c_str()));
        ++shown;
    }

    if (!shown)
        Reply(ctx, "No commands available.");
}

}  // namespace

void RegisterBuiltinCommands(Core& core) {
    core.Cmds().Add(nullptr, "rmm", RMM_CMD_ALL, RMM_ADMFLAG_GENERIC, "Refractor MultiMod management", "<subcommand>",
                    &RmmCommand, nullptr);
    core.Cmds().Add(nullptr, "help", RMM_CMD_ALL, 0, "List commands you can use", "", &HelpCommand, nullptr);
}

}  // namespace rmm
