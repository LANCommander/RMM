# Game support

## Summary

| | BF1942 | BFV | BF2 | BF2142 |
|---|---|---|---|---|
| Backend | native | native | python-bridge, plus native hooks on Linux | python-bridge |
| Linux server | yes (`bf1942_lnxded.dynamic`, `LD_PRELOAD`) | yes (`bfv_linded.dynamic`, `LD_PRELOAD`) | yes (`LD_PRELOAD`) | yes (`LD_PRELOAD`) |
| Windows server | 1.61 only (`winmm.dll` proxy) | not yet; RMM declines to load | yes (`rmm_launcher.exe`) | yes (`rmm_launcher.exe`) |
| Tested on a live server | yes, both platforms | partly (Linux) | no | no |
| Event source | engine hooks, plus player-list diffing for joins, leaves and team changes | engine hooks | `host.registerHandler` | `host.registerHandler` |
| Blockable chat | yes, and editable (63 characters) | yes | no | no |
| Console execution | `OldConsole::executeLine` | `OldConsole::executeLine` | `host.rcon_invoke` | `host.rcon_invoke` |

`rmm events` and `rmm caps` in the server console list exactly what the
running game supports.

Reverse-engineering notes for each server binary:
[BF1942 Linux](re/bf1942-linux.md), [BF1942 Windows](re/bf1942-windows.md),
[BF2 Linux](re/bf2-linux.md).

## Battlefield 1942 (`native`)

BF1942 has no scripting layer, so RMM gets everything from hooks on engine
functions. The Linux and Windows backends share one source file,
`src/games/refractor1/r1_native.cpp`.

- **Linux:** `bf1942_lnxded` ships with its full ELF symbol table. RMM resolves
  functions and globals by mangled name from `/proc/self/exe` (gamedata
  `[linux]`), hooks them with funchook, and uses `libstdc++.so.5` to handle the
  engine's GCC 3 `std::string`. To check gamedata against a binary offline:
  `rmm_elfsym bf1942_lnxded.dynamic gamedata/bf1942.ini`.
- **Windows:** `BF1942_w32ded.exe` 1.61 has no symbols. Its functions were
  matched to the named Linux ones and are listed by address in gamedata
  `[windows]`, with the VC++ structure layouts in `[windows_offsets]`. RMM
  checks the PE timestamp and won't attach to any other build. `r1_abi.h`
  covers the ABI differences (`__thiscall`, the VC7 `std::string` from
  `bfcprt.dll`, container layouts). The server loads RMM through the
  `winmm.dll` proxy, and keeps it when it restarts itself after a map change.

Both platforms support:

- **Players:** the player list (name, id, team, bot flag, IP, alive, kit,
  position, vehicle and seat, score, kills, deaths, ping), the CD-key hash as
  `auth_id` (so identity-based bans and admins work), and connect,
  disconnect, team, spawn (with position and kit), kill (with weapon and
  teamkill flag) and score events.
- **Chat:** blockable and editable, which makes chat commands possible.
  Messages to everyone or to one player through `GameServer::say` and
  `sayClient`.
- **Damage:** blockable and editable player and vehicle damage, with the
  weapon and the vehicle's occupants; direct kill, damage, health and
  knock-back.
