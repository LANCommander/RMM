# Writing RMM plugins

This guide shows how the parts of the API fit together. The headers in
`include/rmm/` are the full reference; every function and struct is
documented there.

| Header | Contents |
|---|---|
| `rmm.h` | Umbrella header for C plugins, plus `rmm_logf` and `rmm_replyf` |
| `rmm.hpp` | Optional C++17 layer: `rmm::Plugin`, `rmm::On<T>`, `rmm::Command`, `rmm::Var`, `rmm::Every` and helpers |
| `rmm_plugin.h` | Plugin exports, `rmm_plugin_info`, lifecycle |
| `rmm_api.h` | The root `rmm_api` table and core services: logging, paths, timers, interfaces |
| `rmm_events.h` | Event ids, payload structs, hooks |
| `rmm_vars.h` | Engine variables, portable settings, RMM variables |
| `rmm_commands.h` | Console and chat commands, engine console execution |
| `rmm_players.h` | Player info, targeting, player actions |
| `rmm_admin.h` | Admin flags, immunity, kicks, bans |
| `rmm_game.h` | Game id, capabilities, maps, messages, tickets, round control |
| `rmm_world.h` | World objects: vehicles, seats, position and velocity, spawning, removal, terrain |
| `rmm_bots.h` | AI bots: inspection, orders, team strategy, skill, removal, input control |

The C++ examples below use `rmm.hpp`. Everything they do is also available
through the C tables (`api->events`, `api->players` and so on).

## A minimal plugin

A plugin is a shared library that exports `RMM_Query` and `RMM_Load`:

```c
#include <rmm/rmm.h>

static const rmm_plugin_info info = {
    RMM_PLUGIN_INFO_HEADER, "My Plugin", "1.0", "me", "Does things", "", "myplug",
    RMM_GAME_ANY, 0 };

RMM_PLUGIN_EXPORT const rmm_plugin_info* RMM_CALL RMM_Query(void) { return &info; }

RMM_PLUGIN_EXPORT rmm_bool RMM_CALL RMM_Load(const rmm_api* api, rmm_plugin_t self,
                                             rmm_bool late, char* err, size_t cap) {
    rmm_logf(api, self, RMM_LOG_INFO, "running on %s", api->game->name());
    return RMM_TRUE;
}
```

- Keep `api` and `self` in globals. `self` identifies your plugin in every
  registration call, which is how RMM cleans up after it.
- To refuse to load (wrong game mode, missing capability, bad config), write
  a message into `err` and return `RMM_FALSE`.
- `late` is true when the plugin is loaded into a running server. Players may
  already be connected, so list them with `players->list` rather than waiting
  for connect events.
- `RMM_AllLoaded`, `RMM_Pause` and `RMM_Unload` are optional exports.

The same plugin in C++:

```cpp
#include <rmm/rmm.hpp>

class MyPlugin : public rmm::Plugin {
public:
    bool OnLoad(bool late, std::string& error) override {
        rmm::Info("running on %s", rmm::Api().game->name());
        return true;
    }
};
RMM_DECLARE_PLUGIN(MyPlugin, "My Plugin", "1.0", "me", "Does things", "", "myplug", RMM_GAME_ANY, 0)
```

Build it with `rmm_add_plugin(myplugin myplugin.cpp)` from
`cmake/RmmPlugin.cmake`. The output must be named `<name>.dll` or `<name>.so`
and go in `rmm/plugins/`.

## Events

```cpp
// POST: the event has happened.
rmm::On<rmm_ev_vehicle>(RMM_EV_VEHICLE_ENTER, [](rmm_ev_vehicle& e) {
    rmm::Info("%s got into %s", rmm::PlayerName(e.player).c_str(), e.vehicle_template);
    return RMM_CONTINUE;
});

// PRE: runs before the engine acts, and can block or change it.
rmm::On<rmm_ev_chat>(RMM_EV_CHAT, [](rmm_ev_chat& e) -> rmm_result {
    if (std::strstr(e.text, "badword")) return RMM_HANDLED;     // drop the message
    return RMM_CONTINUE;
}, RMM_PHASE_PRE, RMM_PRIORITY_HIGH);
```

| Return | Meaning |
|---|---|
| `RMM_CONTINUE` | Nothing to do, or observed only |
| `RMM_CHANGED` | You changed a `[mutable]` field of the payload |
| `RMM_HANDLED` | Block the engine's action. The other PRE hooks still run; POST hooks don't |
| `RMM_STOP` | Block, and skip the remaining PRE hooks |

**Check what the running game supports.** `events->flags(id)` returns a
combination of `RMM_EVF_SUPPORTED`, `RMM_EVF_BLOCKABLE` and
`RMM_EVF_MUTABLE`. On BF2, for example, chat is supported but can't be
blocked, because the Python API reports it after it has been sent. Plugins
should adapt: the chat filter in the bundled `eventmonitor` plugin blocks
where it can, censors where it can, and otherwise warns and then kicks. The
`rmm events` console command prints the table for the running game.