- **Vehicles:** enter and exit events with seat and reason, blockable entry,
  blockable exit for the use key and seat switches (the game's own exits,
  such as on death, can't be blocked), and moving players in and out of
  seats.
- **World:** finding vehicles and other objects, position, rotation and
  velocity, spawning and removing objects, damaging and destroying vehicles,
  terrain height, and destroyed-object events.
- **Game modes:** `set_kit` (any kit template, using the engine's own kit
  hand-out), kit pickup and drop events (pickups from the ground are
  blockable), weapon-switch events (blockable) and selecting a held item,
  `set_score`, tickets, `end_round` and the round winner, map spawner events
  (blockable), and respawn timing.
- **Control points:** capture events with the capturing players.
- **Bots** (co-op rounds only): listing and inspecting bots, movement orders,
  team strategy and skill through the engine's AI console (gamedata `bot_*`),
  native bot removal, and per-bot input control through a hook on
  `AIPlayer::addInput`.
- **Console:** running commands and intercepting them, output to the server
  console, and the per-frame tick.

BF1942 has no squads or revives, so those events are never raised.

Not available on BF1942:

- **Adding bots** is turned off (`[capabilities] bot_add = 0`). A bot added
  beyond the round's starting count hung the server in the strategic AI.
  Lowering the maximum bot count at runtime crashes the server as well.
- **`remove_item`** always fails. In a live test the server later fired the
  removed weapon and exited with a fatal error.

### Testing status

All of the above has run on live servers in co-op Berlin with bots. Still to
be confirmed:

- On Windows, everything that needs a human player: chat events and blocking,
  private messages, CD-key auth, ping, kit pickup from the ground, and
  blocking vehicle exits by the use key or a seat switch.
- On both platforms: that clients follow `teleport`, `set_rotation` and
  objects spawned with `spawn_object`; the vehicle explosion from
  `damage_object` on clients; the respawn countdown a human player sees after
  `set_spawn_timing`; and what each bot input axis does.
- The weapon reported for roadkills, mines, explosive packs and artillery
  (see [BF1942_WEAPONS.md](BF1942_WEAPONS.md)).

The full list is the live-test checklist at the end of
[re/bf1942-linux.md](re/bf1942-linux.md).

## Battlefield Vietnam (`native`)

BFV uses the same backend as BF1942. Its Linux server, `bfv_linded.dynamic`,
also ships with symbols, and most mangled names match BF1942's, but the
structure layouts differ. `gamedata/bfv.ini` lists every offset as read from
the BFV binary. A hook is only enabled once everything it reads has been
confirmed for BFV.

- Tested live: joins and leaves, chat and chat commands, the CD-key callback,
  map load, round start and end, spawn with kit and position, and control
  point captures.
- Built but not tested live: kills and score, vehicle enter and exit, and
  damage.
- Not ported: world objects, items, spawners and bot input control. The
  bot console commands are copied from BF1942 and unverified.
- Windows: not supported yet.

## Battlefield 2 and 2142 (`python-bridge`)

- RMM intercepts `Py_Initialize` and registers a `_rmm` native module
  (`src/games/refractor2/`).
- `python/rmm/rmm_bridge.py` handles connect, disconnect, spawn, kill,
  revive, team, squad, score, kit pickup and drop, vehicle enter, exit and
  destroyed, control points, chat, in-game RCon, and game status (map load,
  round start and end). It also provides the functions RMM calls for console
  commands, player info, kill, team, score, teleport, map changes and tickets.
- Gamedata covers console commands and `sv.*` settings.
- Object handles are the engine's `IObject*`, read from each Python object's
  `token` (`_rmm.object_ptr`), so the bridge and the native hooks agree on
  them.

**Native hooks on Linux.** The BF2 1.5 Linux server ships its full symbol
table in both `bin/ia-32/bf2` and `bin/amd-64/bf2`. RMM hooks
`GameLogicDamage::serverGiveDamage`, which provides `PLAYER_DAMAGE` and
`VEHICLE_DAMAGE` as blockable and editable events; the Python API has no
damage event. Virtual calls use slot indices, so one gamedata file covers
both builds. Using the 64-bit server requires a 64-bit build of RMM. Enable
the hooks with `[linux] native = 1` in `gamedata/bf2.ini`.

The Windows `bf2_w32ded.exe` has no symbols or RTTI, so native hooks there
would need byte signatures derived from the named Linux functions.

### Verification checklist (live 1.5 server)

- [x] `bf2_w32ded.exe` imports `Py_Initialize` from `dice_py.dll` by name
  (checked on the BF2Hub 1.5 build), so the import patch applies.
- [x] The Linux `bf2` imports `Py_Initialize` from the separate
  `libdice_py.so`, so `LD_PRELOAD` interposition applies.
- [ ] Remote-thread injection into a suspended server works on current
  Windows versions.
- [ ] Every `# VERIFY` in `rmm_bridge.py`: handler argument lists
  (`PlayerRevived`, `PlayerChangedSquad`, `ChatMessage` channel strings and
  text prefixes), killing with `setDamage(0)`, teleporting with
  `setPosition`, seat detection with `getOccupyingPlayers`, and
  `cp_getParam('team')`. Handler names and the `bf2` package functions
  (`getMapName`, `getModDir`, `getGameMode`, `getMaxPlayers`,
  `getObjectsOfType`, `getVehicle` and others) are confirmed against the
  binary and the shipped `python/bf2`.
- [ ] The output format of `maplist.list` (used by `change_map` and
  `ParseMaplist`).
- [ ] The syntax of `admin.banPlayer` and `game.sayTeam`, the
  `sv.tkNumPunishToKick` setting, and whether `physics.gravity` can be changed
  at runtime.
- [ ] Reading the map size for `rmm_ev_map.size`.
- [ ] RCon integration: call `rmm_bridge.handle_rcon(cmd)` from the command
  dispatch in `admin/default.py`, so RMM commands work from standard RCon
  tools.
- [ ] A private-message mechanism, if the game has one. For now
  `private_message = 0`, and replies fall back to public chat prefixed with
  `@name`.
- [ ] The world and damage functions in `rmm_bridge.py`: the
  `PlayerControlObject` type string for `getObjectsOfType`, `getParent()` for
  vehicle roots, `getDamage()`/`setDamage()` as hit points,
  `setPosition`/`setRotation` on vehicle roots, and that `token` pointers
  match the native hook's `getRootParent` result. Velocity isn't supported:
  the Python API has none.
- [ ] The native damage hook on Linux: correct victim and attacker ids, the
  damage value is what the soldier loses, and blocking has no side effects.
  See [re/bf2-linux.md](re/bf2-linux.md).

Possible native additions on Linux: a hook on the chat broadcast (blockable,
editable chat), a hook on `GameServer::createPlayer` (reject banned players
before they load in), and velocity through the physics nodes.

## Porting to another build

For another Windows build of BF1942, or for BFV, start from the named Linux
function and find its counterpart in the executable through shared strings,
vtables, or callers of functions already found. Confirm its arguments and any
`this` adjustment from the disassembly, then add its address to `[windows]`.

Every vtable slot and field offset the code uses must be confirmed for the
new build. VC++ orders vtables differently from GCC and its `std::string` and
containers are larger, so Linux values are usually wrong on Windows. Finally,
confirm the console command and setting names marked `; VERIFY` in the
gamedata file.

## Adding a game or mod

- **A mod for a supported game** (Desert Combat, Project Reality and so on)
  usually works as is. If the mod renames console commands, copy the gamedata
  file and edit it. Per-mod gamedata overrides (`gamedata/<game>.<mod>.ini`)
  are planned.
- **Another Refractor game** needs an `rmm_game` bit, a detection rule in
  `src/host/bootstrap.cpp`, and either `R2Adapter` (if the game has the Python
  host API) or `R1Adapter` with new gamedata.