| Event | Payload | Notes |
|---|---|---|
| `SERVER_START`, `SERVER_SHUTDOWN` | none | |
| `FRAME` | `rmm_ev_frame` | Every server frame; every 100 ms on BF2/2142 |
| `MAP_LOAD`, `ROUND_START` | `rmm_ev_map` | |
| `ROUND_END` | `rmm_ev_round_end` | `winner`, and `reason` (the victory type) |
| `GAME_STATUS` | `rmm_ev_game_status` | Pre-game, playing, end-game, paused, loading |
| `PLAYER_CONNECT` | `rmm_ev_player_connect` | Blockable where supported; set `reject_reason` |
| `PLAYER_AUTH` | `rmm_ev_player` | `auth_id` is now valid. The best place for on-join logic |
| `PLAYER_DISCONNECT` | `rmm_ev_player` | The player can still be queried during the event |
| `PLAYER_SPAWN` | `rmm_ev_player_spawn` | |
| `PLAYER_KILLED` | `rmm_ev_player_killed` | Teamkill and suicide flags, weapon, attacker's vehicle |
| `PLAYER_DAMAGE` | `rmm_ev_player_damage` | Hits on a player's soldier. `*damage` is mutable where supported |
| `PLAYER_TEAM`, `PLAYER_SQUAD` | `rmm_ev_player_change` | |
| `PLAYER_SCORE` | `rmm_ev_player_score` | |
| `PLAYER_KIT_PICKUP`, `PLAYER_KIT_DROP` | `rmm_ev_player_kit` | `reason`: spawn, pickup from the ground, plugin... PICKUP is blockable for `RMM_KIT_REASON_PICKUP` only |
| `PLAYER_REVIVED` | `rmm_ev_player_killed` | `victim` is the revived player, `attacker` the medic |
| `PLAYER_WEAPON_SWITCH` | `rmm_ev_weapon_switch` | Blockable for player and network switches. See [Items](#items-and-weapon-switching) |
| `CHAT` | `rmm_ev_chat` | `text` is mutable where supported |
| `RCON_COMMAND` | `rmm_ev_rcon` | Block it to deny a console command |
| `VAR_CHANGED` | `rmm_ev_var_changed` | RMM variables, and engine variables watched with `engine_watch` |
| `VEHICLE_ENTER`, `VEHICLE_EXIT`, `VEHICLE_DESTROYED` | `rmm_ev_vehicle` | `vehicle` may be a seat; `root` and `root_template` are the whole vehicle. See [Vehicles](#putting-players-in-vehicles) for `reason` and blocking |
| `VEHICLE_DAMAGE` | `rmm_ev_vehicle_damage` | Hits on a vehicle, with its occupants. Mutable and blockable where supported |
| `CONTROL_POINT` | `rmm_ev_control_point` | Captured or neutralised. `players` lists who did it (BF1942 and the simulator) |
| `OBJECT_DESTROYED` | `rmm_ev_object_destroyed` | A destructible object other than a vehicle, such as a bridge |
| `SPAWNER_SPAWN` | `rmm_ev_spawner` | A map spawner creates its object. Blockable. See [Map spawners](#map-spawners-and-respawn-time) |
| `BOT_ADDED`, `BOT_REMOVED`, `BOT_ADD_FAILED` | `rmm_ev_bot` | See [Bots](#bots) |
| `PLUGIN_LOADED`, `PLUGIN_UNLOADED` | `rmm_ev_plugin` | Drop any interface pointers you got from a plugin when it unloads |

### Custom events

Plugins can define events for other plugins to hook, like global forwards in
SourceMod:

```cpp
int32_t ev = rmm::Api().events->register_custom(rmm::g_self, "votes.map_vote_started", RMM_EVF_BLOCKABLE);
MyPayload p{...};
if (rmm::Api().events->fire(rmm::g_self, ev, &p) >= RMM_HANDLED) { /* another plugin vetoed it */ }
// Other plugins look it up with events->find("votes.map_vote_started") and hook it as usual.
```

## Commands

```cpp
rmm::Command("slap", RMM_CMD_ALL, RMM_ADMFLAG_SLAY, "Slap a player", "<target>",
    [](rmm::CmdArgs& a) {
        for (rmm_player_t p : rmm::Target(a, a.Arg(1), RMM_TARGET_ALLOW_MULTI))
            a.Replyf("Slapped %s", rmm::PlayerName(p).c_str());
    });
```

One registration covers every way of running the command:

- Console and RCon: `slap Bob`, or `rmm slap Bob`.
- Chat: `!slap Bob`, or `/slap Bob` to hide the command from other players
  where the game allows it.

`a.Reply()` answers wherever the command came from: the console, the RCon
response, or a chat message to the player. RMM checks admin flags before
calling your function; the console always has full access. `rmm::Target`
resolves `#id`, partial names and `@all`, `@t1`, `@t2`, `@alive`, `@dead`,
`@bots`, `@humans`, `@me` and `@!me`. It also enforces immunity, and replies
with an error if no player matches.

### Chat pacing

Refractor's chat box shows only a few lines, for a few seconds. Several lines
sent at once, such as a multi-line command reply, scroll away before anyone
can read them. RMM therefore queues chat per destination (everyone, one team,
or one player) and sends one line per interval:

- The interval is the `rmm.chatInterval` variable: 1 second by default, from
  0 to 10, where 0 turns pacing off. A line to a destination with nothing
  queued goes out at once, so only bursts are spread out. Lines to the same
  destination always stay in order.
- Pacing applies to `say_all`, `say_team`, `players->message` and command
  replies to a player. Console and RCon replies are never paced. Queued
  messages to a player who leaves are dropped.
- Lines longer than the game can show (63 characters on BF1942 and BFV) are
  split at word boundaries, and each part is paced as a line of its own.
- `reply_ex`, `message_ex`, `say_all_ex` and `say_team_ex` take an
  `rmm_msg_opts` to override pacing for one line:
  - `interval`: seconds before the next line to the same destination. A
    negative value means the default; 0 means no gap.
  - `flags = RMM_MSG_IMMEDIATE`: send now, ahead of anything queued.

```cpp
a.Pace(rmm::PaceEvery(2.0f));                 // every Reply() below: 2 s apart
for (...) a.Replyf("#%d %s", id, name);
a.Reply("Done.", rmm::PaceEvery(0));           // options for one reply
rmm::SayAll("Server restarting NOW", rmm::PaceImmediate());
rmm::SayTeam(1, "Hold the bridge");            // default pacing
rmm::Message(player, "Welcome back", rmm::PaceDefault());
```

`say_all`, `say_team` and `message` return true once the line has been sent
or queued.

## Variables and settings

```cpp
rmm::Var enabled("afk.enabled", "1", "Kick idle players", RMM_VAR_ARCHIVE | RMM_VAR_NUMERIC);
rmm::Var seconds("afk.seconds", "180", "Idle time before kick", RMM_VAR_ARCHIVE);
seconds.Bounds(30, 3600).OnChange([](auto& o, auto& n) { rmm::Info("afk time %s -> %s", o.c_str(), n.c_str()); });

if (enabled.Bool()) ...
```

- Names follow Refractor's `object.property` style. Admins set them with
  `rmm var afk.seconds 300`, with `afk.seconds 300` in the console, or in
  `configs/plugins/<plugin>.cfg`, which runs right after the plugin loads.
- Values of `RMM_VAR_ARCHIVE` variables are saved across restarts in
  `configs/rmm_vars.cfg`.
- For server settings, use portable settings so the plugin works on every
  game:

```cpp
rmm::SetSetting(RMM_SETTING_GRAVITY, "-5");              // false if this game has no gravity setting
if (rmm::Api().vars->setting_var(RMM_SETTING_BOT_RATIO)) // is the setting supported here?
```

- For anything else, use the engine directly:
  `vars->engine_get("sv.timeLimit", ...)`, `vars->engine_set(...)` and
  `cmds->engine_exec("admin.restartMap", out, cap)`.

## Players, admin and game

```cpp
for (rmm_player_t p : rmm::Players()) {
    auto info = rmm::GetPlayer(p);            // name, team, score, kit, vehicle, position, auth_id...
}
rmm::Api().admin->kick(p, "AFK", RMM_PLAYER_NONE);
rmm::Api().admin->ban(p, 60, "Teamkilling", caller);         // RMM's own ban list, same on every game
if (rmm::HasCap(RMM_CAP_KILL_PLAYER)) rmm::Api().players->kill(p);
rmm::Api().game->change_map("gulf_of_oman", "gpm_cq", 64);
rmm::SayAll("Hello!");
```

Capabilities (`RMM_CAP_*`) say what the running game and backend can do.
Calls that aren't supported return false. `rmm caps` lists them.

A living player's `position` comes with `forward`, the unit vector their
soldier (or their seat in a vehicle) faces. Both are zero when the player is
dead or the game can't report them (`RMM_CAP_PLAYER_POSITION`).

### Query replies

```cpp
// Server browsers and patched clients see \commsport\27800 in the server's query replies.
rmm::SetQueryRule("commsport", "27800");
rmm::RemoveQueryRule("commsport");
```

`game->set_query_rule(self, key, value)` (`RMM_CAP_QUERY_RULES`) adds a
`\key\value` pair to the server's GameSpy query replies, so a plugin can
announce a service it runs next to the server. A NULL value removes the key.

- The pair goes before `\final\` in every reply, unless the reply has that key
  already.
- Keys are 1-32 and values up to 128 printable ASCII characters, without `\`.
  A key another plugin set is refused. A plugin's keys go when it unloads.
- `rmm query` on the console lists the pairs and their plugins.
- BF1942/BFV: RMM rewrites the server's own UDP sends (the WSOCK32 `sendto`
  import on Windows; on Linux `librmm.so` exports `sendto`).

The bundled `voicechat` and `fastdl` plugins use this (see
[Voice chat and FastDL](#voice-chat-and-fastdl)).

## Support by game

The rest of this guide covers features that depend on the game. On BF1942
they work on both the Linux and Windows servers. BFV supports a subset; see
[GAME_SUPPORT.md](GAME_SUPPORT.md). The simulator supports everything.

| Feature | BF1942 | BF2 / 2142 |
|---|---|---|
| Player damage and health | yes | yes; the attacker isn't credited, and max health is unknown |
| `PLAYER_DAMAGE`, `VEHICLE_DAMAGE` events | yes, blockable and mutable | BF2 Linux server only |
| `players->push` | yes | no |
| Find vehicles, object info, occupants | yes | yes |
| `set_position`, `set_rotation` | yes | yes |
| Object velocity | yes | no |
| `find_objects`, `spawn_object`, `remove_object`, `ground_height` | yes | no |
| `damage_object` | yes | no |
| `enter_vehicle`, `exit_vehicle`; blockable enter and exit | yes | no |
| `set_kit`, `end_round` | yes | no |
| Blockable `KIT_PICKUP` | yes | no |
| `set_score`, tickets | yes | yes |
| Items and `PLAYER_WEAPON_SWITCH` | yes (`remove_item` disabled) | no |
| `SPAWNER_SPAWN`, spawn timing | yes | no |
| Bots | yes, in co-op rounds | no |

Features that still need confirming on a live server are listed in
[GAME_SUPPORT.md](GAME_SUPPORT.md#testing-status).

## Damage, health and knock-back

```cpp
// Mirror friendly fire: the victim takes a quarter, the attacker the rest.
rmm::On<rmm_ev_player_damage>(RMM_EV_PLAYER_DAMAGE, [](rmm_ev_player_damage& e) -> rmm_result {
    auto v = rmm::GetPlayer(e.victim), a = rmm::GetPlayer(e.attacker);
    if (!v || !a || e.victim == e.attacker || v->team != a->team) return RMM_CONTINUE;
    rmm::DamagePlayer(e.attacker, *e.damage * 0.75f);   // no attacker, so a death isn't scored as a teamkill
    *e.damage *= 0.25f;
    return RMM_CHANGED;
}, RMM_PHASE_PRE);
```

- `players->damage(p, amount, attacker)` (`RMM_CAP_DAMAGE_PLAYER`) doesn't
  raise `PLAYER_DAMAGE`, so calling it from that event's handler can't loop.
  On BF1942, a call made while the engine is applying damage takes effect at
  the end of the frame.
- On BF2 the damage events need the Linux server's native hook
  (`[linux] native = 1` in `gamedata/bf2.ini`). Check `events->flags()`.
- `players->get_health(p, &hp, &max)` (`RMM_CAP_PLAYER_HEALTH`). `max` is -1
  where the game doesn't report it (BF2/2142).
- Hits on vehicles arrive as `VEHICLE_DAMAGE`, with the list of occupants.

`players->push(p, {x, y, z})`, or `rmm::PushPlayer` (`RMM_CAP_PLAYER_PUSH`),
adds a velocity in metres per second to a living player's soldier. Y is up:
`{0, 8, 0}` throws a player about a metre and a half into the air, and 8 m/s
is also the speed at which BF1942 plays its explosion knock-back animation.
The engine handles gravity, landing and fall damage. Pushing a player in a
vehicle pushes the whole vehicle. The `basecommands` plugin uses this for
`slap <target> [damage=10] [launch=8]`.

## World objects

```cpp
// Stop empty carriers from drifting.
rmm::Every(1.0, [] {
    for (rmm_object_t ship : rmm::FindVehicles("*carrier*", RMM_FIND_ROOTS_ONLY | RMM_FIND_EMPTY_ONLY))
        rmm::StopObject(ship);
    return true;
});

// Was that a plane they just got into?
rmm::On<rmm_ev_vehicle>(RMM_EV_VEHICLE_ENTER, [](rmm_ev_vehicle& e) {
    bool plane = IsAircraft(e.root_template);   // your own list of templates
    ...
});
```

- A vehicle is a tree of objects: each seat (a gunner position, for example)
  is a child of the vehicle's **root** object. Event payloads give both.
  `world->get_info` reports `root`, `is_root` and the number of occupants.
  `set_position`, `set_velocity` and `set_rotation` always act on the root.
- Handles become invalid when the object is destroyed (a respawned vehicle is
  a new object) and when the map changes. Every call checks the handle, so
  using an invalid one just fails. Find vehicles again after `MAP_LOAD`.
- Patterns are case-insensitive wildcards, separated by commas:
  `"*carrier*,*yorktown*"`.
- `rmm objects [pattern] [roots] [empty]` in the console lists vehicles with
  their handles, and `rmm object <handle> [stop | pos x y z | vel x y z | rot
  y p r]` shows or moves one. Use these to look up a map's template names.

### Any object, spawning, terrain

```cpp
// Every building-like object on the map. Cache the result: this walks thousands of objects.
auto houses = rmm::FindObjects("*citymesh*,*ruin*", RMM_FIND_NO_VEHICLES | RMM_FIND_NO_SOLDIERS);

// Drop a kit 4 m east of one, 0.3 m above the ground, turned 90 degrees.
rmm_object_t kit = 0;
if (auto info = rmm::ObjectInfo(houses[0])) {
    rmm_vec3 p = info->position;
    kit = rmm::SpawnObject("German_Assault", {p.x + 4, 0.3f, p.z}, {90, 0, 0}, RMM_SPAWN_ON_GROUND);
}

// What's near a player? Nearest first.
auto nearby = rmm::FindObjectsNear(rmm::GetPlayer(p)->position, 25.0f);
rmm::RemoveObject(kit);                      // removed for every client too
std::optional<float> h = rmm::GroundHeight(x, z);
```

- `find_objects(pattern, center, radius, flags, ...)` lists **top-level**
  objects of every kind: buildings, props, kits on the ground, vehicles,
  soldiers, spawn points, control points. `RMM_FIND_NO_VEHICLES` and
  `RMM_FIND_NO_SOLDIERS` filter the list, and giving a center sorts it
  nearest first. The handles work with `get_info`, `set_position`,
  `remove_object` and the other object functions.
- `spawn_object(template, pos, ypr_degrees, flags)` creates an object the
  way the engine's vehicle spawners and kit drops do. Players only see
  **networked** templates, such as vehicles and kits;
  `rmm_object_info::is_networked` tells you which an object is. The template
  must also exist on the clients, in the base game or the running mod.
- Spawn vehicles in a clear spot. On BF1942, a vehicle that starts with its
  wheels inside certain objects is destroyed by collision damage
  immediately, and vehicles won't spawn within 2 m of a soldier.
- `remove_object` removes the object's root from the server and every client.
  The engine kills anyone sitting in a removed vehicle. Soldiers can't be
  removed.
- `ground_height(x, z)` is the height of the terrain only, not of buildings.
  With `RMM_SPAWN_ON_GROUND`, `position.y` is a height above the terrain.
- Console: `rmm find [pattern] [radius [x y z | @player]] [novehicles]
  [nosoldiers] [summary]` (`summary` counts objects per template, which is a
  quick way to learn a map), `rmm spawn <template> [x y z | @player] [yaw]
  [ground]`, `rmm remove <handle>` and `rmm ground <x z | @player>`.

### Destroying vehicles

```cpp
// Blow up a vehicle: the engine's own explosion and wreck, and the occupants die.
if (auto i = rmm::ObjectInfo(tank); i && !i->is_destroyed)
    rmm::DamageObject(tank, i->health + 1000.0f, killer);
```

`world->damage_object(obj, amount, attacker)` (`RMM_CAP_OBJECT_DAMAGE`) hits
the root of a vehicle or other armoured object the way a weapon would. Enough
damage destroys it and raises `VEHICLE_DESTROYED` or `OBJECT_DESTROYED`; no
damage event is raised. It refuses soldiers (use `players->damage`). Called
from a KILLED or DAMAGE handler, BF1942 applies it at the end of the frame.
In the console: `rmm object <handle> damage <amount>`.

## Putting players in vehicles

```cpp
// Spawn a tank next to each player and put them in the driver's seat.
rmm::On<rmm_ev_player_spawn>(RMM_EV_PLAYER_SPAWN, [](rmm_ev_player_spawn& e) {
    rmm_vec3 at{e.position.x + 6, e.position.y, e.position.z};
    rmm_object_t tank = rmm::World()->spawn_object("T34", at, rmm_vec3{0, 0, 0}, RMM_SPAWN_ON_GROUND);
    if (tank) { owner[tank] = e.player; rmm::EnterVehicle(e.player, tank, 0); }
    return RMM_CONTINUE;
});
// No getting out and no changing seats. The game's own exits (death and so on) still happen.
rmm::On<rmm_ev_vehicle>(RMM_EV_VEHICLE_EXIT, [](rmm_ev_vehicle& e) -> rmm_result {
    return e.reason == RMM_VEHICLE_REASON_PLAYER || e.reason == RMM_VEHICLE_REASON_SWITCH ? RMM_HANDLED : RMM_CONTINUE;
}, RMM_PHASE_PRE);
// Nobody else gets in.
rmm::On<rmm_ev_vehicle>(RMM_EV_VEHICLE_ENTER, [](rmm_ev_vehicle& e) -> rmm_result {
    auto it = owner.find(e.root);
    return it != owner.end() && it->second != e.player ? RMM_HANDLED : RMM_CONTINUE;
}, RMM_PHASE_PRE);
```

- `players->enter_vehicle(player, vehicle, seat)` (`RMM_CAP_PUT_IN_VEHICLE`)
  puts a living player into any seat of a vehicle, wherever the vehicle is.
  Seat -1 means the first free seat, starting with the driver. A player who
  is already in a vehicle leaves it first. `exit_vehicle` takes a player out.
  Both raise ENTER or EXIT with reason `PLUGIN`, which can't be blocked.
  Called from a KILLED or DAMAGE handler, BF1942 applies them at the end of
  the frame.
- Seats: `rmm_object_info::seat` and `seat_occupant` for each seat object
  (`find_vehicles` lists them), and `rmm_player_info::seat` for a player.
- `rmm_ev_vehicle::reason` is one of:
  - `PLAYER`: the use key (bots included).
  - `SWITCH`: F1-F8. A switch is an EXIT from the old seat followed by an
    ENTER into the new one.
  - `SPAWN`: the player spawned into the vehicle.
  - `GAME`: death, respawn, the vehicle being destroyed, and so on.
  - `PLUGIN`: `enter_vehicle` or `exit_vehicle`.
- ENTER is blockable for every reason except `SWITCH` and `PLUGIN`; blocking
  shows the engine's own "can't enter" response. EXIT is blockable for
  `PLAYER` and `SWITCH`; blocking a switch's EXIT cancels the switch.
- A player holding the key retries on every input frame (up to about 15
  times a second on BF1942), so a blocked enter or exit raises an event each
  time. Rate-limit any message you send in response.

## Kits, score and round end

These are enough for game modes such as gun game, knife fights or
infantry-only rounds:

```cpp
// Give each player their ladder kit when they spawn.
rmm::On<rmm_ev_player_spawn>(RMM_EV_PLAYER_SPAWN, [](rmm_ev_player_spawn& e) {
    rmm::SetKit(e.player, LadderKit(e.player));          // e.g. "German_Assault", "Rus_AT"
    return RMM_CONTINUE;
});
// Move up a level on each kill.
rmm::On<rmm_ev_player_killed>(RMM_EV_PLAYER_KILLED, [](rmm_ev_player_killed& e) {
    if (e.attacker == RMM_PLAYER_NONE || e.suicide) return RMM_CONTINUE;
    if (++level[e.attacker] == kLadderSize) { rmm::EndRound(TeamOf(e.attacker)); return RMM_CONTINUE; }
    rmm::SetKit(e.attacker, LadderKit(e.attacker));
    rmm::Api().players->set_score(e.attacker, level[e.attacker]);
    return RMM_CONTINUE;
});
// No picking up other kits, and no vehicles.
rmm::On<rmm_ev_player_kit>(RMM_EV_PLAYER_KIT_PICKUP, [](rmm_ev_player_kit& e) {
    return e.reason == RMM_KIT_REASON_PICKUP ? RMM_HANDLED : RMM_CONTINUE;
}, RMM_PHASE_PRE);
rmm::On<rmm_ev_vehicle>(RMM_EV_VEHICLE_ENTER, [](rmm_ev_vehicle&) { return RMM_HANDLED; }, RMM_PHASE_PRE);
```

- `players->set_kit(player, template)` (`RMM_CAP_SET_KIT`) replaces the whole
  loadout of a living player on foot. BF1942 can't give single weapons, so
  the kit is the unit: a ladder is a list of kit templates, from any team or
  from a mod. The kit must exist on the clients. The old kit is removed, not
  dropped. `set_kit` raises KIT_DROP and KIT_PICKUP with reason
  `RMM_KIT_REASON_PLUGIN`.
- On BF1942, `set_kit` takes effect at the end of the frame when it is called
  from a KILLED or DAMAGE handler, and always for bots. A bot's AI plan is
  reset, because it refers to the old kit's weapons; if RMM can't find the
  bot's AI, `set_kit` fails.
- Only pickups from the ground (`RMM_KIT_REASON_PICKUP`) can be blocked; the
  kit stays where it is. Kits handed out at spawn and by `set_kit` can't be
  blocked, because the engine has already removed the old kit.
- `players->set_score` (`RMM_CAP_SET_SCORE`) sets the scoreboard score.
- `game->end_round(winner)` (`RMM_CAP_END_ROUND`) ends the round as if the
  score limit had been reached. Pass team 1 or 2, or `RMM_TEAM_NONE` for a
  draw. BF1942 applies it on the next server frame, and `ROUND_END` then
  reports the winner.
- `game->get_tickets` and `set_tickets` read and set a team's current
  tickets.

BF1942's kit, weapon and vehicle template names are listed in
[BF1942_WEAPONS.md](BF1942_WEAPONS.md).

## Items and weapon switching

```cpp
// Pistols only: keep everyone on slot 2.
rmm::On<rmm_ev_weapon_switch>(RMM_EV_PLAYER_WEAPON_SWITCH, [](rmm_ev_weapon_switch& e) -> rmm_result {
    if (e.reason != RMM_WEAPON_REASON_PLAYER && e.reason != RMM_WEAPON_REASON_NETWORK) return RMM_CONTINUE;
    return e.to_slot == 2 ? RMM_CONTINUE : RMM_HANDLED;
}, RMM_PHASE_PRE);
for (auto& i : rmm::Items(p)) if (i.slot == 2) rmm::SelectItem(p, 2);
```

A soldier carries one item per slot. On BF1942 the slots are 1 knife,
2 pistol, 3 main weapon, 4 grenade or explosive, and 5 and up for tools. The
following need `RMM_CAP_ITEMS` and work on living players on foot:

- `players->get_items(player, out, cap)` lists the items as `rmm_item_info`
  (`slot`, `weapon`, `active`).
- `select_item(player, slot)` makes the player hold an item.
- `remove_item(player, slot)` takes an item away until the player's next kit.
  **On BF1942 it is disabled and always returns false:** in testing, the
  server later fired the removed weapon and exited with a fatal error.

`RMM_EV_PLAYER_WEAPON_SWITCH` (`rmm_ev_weapon_switch`) fires before and after
a soldier changes the item in their hands. It has `from_slot` and `to_slot`,
the two weapon templates, and a `reason`:

- `PLAYER`: the player's weapon keys.
- `NETWORK`: state received from the network.
- `GAME`: a new kit, running out of ammo, the detonator after an explosive
  pack.
- `PLUGIN`: `select_item`.

A PRE handler can block `PLAYER` and `NETWORK` switches by returning
`RMM_HANDLED`; the player keeps the current item. On BF1942 the player's
client starts the switch animation, then the server puts the old weapon back
in their hands.

On BF1942, wait about a second after `set_kit` before calling `select_item`.
Called in the same frame, the switch reached the client before the new kit
did, and left the player's game in a broken state (view stuck pointing at the
ground).

`rmm items <target> lock 3` tries out switch blocking without a plugin.

## Map spawners and respawn time

```cpp
// No map vehicles; remember where they would have been.
rmm::On<rmm_ev_spawner>(RMM_EV_SPAWNER_SPAWN, [](rmm_ev_spawner& e) {
    spots[e.spawner] = e.position;
    return RMM_HANDLED;
}, RMM_PHASE_PRE);
// Let dead players spawn again at once.
if (auto t = rmm::GetSpawnTiming()) { t->delay = 0; t->waves = RMM_FALSE; rmm::SetSpawnTiming(*t); }
```

`RMM_EV_SPAWNER_SPAWN` (`rmm_ev_spawner`) fires when one of the map's object
spawners creates its object: map vehicles, stationary guns and the like.

- It carries the spawner, the object's template, position and rotation, and
  the spawner's team. In POST it also carries the new object.
- A PRE handler can block it, and then nothing spawns. The spawner tries
  again after its usual respawn time (on BF1942, at least 5 seconds).
- Spawner positions are where the map's designer placed each vehicle, so they
  are clear of buildings. That makes them good places to spawn your own
  vehicles.
- Vehicles already on the map aren't affected. Remove them with
  `remove_object`; removing an empty one is safe.

`game->get_spawn_timing` and `set_spawn_timing` (`rmm_spawn_timing`,
`RMM_CAP_SPAWN_TIMING`) control how soon dead players can spawn again:

- `delay`: the wait when spawn waves are off.
- `waves`: players wait for the next wave instead.
- `wave_interval`: the time between waves.

Changes apply to players who die afterwards, for the current round. Loading a
level restores the server's own settings (on BF1942, `game.serverSpawnTime`
and `game.serverSpawnDelay`), so apply yours again on `ROUND_START`.

## Control points

```cpp
// Hand every flag but the bases to team 1, and make the first one the only one in play.
for (rmm_object_t cp : rmm::ControlPoints()) {
    auto i = rmm::ControlPointInfo(cp);
    if (!i || i->fixed) continue;
    rmm::SetControlPoint(cp, 1);
    rmm::LockControlPoint(cp, cp != first);
}
```

These are `world->control_points`, `get_control_point`, `set_control_point`
and `lock_control_point`, with the capabilities `RMM_CAP_CONTROL_POINTS`
(list and info), `RMM_CAP_CONTROL_POINT_SET` and `RMM_CAP_CONTROL_POINT_LOCK`.

- `rmm_control_point_info` holds the template name (the same name
  `RMM_EV_CONTROL_POINT` carries), the position and capture radius, and the
  owner. While a point is being raised it also has `capturing_team`. `state`
  is held, losing or taking. `fixed` and `only_team` show what the map itself
  forbids (BF1942 `unableToChangeTeam`, `onlyTakeableByTeam`), and `locked`
  shows your lock.
- `set_control_point(cp, team)` does what a capture does, but at once: the
  spawn points and map vehicle spawners attached to the point change sides
  with it. It also works on fixed and locked points. Nobody scores. One
  `RMM_EV_CONTROL_POINT` follows, with no players.
- `lock_control_point(cp, true)` takes a point out of play: whoever stands at
  it, its flag doesn't move. Locking cancels a capture in progress. Every
  lock is cleared before the next round starts, so lock again on
  `ROUND_START`.
- `rmm cps` on the console lists the points; `rmm cps team|lock|unlock <cp>`
  tries the calls.

The rush plugin (`plugins/rush`) is built on these.

## Bots

`api->bots` (`rmm_bots.h`) controls the engine's AI. Bots are also players, so
the `players` functions work on them; the bots API adds the AI side. Every
function takes the bot's **player id**.

```cpp
if (!rmm::BotsAvailable()) return;                 // BF1942/BFV: co-op rounds only
for (rmm_player_t p : rmm::BotList()) {
    auto b = rmm::BotInfo(p);                      // team, kit, position, goal, behaviour...
    if (b && b->team == 2) rmm::Bots()->move_to(p, flagPos, 10.0f);
}
rmm::Bots()->set_strategy(1, "north");            // a name from rmm::BotStrategies(1)
rmm::Bots()->set_skill(0.4f);                      // applies now; RMM_SETTING_BOT_SKILL waits for the next round
```

- `available()` says whether the AI is running. On BF1942 and BFV it only
  exists in co-op rounds, and every other bots call fails outside them. RMM
  checks with a harmless command first, because one of the engine's AI
  commands crashes a server that isn't in co-op.
- `list`, `count`, `is_bot`, `get_info` and `behaviours`. `rmm_bot_info`
  holds the engine's bot id, team, kit type (`KTAssault`...), vehicle,
  position, **goal** (where its pathfinding is heading) and current
  **behaviour** (`Idle`, `MoveTo`, `Fire`...). `get_info` takes several
  console commands, so don't call it for every bot every frame.
- `move_to(p, pos, radius)`, `patrol(p, points, 1..2, radius)` and
  `order(p, behaviour, modifiers)` are suggestions: each team's strategic AI
  hands out new goals every few seconds. `order` returns true once the order
  is sent; the engine ignores unknown behaviour names without an error.
- `get_strategy`, `strategies` and `set_strategy` steer a whole team. Only
  names from `strategies(team)` are accepted, and the change takes effect at
  the strategic AI's next update, about a second later.
- `remove` and `remove_all`. On BF1942 the engine can only detach the AI from
  a dead bot, so a living bot is killed first and removed a frame or two
  later. Co-op would immediately replace it, so RMM keeps the slot empty for
  the rest of the round (`[bots] keep_removed = 1` in gamedata); the next
  round starts full again.
- `add(&spawn)` is asynchronous. It returns true if the request was accepted,
  followed later by `RMM_EV_BOT_ADDED` (with your `spawn.tag`) or
  `RMM_EV_BOT_ADD_FAILED`. RMM picks a unique, console-safe name if yours is
  taken. **On BF1942 and BFV adding is turned off** (`[capabilities]
  bot_add = 0`): a bot added beyond the round's starting count hung the
  server in the strategic AI.
- `RMM_EV_BOT_ADDED` and `RMM_EV_BOT_REMOVED` fire whatever the cause,
  including co-op's own filling and map changes. RMM compares bot lists once
  a second, and only while a plugin is hooking one of these events. Bots
  already present when you hook aren't reported as added.

Changing the maximum bot count while the server is running crashes BF1942.

### Bot input control

Every server frame, a bot's AI fills the same 55-slot input record that a
player's keyboard and mouse produce (`rmm_bot_input`, slots `RMM_BI_*`: the
yaw, pitch, roll, throttle and mouse-look axes, then buttons such as fire,
jump, use, crouch, prone and weapon 1-9). RMM hooks the point where that
record enters the engine (`AIPlayer::addInput`), so a plugin or an external
program can drive the bot:

```cpp
rmm::SetBotControl(bot, RMM_BOT_CONTROL_EXCLUSIVE);   // ignore the engine AI's input
rmm::SetBotInput(bot, rmm::BotInput().Set(RMM_BI_THROTTLE, 1).Press(RMM_BI_FIRE).Hold(250));
auto seen = rmm::BotEngineInput(other);              // what the engine AI is doing, e.g. as training data
```

- `OVERRIDE`: the engine AI keeps driving, and only the slots in your mask
  are replaced. `EXCLUSIVE`: only your slots count, and the rest read as
  released.
- `set_input` holds: the input is applied every frame until you replace it,
  or until `hold_ms` runs out. Use `hold_ms` as a dead man's switch for an
  external program: if it stops sending, the bot stops rather than running
  off.
- Buttons stay pressed while set. To tap one (a single shot, a jump), send 1
  and then 0.
- The engine AI keeps running underneath. It still chooses spawn points and
  kits, and `get_info` still reports its goal and behaviour.
- Each bot has one controller. The console (`rmm bots control|input`) can
  always take over. Control ends on a map change, when the bot leaves, and
  when the controlling plugin unloads.
- For perception, use the rest of the API: `players->get_info` (positions,
  health, vehicles), `world->find_objects` and `bots->get_info`. View
  direction isn't available.

## Timers

```cpp
rmm::Every(30.0, [] { rmm::SayAll("Visit example.com"); return true; });   // return false to stop
rmm::After(5.0, [] { ... }, RMM_TIMER_MAP_CHANGE);                          // cancelled on map change
```

## Interfaces between plugins

```cpp
// Provider
struct IStatsV1 { uint32_t version; int32_t (RMM_CALL* kills)(rmm_player_t); };
static IStatsV1 g_iface{1, &KillsImpl};
rmm::Api().core->iface_register(rmm::g_self, "IStats", 1, &g_iface);

// Consumer, in OnAllLoaded or when needed. Look it up again after RMM_EV_PLUGIN_UNLOADED.
auto* stats = static_cast<IStatsV1*>(rmm::Api().core->iface_query("IStats", 1, nullptr));
```

Interfaces must be plain C structs of function pointers declared with
`RMM_CALL`, so that they work across compilers and C runtimes, like the main
API.

## Guidelines

1. Never block the server's main thread. Do disk and network I/O on your own
   thread and pass results back through a queue that a timer drains.
2. Copy any string from RMM or from an event payload that you need after the
   call returns.
3. Check `events->flags()` and `game->has_cap()` instead of assuming one
   game's behaviour.
4. Set `struct_size` before calling `get_info`, `current_map` or
   `maplist_get`.
5. Before calling a function added after the API version you target, check it
   with `RMM_HAS(...)`; the `rmm.hpp` helpers do this for you. The API version
   is 1.0.0 (`RMM_API_VERSION_MAJOR`, `MINOR`, `PATCH`). A plugin built
   against a newer minor version than the core is refused; the patch number
   isn't checked.
6. Prefix your variables, commands and custom events with your plugin's name
   (`myplugin.*`).
7. Don't keep `rmm_player_t` values after a player disconnects, because player
   slots are reused. Key long-lived data by `auth_id`.

## Example: gun game

The bundled `plugins/gungame` plugin is a complete game mode built on the
features above. Players work through a ladder of kits and vehicles; kills
move them up a level, and the first to finish the last level wins the round.

The ladder is a text file, `rmm/configs/gungame/<gungame.ladder>.cfg`, or
`configs/gungame/maps/<map>.cfg` for one map. Each line is one level, either
`kit <template>` or `vehicle <template>`, with options such as `kills=`,
`name=`, `kit=`, `seat=`, `exit=`, `height=` and `weapons=`; see
`configs/gungame/default.cfg`. Four ladders are included: `default` (mixed),
`classic` (infantry), `armour` and `pacific`. Levels whose template the
current map hasn't loaded are skipped.

How it works:

- **Vehicle levels:** the plugin spawns the vehicle with `spawn_object` at
  the nearest free map spawner position (collected from `SPAWNER_SPAWN`)
  within `gungame.spotRange`, or else at the clearest of eight spots around
  the player, then seats the player with `enter_vehicle`. `VEHICLE_EXIT` and
  `VEHICLE_ENTER` handlers keep players in their own vehicle and out of
  others. If the owner dies inside, the vehicle is blown up with
  `damage_object`.
- **Weapon checks:** a kit can't be reduced to a single weapon, so the check
  happens on the kill. A level's `weapons=` patterns are matched against
  `rmm_ev_player_killed::weapon`. On vehicle levels, only kills from the
  player's own vehicle count, because BF1942 reports every gun on a vehicle
  as the vehicle. Kills the game reports without a weapon always count.
- **Weapon lock** (`gungame.lockWeapons`): `PLAYER_WEAPON_SWITCH` handlers
  stop players switching to anything but the level's weapons, and the plugin
  selects the level's weapon a second after handing out the kit.
- Everything that touches the engine runs from the frame event, one step per
  frame (exit, remove the old vehicle, kit, spawn, enter). Nothing is spawned
  from inside an engine callback.

Settings, in `configs/plugins/gungame.cfg`:

| Variable | Default | Effect |
|---|---|---|
| `gungame.ladder` | `default` | The ladder file to use |
| `gungame.strictWeapons` | 1 | Only kills with the level's weapons count |
| `gungame.knifeWeapons` | `*knife*,*bayonet*` | Weapons that count as knives |
| `gungame.knifeCounts` | 1 | Knife kills count on every level |
| `gungame.knifeDemote` | 1 | Levels a knifed player loses |
| `gungame.lockWeapons` | 0 | Players can only switch to the level's weapons |
| `gungame.vehicleExit` | 0 | Players may leave their vehicle |
| `gungame.vehicleSwitch` | 1 | Players may change seats (F1-F8) in their vehicle |
| `gungame.vehicleReenter` | 1 | Players who got out may get back into their own vehicle |
| `gungame.otherVehicles` | 0 | Players may enter vehicles that aren't theirs |
| `gungame.vehicleExplode` | 1 | Blow up a vehicle when its owner dies inside |
| `gungame.spotRange` | 300 | How far (in metres) to look for a map spawner position |
| `gungame.mapVehicles` | 0 | Let the map's own vehicles spawn; with 0, existing ones are removed once empty |
| `gungame.respawnTime` | 0 | Respawn delay in seconds, with waves off; -1 leaves the server's setting. Restored when gun game stops |

The config file documents the rest. By default, kit pickups are blocked,
the scoreboard score shows the level, and tickets are held at their starting
value. Players can use `!gg` (their level), `!gg ladder`, `!gg top` and
`!gg stuck` (a new vehicle). Admins have `gg set`, `gg restart` and
`gg reload`.

## Voice chat and FastDL

These two bundled plugins provide the server side of the Battlefield 1942
patch's voice chat and FastDL downloads. Both announce themselves in the
server's query replies ([Query replies](#query-replies)), so players don't need
any extra settings.

**`plugins/voicechat`** runs the voice server of the LANCommander Interposer's
voice API next to the game server, and mirrors the game into it:

- Every human player is registered under voice id = player id + 1. A client
  logs in by claiming its own game player, so voice always matches who is on
  the server.
- Teams 1 and 2 are server groups ("Axis" and "Allies" on BF1942), which the
  clients use as the team radio.
- Living players' `position` and `forward` drive proximity chat and its stereo
  panning, every `voicechat.updateInterval` seconds. Dead players leave
  proximity but keep the radio.
- The voice port goes into the query replies as `\commsport\`.

The voice server is compiled into the plugin. It links the Interposer's
embeddable static library with only the VOICE feature (see
[Building with the Interposer](../README.md#building-with-the-lancommander-interposer)),
so there is no Interposer DLL to deploy. The plugin doesn't attach the
Interposer: none of its hooks, config files, session log or plugins run in the
server, only the voice API. Without the Interposer at build time the plugin
isn't built. Open the voice port (UDP, default 27800).

| Setting | Default | Meaning |
|---|---|---|
| `voicechat.enabled` | 1 | Host the voice server |
| `voicechat.port`, `voicechat.bindAddress` | 27800, all | Where it listens |
| `voicechat.password` | (none) | Clients must also give this (their `voiceChat.password`) |
| `voicechat.adminKey` | (none) | Enables the voice server's remote admin channel |
| `voicechat.maxPlayers`, `voicechat.maxBitrate` | 256, 32000 | Limits |
| `voicechat.proximityRange`, `voicechat.proximityFullVolumeRange` | 50, 4 | Metres: silent beyond, full volume within |
| `voicechat.updateInterval` | 0.1 | Seconds between position and team updates |
| `voicechat.includeBots` | 0 | Register bots too (testing) |

The port, address, password and limits apply when the voice server starts.
After changing them, run `voiceadmin restart`. `voice` shows how many players
are connected. `voiceadmin <command>` (flag `h`) runs the voice server's own
admin commands: `status`, `players`, `groups`, `kick`, `mute`, `limit`,
`help`.

**`plugins/fastdl`** adds `\fastdlport\`, `\fastdlhost\` and `\fastdlpath\`.
Patched clients download missing maps and mods from
`http://<host>:<port><path>/...` before they join. Any web server that serves
the game folder's layout works; the plugin only announces it. The settings are
`fastdl.port` (0 = off), `fastdl.host` (empty = the game server's address) and
`fastdl.path`. `fastdl` shows what is announced.

## Console commands

| Command | Effect |
|---|---|
| `rmm plugins list` | Id, name, version and state (running, paused, FAILED) |
| `rmm plugins info <plugin>` | Details, including the last error or crash |
| `rmm plugins load <file>` | Load a plugin from `rmm/plugins/` |
| `rmm plugins unload\|reload <plugin>` | Unload or reload a running plugin. The DLL can be replaced first |
| `rmm plugins pause\|resume <plugin>` | Stop or resume delivering events, commands and timers |
| `rmm cmds`, `rmm vars [filter]`, `rmm var <name> [value]` | List commands and variables, or show and set one |
| `rmm events`, `rmm caps`, `rmm settings` | What the running game supports |
| `rmm query` | The plugins' extra pairs in the server's query replies |
| `rmm objects [pattern] [roots] [empty]` | Vehicles, with handles, position, velocity, health and occupants |
| `rmm object <handle> [stop\|pos\|vel\|rot\|damage ...]` | Show a vehicle, or change it (flag `m`) |
| `rmm find [pattern] [radius [x y z\|@player]] [novehicles] [nosoldiers] [summary]` | Any world object |
| `rmm spawn <template> [x y z\|@player] [yaw] [ground]`, `rmm remove <handle>` | Create or remove an object (flag `m`) |
| `rmm ground <x z\|@player>` | Terrain height |
| `rmm kit <target> <kit template>`, `rmm score <target> <score>` | Give a kit, set a score (flag `m`) |
| `rmm endround [1\|2\|draw]` | End the round now (flag `g`) |
| `rmm putin <target> <vehicle name\|nearest\|handle> [seat]`, `rmm eject <target>` | Put a player into a seat of the nearest such vehicle, or take them out (flag `f`). `rmm objects` shows seat numbers |
| `rmm items <target> [select <slot>\|remove <slot>\|lock <slot,...>\|unlock]` | List a player's items, select or remove one, or block switching to other slots (flag `m` for changes) |
| `rmm cps [list]`, `rmm cps team <cp> <0\|1\|2>`, `rmm cps lock\|unlock <cp>` | List control points (owner, state, locks, position, radius); give one to a team or lock it (flag `m` for changes). `<cp>` is the name or handle |
| `rmm bots [list\|info\|behaviours\|move\|patrol\|order\|strategy\|skill\|add\|remove\|removeall\|control\|input ...]` | AI bots; `rmm bots help` explains each. Orders and control need flag `m`, configuration flag `h` |
| `rmm admins`, `rmm bans`, `rmm exec <cfg>` | Admin configuration |

Admin flags follow SourceMod's letters: `f` slay, `g` change map, `h`
config, `m` cheats, `z` root. See `rmm_admin.h` for the full list.
