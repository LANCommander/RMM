# Battlefield 1942 Linux server: reverse-engineering notes

Target: `bf1942_lnxded.dynamic` from the standalone package used by
[hootmeow/bf1942-linux](https://github.com/hootmeow/bf1942-linux)
(`linux-bf1942-server.tar`, SHA-256 `6b6a2fc2…0938b8e`).

| Property | Value |
|---|---|
| Format | ELF32 i386, non-PIE (base `0x08048000`) |
| Compiler | GCC 3.2.3 (plus 2.95.3 objects), `libstdc++.so.5` |
| Symbols | full `.symtab` (~50k symbols, C++ names), tiny `.stab`, no DWARF |
| Dynamic exports | 321, so `dlsym` is useless and RMM parses `.symtab` itself |
| `.static` build | also has `.symtab`, at different addresses. It cannot be `LD_PRELOAD`ed, so RMM supports the `.dynamic` build only |

All addresses below are from the `.dynamic` build. RMM never uses raw
addresses on Linux: gamedata (`gamedata/bf1942.ini` `[linux]`) stores mangled
names, and `rmm_elfsym` checks them against any build. The native backend
(`src/games/refractor1/r1_native.cpp`) is shared with the Windows 1.61 server;
Windows addresses, calling conventions and differences are in
`docs/re/bf1942-windows.md`.

## Tooling

Reproducing the symbol and disassembly dumps:

```
nm -C -n bf1942_lnxded.dynamic > symbols_demangled.txt     # MSYS2 mingw32 binutils read ELF fine
objdump -d --no-show-raw-insn bf1942_lnxded.dynamic > disasm.txt
rmm_elfsym bf1942_lnxded.dynamic gamedata/bf1942.ini      # verify gamedata
```

The rest is the maintainer's local setup. None of these scripts are in the
repository; they are named here so the notes below can refer to their output.

- Ghidra 12.2 project with full auto-analysis:
  `C:\Development\Tools\ghidra-projects\bf1942_lnxded`. Query it headlessly,
  read-only:

  ```
  analyzeHeadless C:\Development\Tools\ghidra-projects\bf1942_lnxded bf1942_lnxded ^
    -process bf1942_lnxded.dynamic -noanalysis -readOnly ^
    -scriptPath C:\Development\Tools\re-work\ghidra-scripts ^
    -postScript RmmDecompile.java out.c "~GameServer::handleChatMessage" 0x0813c9b0 "callers:0x0805ee80"
  ```

- Decompiled output: `C:\Development\Tools\re-work\bf1942_lnx\decomp\`
  (`damage.c`, `br_feas*.c`, `gg1.c`, `gg2.c`, `veh1.c`, `veh2.c`,
  `ai_conobjs.txt`).
- Python helpers in `C:\Development\Tools\re-work\bf1942_lnx\scripts`:
  `callers.py` (who calls an address), `disfn.py` (print one function),
  `getters.py` (map trivial getters to field offsets), `vtslot.py` (find a
  function's slot by scanning vtables), `conobjs.py` (rebuild console method
  records, see "AI bots") and `rfa_extract.py` (unpack `.rfa` archives: RFA
  directory + LZO1X segments).
- gdb scripts for the WSL test server in `C:\Development\Tools\re-work\wsl`,
  such as `gdb_spawn_damage.sh "<console cmd>"` (backtrace every
  `_giveDamage` around a command) and `gdb_kit_templates.sh` (list every kit
  template). Others used below: `gdb_kits.sh` (dump the kit and soldier maps
  and every player's team and kit), `gdb_attach_crash.sh` and
  `gdb_spawn_fields.sh` (read the spawn-timing fields).

## ABI

- Linux i386 C++ member functions use cdecl, with `this` as the first stack
  argument, so RMM declares them as free functions. The Windows server uses
  `__thiscall`; see `bf1942-windows.md`.
- `std::string` is the GCC 3 copy-on-write string: one pointer to the
  character data, preceded by `_Rep { size_t length; size_t capacity; int refcount; }`.
  Confirmed by the `lock xadd` on `data-4` and `lea -0xc(data)` in string
  destructors. RMM creates and frees these strings only through
  `libstdc++.so.5` (`_ZNSsC1EPKcRKSaIcE`, `_ZNSsD1Ev`, `_ZNSs6assignEPKcj`,
  version `GLIBCPP_3.2`). See `src/games/refractor1/gcc3_string.*`.
- `std::list` (GCC 3) holds a pointer to a heap sentinel node. Nodes are
  `{ next, prev, value }`.
- GCC 3 `std::map` members are likewise a pointer to the heap header node.
- The vptr points to `vtable + 8`.
- Functions returning `std::string` by value use a hidden return pointer
  that the callee pops (i386 psABI). RMM declares them as returning a
  one-pointer POD by value, which makes a modern GCC use the same
  convention. Arguments of type `std::string` passed by value become a
  pointer to a caller-owned copy (Itanium ABI, non-trivial type).

### Static initialisation order (LD_PRELOAD)

`Attach()` runs from librmm's ELF constructor, before the dynamic
initialisers of the other translation units. A namespace-scope
`std::vector` that `Attach()` filled was then reset by its own constructor,
which is why `player_push` and `object_velocity` reported "no" in the first
live test. Anything `Attach()` writes must be constant-initialised (PODs,
raw pointers, arrays).

## Core hooks and globals

| Role | Symbol | Evidence |
|---|---|---|
| Console execution and interception | `dice::ref2::io::OldConsole::executeLine(std::string const&, std::string&, bool, bool, bool, bool, bool)` (gamedata `console_execute`) | `io::Console` vtable `+0x8`. The 1.61 server runs every console line through it: `.con` scripts, the server console and remote admin. RMM detours it to handle its own commands, and calls it on `mainConsole` to run engine commands (see "Console"). |
| Frame | `dice::bf::Setup::updateConsole()` `080d86e0` | Called from `Setup::mainLoop()` each iteration (`mainLoop` itself is called once, from `Setup::start`). Simple prologue (`push ebp; mov ebp,esp; push eax; push eax`). |
| Players | `dice::ref2::world::playerManager` `086cb72c` | A global `IPlayerManager*` (e.g. `handleChatMessage`: `mov 0x86cb72c,%eax; mov (%eax),%edx; call *0x20(%edx)`). |
| Player list | `PlayerManager::getPlayers()` returns `this + 0x0c` | `add $0xc,%eax; ret` |
| BFPlayer vtable | `_ZTVN4dice2bf8BFPlayerE` `086bb020` | Used to validate player pointers before reading fields |
| GameServer | `dice::bf::game` (gamedata `game_instance`) | The `GameServer*`. Before it was known, RMM captured the instance from the chat hook (`this`) or the auth callback (`instance`); that remains the fallback. |

The newer `console::Parser` / `LocalConsole` classes are linked in but not
used by the 1.61 server. For reference:
`dice::ref2::console::Parser::handleCommand(std::string const&, std::string&, AccessType)`
(`08356250`) returns an int status (e.g. `2`) and is recursive (calls itself
at `083579fc`); `dice::ref2::console::LocalConsole::executeLine(std::string const&, std::string&)`
(`08351c00`) is virtual (no direct callers) and is exactly
`return Parser::handleCommand(this+4, cmd, out, 2)`.

### BFPlayer field offsets (from trivial getters/setters)

| Offset | Field | Source |
|---|---|---|
| `0x44` | name (`std::string`) | `getName() const` returns `this+0x44` |
| `0x4c` | current vehicle (`IObject*`) | `getVehicle()` |
| `0x50` | camera | `getCamera()` |
| `0x54` | input id | `getInputId()` |
| `0x58` | player id | `getId()` / `setId()` |
| `0x5c` | update frequency type | `get/setUpdateFrequencyType` |
| `0x68` | default vehicle (soldier) | `setDefaultVehicle()` |
| `0x78` | is AI (bool) | `get/setIsAIPlayer` |
| `0x7c` | team | `setTeam()`: `cmp %edx,0x7c(%eax)` … `mov %edx,0x7c(%eax)` |
| `0x90` | reset to 0 on team change | `setTeam()` |
| `0x14a` | is remote (bool) | `get/setIsRemote` |

More fields (`+0x79` alive, `+0x80` kit, score counters) are described in
the sections that use them.

### Global objects

| Symbol | Address | Notes |
|---|---|---|
| `dice::bf::setup` | `086c4664` | `Setup*`; `setup->0x410` is the `EventLogger*` (null-checked before every `beginEvent`) |
| `dice::ref2::world::playerManager` | `086cb72c` | `IPlayerManager*` |
| `dice::ref2::world::objectManager` | `086cb724` | `ObjectManager*`, see "World objects" |
| `dice::ref2::world::objectTemplateManager` | `086cb728` | `IObjectTemplateManager*` |
| `dice::ref2::console::manager` | `0874a8f4` | console manager |
| `dice::ref2::io::mainConsole` | `0875285c` | `OldConsole*`, used for console output (`08752860` is the second console `Setup::initConsole` sets up) |

### Messaging and player info

| Function | Use in RMM |
|---|---|
| `GameServer::say(int, const std::string&, bool, bool)` `08140d20` | `say_all`: `say(-1, msg, false, true)`, identical to `GameServer::serverMessage`. 63-char chunks |
| `GameServer::sayClient(int, std::string, bool, bool)` `08141060` | private message: `StringManager::addStringMsg` to that client only |
| `GameServer::getPlayerIPAndPort(int) const` `08141b40` | `"ip:port"`, `"bot"` or `"localhost"` |

### Other GameServer methods

Present in the symbol table: `kickPlayer(int, char const*)`,
`banPlayer(int, std::string const&)`, `banPlayerKey`, `addKeyToBanList`,
`changeMap(std::string const&, std::string const&, std::string const&)`,
`restartMap()`, `killPlayer(int)`, `setTeam(int, int)`, `spawnPlayer(int)`,
`say(int, std::string const&, bool, bool)`, `sayClient(int, std::string, bool, bool)`,
`serverMessage(std::string const&)`, `setNumberOfTickets(unsigned, unsigned)`,
`getPlayerIPAndPort(int) const`, `createAIPlayer(...)`, `loadBots(int)`,
`listPlayers()`. RMM calls `say`, `sayClient`, `getPlayerIPAndPort` and
`killPlayer(int)` directly; kick, ban and map changes go through console
commands (gamedata `[commands]`).

## Console

### Output

`dice::ref2::io::mainConsole` (`0875285c`; one of the two pointers
`Setup::initConsole` uses, so it has a symbol) is an `OldConsole`.
`OldConsole::output(const std::string&)` is vtable `+0x10`, the call
`handleChatMessage` makes to print chat. RMM's log sink uses it after a
vptr check, so RMM output appears on the server console and in remote
consoles, not only on stderr.

### Executing lines and reading results

Verified live.

- A line must end in `\n`. `executeLine` splits its input with
  `istream::getline` and discards any line that ends at EOF: it tests
  `eofbit` right after `getline`, at `0x83f0c4b`. Without the newline every
  command RMM executed was silently dropped.
- Flags are `(echo, print, history, ?, ?)`. The engine's own callers pass
  all false. A line typed at the console arrives with `(1,1,1,0,0)` from
  `OldConsole::updateAsciiKey`:
  - b3 echoes `> line` through `output()`.
  - b4 prints the result when `handleCommand` returns 0.
  - b5 records history.
- With all flags false, the result comes back in `out`: for example `32`
  for `game.serverMaxPlayers`.
- Fallback capture: RMM also detours `OldConsole::output` (`083e3fc0`,
  gamedata `console_output`). While RMM's own `Exec` runs, anything printed
  is captured, and used only if `out` is empty.

`game.listMaps` prints lines like `Map:0 'berlin' Mod:bf1942 GPM:GPM_CQ`.
The shared `ParseMaplist` (`src/games/common/adapter_base.cpp`) does not
parse this format.

### Console names (from the string table)

The binary groups each console object's method names together, which
attributes most commands:

- `admin.*`: `kickPlayer`, `kickPlayerByName`, `banPlayer`, `banPlayerKey`,
  `changeMap`, ban-list management, vote settings, `enableRemoteAdmin`,
  `enableRemoteConsole`
- `game.*` (commands): `sayAll`, `listPlayers`, `listMaps`, `voteMap`,
  `voteKickPlayer`, `TKForgive`; `game.addLevel %s %s %s` is formatted by the binary
- `game.server*` (settings): `serverMaxPlayers`, `serverGameTime`,
  `serverScoreLimit`, `serverTicketRatio`, `serverNumberOfRounds`,
  `serverSpawnTime`, `serverSpawnDelay`, `serverSoldierFriendlyFire(OnSplash)`,
  `serverVehicleFriendlyFire(OnSplash)`, `serverAlliedTeamRatio`,
  `serverAxisTeamRatio`, `serverAutoBalanceTeams`, `serverCoopAiSkill`,
  `serverCoopCpu`, `serverPunkBuster`, `serverAllowNoseCam`,
  `serverExternalViews`, `serverEventLogging`, `serverTKPunishMode`, `serverKickBack`, …
- physics properties near `gravity`: `wind`, `airDensityZeroAtHeight`
- unattributed: `killPlayer`, `setTeam`, `serverMessage`, `restartMap`,
  `runNextLevel`, `setNextLevel`
- There is no `sayTeam`.

## Chat (`GameServer::handleChatMessage(ChatMessage const&)`, `0813bae0`)

The function does everything itself: prints to the server console, writes
the `chat` event-log entry, then sends the message to every connected client
in the client map at `this+0x100`, skipping the sender and, for team chat,
other teams. Not calling the original blocks the message completely.
`GameServer::say(int id, const std::string&, bool teamOnly, bool)` builds the
same struct on the stack and passes it to the same virtual function, which
confirms the layout:

| Offset | Field | Notes |
|---|---|---|
| `0x00` | int kind | 0 = team, otherwise global (`say` stores `!teamOnly`) |
| `0x04` | `char[68]` text | narrow chars; `say` copies ≤ 63 (`strncpy(…, 0x3f)`) |
| `0x48` | text length | must be updated together with the text |
| `0x4c` | sender player id | -1 = server |
| `0x50` | bool server message | `say` sets `id == -1` |
| `0x51` | bool | `say`'s 5th argument |

The wide strings in the function only build the local console display
(`"-- [Axis] name: text"`). Team 1 = Axis and team 2 = Allies. RMM hooks this
function only for player chat (sender ≥ 0, server flag clear), so player
chat is blockable and mutable.

## Authentication and player slots

### CD-key callback (`cdKeyAuthorizeCallback`, `0813c9b0`, file-local)

GameSpy SDK callback `(int gameid, int localid, int authenticated, char* errmsg, void* instance)`,
where `instance` is the `GameServer*`. `localid == -1` is the server's own
key check. On success, with CD-key checking enabled (`setup+0x2a8`), the
engine calls `gcd_getkeyhash(gameid, localid)` (BF1942 GameSpy game id
`0x21d` = 541) and logs `playerKeyHash`, then calls
`GameServer::authentificationOk(instance, gameid, localid)`. RMM lets the
original run, then reads the hash with `gcd_getkeyhash` and uses it as
`auth_id`. `PLAYER_AUTH` fires when the player next shows up in the polled
list.

GameSpy `localid` is the player id: `listPlayers` prints
`gcd_getkeyhash(0x21d, player->getId())`, and `GameServer::sayClient` looks
the client map up with a player id.

### Freeing a slot (`GameServer::destroyPlayer(BFPlayer*)`, `0813aef0`)

It cancels that player's votes, notifies `GameEventManager` and the network
manager, then removes the player from `playerManager`. RMM detours it
(`destroy_player`) and drops the CD-key hash for that id there. Doing it in
the 0.5 s player poll raced with the next occupant's
`cdKeyAuthorizeCallback`, and also wiped the hash on renames, which the poll
sees as leave + join. If the callback never comes (for example on a LAN
server), the player is authenticated without a hash after
`[game] auth_timeout` seconds (default 20). A hash that arrives later fires
`PLAYER_AUTH` again. VERIFY live: whether the callback runs with
`serverInternet 0`.

## Gameplay events

### Spawn, vehicles and score

All four functions are cdecl with the `GameServer*` first. RMM calls the
original first, then compares engine state before and after the call, so
internal early-return branches can't produce false events. All four are
observe-only (vehicle entry and exit blocking is a separate path, see
"Vehicle entry, exit and seats").

| Function | Finding | RMM event |
|---|---|---|
| `spawnPlayer(BFPlayer*)` `08153a00` | Always returns 0. Success shows only as `player+0x79 = 1` (just before the `spawnEvent` log) | `PLAYER_SPAWN` when `+0x79` goes 0 → 1 |
| `enterVehicle(BFPlayer*, IPlayerObject*, bool isDefault, bool isFake)` `081558d0` | `isDefault` = placing the player in their own soldier (calls `setDefaultVehicle`). Logged only when `player+0x79` is set | `VEHICLE_ENTER` when the controlled object changes to something other than the soldier, and neither flag is set |
| `exitVehicle(BFPlayer*, bool isFake)` `08155330` | Several internal paths | `VEHICLE_EXIT` when the controlled object changes away from a vehicle |
| `handleScore(BFPlayer*, ScoreMsg, BFPlayer* victim, int weaponTemplateId, int)` `08151d00` | `ScoreMsg`: 0 FlagCapture, 1 Attack, 2 Defence, 3 Kill, 4 Death, 5 DeathNoMsg, 6 TK, 7 Spawned, 8 Objective, 9 ObjectiveTK | Kill/TK → `PLAYER_KILLED` (attacker, victim, weapon, teamkill). A Death with no Kill for that victim in the same tick → `PLAYER_KILLED` with attacker none |

Vtable slots, read from the vtables in the binary (not inferred):

| Slot | Function |
|---|---|
| `ObjectTemplateManager` vtable `+0x50` | `getTemplate(unsigned int) const`. Ghidra showed two extra `-1` args, which are artefacts |
| `ObjectTemplate` vtable `+0x14` | `getName() const` → `const std::string&` |
| `BFPlayer` vtable `+0x3c` / `+0x48` | `getVehicle()` / `getId()`, the same fields RMM reads directly (`+0x4c`, `+0x58`) |

A world object's template is at `object+0x4c`, and the global
`dice::ref2::world::objectTemplateManager` (`086cb728`) is an
`IObjectTemplateManager*`. `player+0x79` is the alive/spawned flag (RMM's
`is_alive`).

Connect, disconnect, rename and team changes come from comparing the player
list every 0.5 s (observe-only).

### Player stats (`ScoreManager::scoreEvent(BFPlayer*, ScoreMsg)`, `08168830`)

Counters live directly on `BFPlayer`, in two parallel sets that are
incremented identically. One presumably resets per round (VERIFY live).
RMM reports set A.

| Stat | Set A | Set B |
|---|---|---|
| deaths (Death/DeathNoMsg) | `0xa0` | `0xf4` |
| kills | `0xa4` | `0xf8` |
| flag captures | `0xa8` | `0xfc` |
| defences | `0xac` | `0x100` |
| objectives | `0xb0` | `0x104` |
| objective TKs | `0xb4` | `0x108` |
| attacks | `0xb8` | `0x10c` |
| teamkills | `0xbc` | `0x110` |
| score | `0xc0` | `0x114` |

Points per type come from `ScoreManager+0x3c…+0x58`, and per-team totals
from `ScoreManager + team*0x50 + 0x10…`. Scoring only happens for teams 1–2,
and not in game modes 2 and 5. RMM fires `PLAYER_SCORE` when `+0xc0` changes
across `handleScore`.

### Ping

`NetServer::getPing(int id, int* out) const` (`0843f350`) reads
`SWConnection+0x9c` from a connection map keyed by player id. It's slot
`+0x8c` of `vtable for dice::ref2::io::NetServer` (found with `vtslot.py`).
The NetServer is `GameServer+0x18c`: `listPlayers` and `getPlayerIPAndPort`
call the neighbouring slot `+0x90` on it with a player id. RMM checks that
object's vptr against the NetServer vtable before calling.

### Control points

`ControlPoint::gotControl(int team)` → `setTeam(team)`, and
`lostControl(int)` → `setTeam(0)`. `setTeam(unsigned)` (`0828b500`) stores the
team at `+0x174` unless the template's lock flag (`template+0x200`) is set.
The `ControlPointTemplate*` is at `+0x164`, and its `getName()` is vtable
`+0x14` like every `ObjectTemplate`. RMM hooks `setTeam` and fires
`CONTROL_POINT` (name, old team, new team) when `+0x174` changes.

Who took it: `ControlPoint::handleFrameUpdate(float)` (`0828ab70`) has no
capturer field; it decides with a radius test. A player counts when they are
alive (`+0x79`) and the distance from their controlled object (`BFPlayer`
vfunc `+0x3c`, `getVehicle` = `+0x4c`, then `+0x38` position) to the point's
`getAbsolutePosition` is at most the template radius (`template+0x1dc`,
float). After `gotControl(team)` it calls `handleScore` (game vtable `+0x68`)
for every such player with `player+0x7c == team`. A second team in the
radius stops the capture. RMM repeats the same test right after `setTeam`:
- capture: players of the new team;
- neutralise: players of neither the old team nor team 0.

Other `ControlPoint` fields: `+0x168` and `+0x16c` are the lose and take
timers (from template `+0x1f8` and `+0x1f4`), `+0x170` is the team taking
it, and `+0x17c` is the state (1 lost, 2 losing, 3 getting, 4 controlled).
Template `+0x204` is the minimum number of players, `+0x214` the only team
allowed to take it (0 = any; `gettingControl`, `gotControl`, `losingControl`
and `lostControl` all return early for another team), and `+0x1fc`, `+0x1fe`
and `+0x1ff` are flags. `+0x178` is the "enabled" byte of
`CPEnable`/`CPDisable`.

#### Listing, setting and locking (`world->control_points` etc.)

- **Listing.** Control points are root objects in the ObjectManager grid
  maps. Their template's class id is `CID_ControlPointTemplate` (`0x94a5`,
  `.rodata` `08676ec4`, gamedata `cp_template_cid`). The grid's `IObject*` is
  the `ControlPoint` itself: `handleFrameUpdate` calls IObject slots (`+0x38`
  getAbsolutePosition) on `this`. RMM only accepts an object whose
  `+0x164` equals its object template (`+0x4c`), and refuses anything else
  rather than write to it.
- **Spawns follow the owner.** `gotControl(team)` sets state 4, refills both
  timers, calls `setTeam(team)` and then `CPEnable`. `lostControl` sets state 1,
  calls `setTeam(0)` and then `CPDisable`. `CPEnable` (`0828b150`) gives the
  template's spawn group (`+0x208`, and `+0x20c` only for team 1/2) to the
  point's team, and sets the team of the ObjectSpawners of group `+0x210` and
  activates them. `CPDisable` hands the groups to team 0 and deactivates the
  spawners. Both return early if `+0x178` already has the wanted value, so a
  direct switch from team 1 to team 2 needs `lostControl` first.
  `set_control_point` does exactly that: `lostControl` (when owned), then
  `gotControl(team)`. Around the calls it clears template `+0x200` and
  `+0x214`, then restores them. It also settles the timers and the taking
  team. The `setTeam` hook is muted meanwhile, and one `CONTROL_POINT` with
  no players follows.
- **Locking.** All capture logic lives in `handleFrameUpdate` (vtable slot,
  called every frame per point). It is detoured (`cp_frame_update`) and
  skipped for locked points. The template lock (`+0x200`) can't do this: it
  only stops `setTeam`. With enemies at a fixed point, `lostControl` still
  runs `CPDisable`, and that takes the spawn group away. Locks are per
  object, so they work when several points share one template.
- **Scoring.** The capture score (`handleScore` for each capturer) comes from
  `handleFrameUpdate`'s loop after `gotControl`, so a direct call scores
  nobody.

### Seat

`enterVehicle` logs `pco_id` = `vfunc[+0x2c]` on the interface pointer
returned by `queryInterface(0xc4c5)` (walking up from the entered object).
On the primary `PlayerControlObject` vtable, `+0x2c` is unrelated
(`updateFlags`), so RMM repeats the engine's walk instead of calling a named
getter. Driver = seat 0: seats were seen live as `0→1→2→3→4→0` on a Hanomag
(see "Vehicle entry, exit and seats"), and the Windows live test reports the
driver as seat 0.

### Spawn position and kit

Position is `soldier->getAbsolutePosition()` (`+0x38`). Kit:
`GameServer::changeKit(BFPlayer*, unsigned char kit)` stores the index at
`player+0x80` and resolves its template through a GCC 3
`std::map<int, std::string>` at `GameServer+0x4c` (nodes are
`{color, parent, left, right, key@+0x10, string@+0x14}`). The key is
`team * 256 + kit`: `Game::setKit(int team, int kit, const std::string&)`
(`080656d0`) inserts `team*0x100 + kit`. Verified live on Berlin: `256..260`
= `German_Scout/Assault/AT/Medic/Engineer`, `512..516` = `Rus_*`. (Looking up
the bare kit index never matches.) Team is `player+0x7c`. A second map at
`GameServer+0x40` is team → soldier template (`GermanSoldier`,
`RussianSoldier`), which `spawnPlayer` copies to `player+0x60`.
`BFSoldier::getKitId()` reads `soldier+0x408`. VERIFY live: that `+0x80` is
set for the initial spawn (only `changeKit` was seen writing it); `gdb_kits.sh`
dumps both maps and every player's team and kit.

### Map, round and game status

- `dice::bf::setup` (`_ZN4dice2bf5setupE`, `086c4664`) is the `Setup*`.
  `Setup::getCurrentLevel()` returns `this+0x23c`, a `CampaignEntry`
  `{std::string level; GamePlayMode gpm @+4; std::string mod @+8}`, written by
  `setCurrentLevel`. GamePlayMode values 1-5 are `GPM_CTF`, `GPM_CQ`,
  `GPM_TDM`, `GPM_COOP` and `GPM_OBJECTIVEMODE` (`gpmToString`, `08067880`),
  and `stringToGPM` accepts these names back. RMM maps them itself and must
  not call `gpmToString`:
  - It returns `const std::string&` (a static), not a string by value.
    Calling it with a by-value prototype passed the hidden return pointer as
    the GPM and crashed the server.
  - It treats any other value as a fatal error ("The game type is not
    defined..."), which exits the server.
- `GameServer::setGameStatus` stores the status at `GameServer+0x58`. Status
  `2` triggers `EventLogger::endRound`, so 2 = end game. The rest of the enum
  is not mapped. RMM doesn't need it: it detours the state machine's
  transition functions instead:
  - `gameStatusPreGame(float)` (every pre-game frame) -> `PREGAME`
  - `gameStatusFirstPlaying()` -> `PLAYING` + `ROUND_START`
  - `gameStatusFirstEndGame()` -> `ENDGAME` + `ROUND_END`
- `ROUND_END` reports the winner and victory type from
  `ScoreManager::getWinner()` / `getVictoryType()` (see "Kits, round end,
  score and tickets").
- `MAP_LOAD` fires when the current level, GPM or mod changes. It is checked
  on every status change and every player poll, so the startup map is
  reported too.

### Event-log call sites

`EventLogger::beginEvent(name, player)` (`0805ee80`) has 24 call sites, which
map out where the engine's gameplay events happen:

| Caller | Event | RMM source |
|---|---|---|
| `GameServer::handleChatMessage(ChatMessage const&)` | chat | hook `chat` |
| `GameServer::setTeam(BFPlayer*, int)` | team change | player poll |
| `GameServer::destroyPlayer(BFPlayer*)` | disconnect | hook `destroy_player` (CD-key hash) + player poll |
| `GameServer::changePlayerName(unsigned char, std::string const&)` | rename | player poll |
| `cdKeyAuthorizeCallback(int,int,int,char*,void*)` | CD-key auth | hook `cdkey_auth` (`PLAYER_AUTH` with key hash) |
| `GameServer::handleGameEventManagerEvent(int, GameEvent*)` (×3) | connect/create player etc. | player poll |
| `GameServer::spawnPlayer` / `reSpawnPlayer` | spawn | hook `spawn` |
| `GameServer::enterVehicle` / `exitVehicle` | vehicles | hooks `enter_vehicle` / `exit_vehicle` |
| `GameServer::handleScore(BFPlayer*, ScoreMsg, BFPlayer*, int, int)` | kills/score | hook `score` |
| `GameServer::_giveDamage(...)` | damage | hook `give_damage` |
| `GameServer::handlePickup` | kit pickup | hook `handle_pickup` |
| `GameServer::gameStatusFirstPlaying`, `restartMap` | round start | hook `status_first_playing` |
| `BFSoldier::useMedPack`, `useRepairPack`, `handleUpdate` | medic/repair/death | not hooked |

`beginEvent` only runs when the logger exists (`setup->0x410 != 0`) and its
file is open (`beginEvent` returns early otherwise), that is, only with
`game.serverEventLogging 1`. RMM therefore hooks the `GameServer` functions
directly rather than `beginEvent`/`addEventParam`/`endEvent`: they are
ordinary cdecl functions with `this` first, they run before the engine acts
(so they can block), and they don't depend on event logging. The
`EventLogger` hooks would be a single generic source with named params, but
observe-only.

## Damage and kills

### Damage (`GameServer::_giveDamage`, `081528e0`)

`void _giveDamage(IObject* target, float damage, int attackerId, int, int, Pos3, int, bool, bool, bool)`.
The public `giveDamage` resolves `attackerId` (when `-1`), applies armour via
`calcDamage`, and forwards here. `gameStatusPlaying` also calls it directly,
so every damage path passes through, and `damage` is the final value.
`playerManager->getPlayerFromId(attackerId)` confirms the attacker
parameter. `Pos3` is three floats by value (`giveDamage` forwards its own
three floats). RMM identifies the victim by matching `target` against each
player's soldier (`+0x68`), and fires `PLAYER_DAMAGE` before the original:
blockable (skip the call) and mutable (`*damage`).

Arguments, from the `Armor` setters it calls (slots read from
`vtable for Armor`) and the four `handleScore` calls it makes (via
`GameServer` vtable `+0x68`; kills are scored inside `_giveDamage`):

| Arg | Meaning | Evidence |
|---|---|---|
| `target` | object hit | |
| `damage` | final damage | after `calcDamage` in `giveDamage` |
| `attackerId` | player id | `Armor::setLastHitPlayer` (`+0x74`), `getPlayerFromId` |
| 4th int | material | passed to `calcDamage` in `giveDamage` |
| 5th int | weapon template id | `Armor::setLastHitDamageType` (`+0x84`) and `handleScore(…, Kill/TK, victim, weaponId)` |
| `Pos3` | hit position | `Armor::setLastHitPlace` (`+0x6c`), relative to the root |
| last bool | can TK | `Armor::setLastHitCanTK` (`+0x7c`) |

Damage events therefore get the same weapon names as kill events
(`WeaponName(id)`).

### Kill weapon names

Which template the "weapon template id" names: in `FireArms::fireBarrel`
(`0x08291c10`), when the shooter is a player and the new object is a
Projectile (class `0x9495`), the projectile's fields are set as follows:

| Field | Set to |
|---|---|
| `+0x108` | the player id (`IPlayer +0x48`) |
| `+0x12c` | `IPlayer +0x7c` |
| `+0x134..+0x13c` | the muzzle position |
| `+0x140` | the template id (template vtable `+0x1c`) of what the player controls: the FireArms' own template (`this +0x4c`) if that is the soldier (class `0x9493`), otherwise `getRootParent()` of the controlled object |

`GameServer::handleCollisionForProjectile` (`0x0815ac10`) passes `+0x140` as
the damage type to the armour (vtable `+0x8c`) and to `handleExplosion`,
which leads to `_giveDamage`'s 5th int. As a result:
- On foot, the reported name is the hand weapon (`Bar1918`, `KnifeAxis`,
  `GrenadeAllies`), never the shared projectile (`barProjectile`).
- In a vehicle or at a stationary gun, it is the root vehicle (`T34` for both
  its cannon and coax MG).

VERIFY live: grenades, mines and explosion packs are thrown or placed rather
than fired by a player, so check what they report, along with roadkills.
Kills by bots report no weapon name: the engine itself passes weapon id -1
(seen on the Windows server, see `bf1942-windows.md`).
`docs/BF1942_WEAPONS.md` lists every name from `objects.rfa`.

### Vehicle destruction

`_giveDamage` finds the target's `IArmor` and its `IPlayerControlObject` by
walking up composite parents (`queryInterface(0xc378)`, parent at
interface `+0x50`; `queryComponent(0xc4a4, 0xc4a4)` for armour,
`queryInterface(0xc4c5)` for the control object). It logs `destroyVehicle`
when `Armor::getHitPoints()` (`+0x1c`) − damage ≤ 0.001, and only for
targets with a control object. `Armor` vtable: `+0x14` getMaxHitPoints,
`+0x18` setHitPoints, `+0x1c` getHitPoints, `+0xc8` isDestroyed. RMM does
the same walk. It treats a target as destroyed when `isDestroyed()` is true
or hit points are at or below the engine's 0.001 threshold. When that goes
false → true across the call, RMM fires:
- `VEHICLE_DESTROYED` (attacker, object, template name) for targets with a
  control object;
- `OBJECT_DESTROYED` (object, template, attacker, weapon) for other
  armoured objects such as bridges. This is checked only while a plugin
  hooks the event, because it needs the armour walk on every non-soldier
  hit. VERIFY live: which static objects on stock maps are armoured.

Soldiers are player-control objects with armour as well. Some hits on a
soldier don't match any player's `+0x68`: for example a player killed by
their exploding vehicle, or one who is disconnecting. In the first live test
these were reported as `VEHICLE_DESTROYED RussianSoldier`. RMM skips any
target whose template class is a soldier. It reads the class with
`ObjectTemplate::getClassID()` (vtable `+0xc`) and compares it with
`CID_BFSoldierTemplate` (`08676e88`, a `.rodata` constant `0x9493`), the
same test the engine uses in `changeKit` and before `checkForHitOnSoldier`.

Object vtable slots, checked on `Object`, `BFSoldier` and
`PlayerControlObject`: `+0x08` `queryInterface(unsigned)`, `+0x28`
`queryComponent(unsigned, unsigned)`, `+0x38` `getAbsolutePosition()` (→
`const Pos3&`).

### Kill (`GameServer::killPlayer(int)`, `08154950`)

`playerManager->vfunc[+0x18](id)` (getPlayerFromId), then
`killPlayer(this, player, false)`. It returns 1 if the player exists, else 0.
`killPlayer(BFPlayer*, bool)` returns early if `player+0x79` (alive) is
clear, then clears it and scores `handleScore(…, 5 = DeathNoMsg, 0, -1, -1)`,
so RMM's score hook reports the death with attacker none. RMM calls it
directly (`kill_player`), so `RMM_CAP_KILL_PLAYER` doesn't depend on a
console command.

### Direct damage (`players->damage`)

`GameServer::gameStatusPlaying` calls `_giveDamage` directly with
`(target, damage, attacker, material, weapon, x, y, z, -1, 1, 1, 1)`
(decompiled pushes, `damage.c` ~line 1069). RMM uses the same pattern on
the victim's soldier: material 0 and weapon -1. It calls the original
(trampoline), so no RMM events fire. Attacker -1 is safe: `_giveDamage`
null-checks `getPlayerFromId(attacker)` before scoring (`damage.c` line
113). Calls made while a `_giveDamage` hook is running are queued to the
end of the frame instead of re-entering the engine. Health is read from
the soldier's `Armor`: `getHitPoints` is `+0x1c` and `getMaxHitPoints` is
`+0x14`, both returning float in `st(0)`. VERIFY live that a lethal hit with
weapon -1 produces a normal death.

## World objects

### ObjectManager (`dice::ref2::world::objectManager`, `086cb724`)

The global is an `ObjectManager*`. Its constructor writes only one vptr
(`$0x86cdf08` = vtable `086cdf00` + 8), so `IObjectManager*` and the
concrete pointer are the same. It holds two GCC 3
`std::map<unsigned, IObject*>`, keyed by object id = `IObject+0x48`:

| Offset | Map | Evidence |
|---|---|---|
| `+0x94` | every registered object | `registerObject` (`081a2510`): `lower_bound` on `this+0x94` with key `obj+0x48`, value stored at node `+0x14`; `getAllRegisteredObjects` returns `this+0x94` |
| `+0x11c` | player-control objects (vehicles and seats) | `addPco` (`081aa4c0`): `insert_unique` into `this+0x11c` with `{obj+0x48, obj}`; `getPcoMap` returns `this+0x11c` |

The map member is a pointer to the heap header node, as with the kit
map. RMM enumerates vehicles from the PCO map. Object handles are
`(id << 32) | pointer`, and a handle is valid only while the map entry
for `id` still holds that pointer, so a freed and reused address never
validates.

The registered map is not the full object list; see "Enumerating objects"
below for the grid maps.

### Roots

`getRootParent(const ICompositeObject*)` (`08194520`): if the bit
`0x02` at `+0x7` (flags dword `+4`) is set, the object is its own root;
otherwise the result is cached at `+0xd4` and computed by recursing on
`+0x50` (the parent, used directly as an `ICompositeObject*`). `_giveDamage`
calls it, then `getAbsolutePosition` (`+0x38`) on the result, so the root
is a full object. `IID_IObject == IID_ICompositeObject == 0xc378`. RMM
canonicalises every object through `queryInterface(0xc378)` before
comparing or making handles. VERIFY live that this returns the object
pointer itself (single inheritance in `BObject`/`BCompositeObject`).

### Position and physics

- `setAbsolutePosition(const Pos3&)` is object vtable `+0x3c` on `Object`,
  `CompositeObject`, `PlayerObject` and `PlayerControlObject` (read from the
  vtables). On a root, `BCompositeObject<IPlayerObject>::setAbsolutePosition`
  (`0816b4a0`) writes the translation row of the transform at `+0x74`
  (position `+0xa4`) and calls `updateFlags(0x500020, 0)`.
  `PhysicsNode::setAbsolutePosition` (`0825ba20`) checks that the values
  are finite and forwards to that same object function (`node+0xc` =
  owner), so RMM calls the object function directly.
- The physics component is attached by
  `SimpleObjectTemplate::setPhysicsNodeComponent` with
  `obj->setComponent(IID_IPhysicsNode = 0xc422, node)` (`+0x24`), so RMM
  reads it with `queryComponent(0xc422, 0xc422)` (`+0x28`), the same way
  it reads Armor.
- `IPhysicsNode` slots, identical on `PhysicsNode`, `PointPhysicsNode` and
  `StaticPhysicsNode` (and on the PhysicsNode subclasses `PhysicsEngine`,
  `PhysicsFloatingBundle`, `PhysicsSpring`, `PhysicsWing`):
  `+0x30` setAbsolutePosition, `+0x38` getPositionalSpeed, `+0x3c`
  getRotationalSpeed, `+0x40` setPositionalSpeed, `+0x44`
  setRotationalSpeed, `+0xd0` setIsSleeping, `+0xd4` setIsAwake.
  `PhysicsNode::setPositionalSpeed` stores to `node+0x10..0x18`, and
  `setRotationalSpeed` stores to `+0x1c..0x24`.
- `PhysicsNode::getRotationalSpeed` returns a static zero vector when
  the owner has a parent (`node+0xc → +0x50`): children move with their
  root, so RMM always sets velocity on the root.
- RMM touches a component only if its vtable slot `+0x40` is one of the
  three resolved `setPositionalSpeed` implementations.
- VERIFY live: that zeroing a floating root's speeds every second holds an
  empty ship in place (buoyancy and engine forces still apply on each
  physics tick), and that clients follow `setAbsolutePosition` jumps.
- Rotation through the transform: it is a `Mat4` at `+0x74` with rows
  right/up/forward/position. RMM sets rotation with `setRotation` instead
  (see "Find, spawn, remove, ground").

### Soldier knock-back (`players->push`)

- `BFSoldier::BFSoldier` (`08272470`) allocates a `PointPhysicsNode`
  (`0x50` bytes) and attaches it with `setComponent(0xc422, node)`, so
  `queryComponent(0xc422)` on the soldier returns a node whose `+0x40`
  slot is `PointPhysicsNode::setPositionalSpeed` (`0825d900`, stores
  `+0x10..0x18`). The velocity check above accepts it.
- `BFSoldier::handleUpdate` (`08278ea0`) only reads the node's speed
  (`+0x38`). Index 1 is vertical, which is checked when a parachutist lands
  (`|vy| < 2`). Within 0.2 s of `triggerFallingAnimation` a speed over
  8 m/s (`|v|² > 64`) plays `Lb_Explosion*` animations, so the engine
  knocks soldiers back with speed too.
- `disablePhysicsOnSoldier` / `reenablePhysicsOnSoldier` (`08323d80`,
  `08323e30`) toggle the object at `+0x60` (slot `+0xb8`) and component
  `0xc42c` (`+0x30`/`+0x34`), and do the same for every child.
- RMM adds the push to the current speed and then calls `setIsAwake`.
  Clients follow it (verified live with `!slap`).
- In a vehicle, the soldier isn't what physics moves, so RMM pushes the
  vehicle's root `PhysicsNode` instead: the same component and validation as
  `SetObjectVelocity`. The whole vehicle goes up, with every occupant. The
  speed is set directly, so the launch doesn't depend on the vehicle's mass.
  VERIFY live: tanks and jeeps (and that `StaticPhysicsNode` roots such as
  fixed MGs ignore it).

### Runtime object creation

From the Ghidra decompilation (`br_feas*.c`).

- `GameServer::spawnObject(IObjectTemplate const*, Pos3, Vec3 rot)`
  (`081394b0`) is a static `__cdecl` wrapper for
  `createObjectOnAllClients(tmpl, pos, rot, true, 0)` (`0813aa80`).
- `createObjectOnAllClients`:
  1. For templates with interface `0xc4c4` (type 0, or type 2 with
     `+0x88 != 0x24`), it refuses (returns NULL) if a living soldier
     (class `0x9493`) is within 2 m. This is the vehicle-spawn guard.
  2. It creates the object with `tmpl->vtable+0x2c`. A non-zero pos/rot
     is applied with `obj+0x3c` / `setTransformation`.
  3. Only if the object has a networkable (`obj+0x68 != 0`) does it
     register it with `networkManager` (`+0x14`, or `+0x18` with a fixed
     ghost id = last arg). It then queues a `CreateObjectEvent` (template id
     from `tmpl->vtable+0x1c`, ghost id, pos, rot) to every client
     connection (all of them when the bool is false, otherwise only those
     with `conn+0x14 -> +8` set). For kits (class `0x9496`), it also calls
     `obj->vtable+0x118`.
  4. `objectManager->vtable+0x5c(obj)` adds it to the world.

  Only networkable templates show up on clients. A plain effect object (no
  `networkableInfo`) exists only on the server.
- Kits use this path. `GameServer::changeKit` and `spawnPlayer` create
  the kit with `createObjectOnAllClients(kitTmpl, 0,0,0, 0,0,0, 1, 0)`
  ("Unable to create the requested kit!"). The soldier holds the kit's
  object id at `+0x408`. `BFSoldier::dropKit` (`08280900`) does not
  create a new object. It detaches the kit parts from the soldier, snaps the
  existing kit object to the ground (`terrainBase->vtable+0x54(x, z, &normal)`
  for terrain height, plus an `objectManager+0x48` ray test for anything
  standing on it), sets its transform, and calls kit interface
  `0xc49c` `+0x24` and `+0x14(-1)` (probably clear the owner).
- Pickup: `findKitObject(Pos3 const&, float r)` (`081517e0`) does an
  `objectManager+0x30` radius query with `ObjectFlagPredicator(0x400)` and
  takes the nearest object with class `0x9496`. `Game::handlePickup` forwards
  to kit interface `0xc49c` `+0xc(player, soldier)`.
- Client side: `CreateObjectEvent::eventReceivedByRemote` calls
  `game->queryInterface(0x1d4c3)->+0x38(templateId, ghostId)`. The client
  creates its own template by id, so the template must exist on the client
  (vanilla/mod templates only, no server-only templates).

VERIFY live: a kit made with `spawnObject(kitTmpl, groundPos, 0)` and
then given the `0xc49c +0x24 / +0x14(-1)` "dropped" treatment can be picked
up, and despawns (or not) like a dropped kit. Does a server-only
`networkableInfo` on an effect template make clients create the effect from
their own template? Risky, because the client may expect a ghost.

### Find, spawn, remove, ground

Live-tested on Berlin.

- Enumerating objects: `ObjectManager::addObjectToGrid` files every
  root object (flag `0x2000000` at `IObject+4`) in an id-keyed
  `std::map<unsigned, IObject*>`: `om+0x48` (`getAllObjectsFromGrid`,
  `getObjectFromGrid(id)` = vtable `+0x20`), or `om+0x60` when flag `0x200`
  is set (`getObjectFromNoCollisionGrid(id)` = `+0x24`). RMM reads both maps
  directly (no engine calls) for `find_objects`, and looks handles up in
  them to validate non-vehicle objects. Berlin has 380 roots in 84
  templates: buildings (`citymesh*`, `rusruin*`, `Berlin_Galler_M1`),
  props, `Ammobox`, `mediclocker`, spawners, control points, spawn points,
  `TSun`, `Coastline`, `ObjectManagerCamera`. The registered map
  (`om+0x94`) is not the full list. `ObjectFlagPredicator::predicate` is
  `(obj+4 & flags) == flags`.
- Template by name: `ObjectTemplateManager::getTemplate(const std::string&) const`
  (`081dcf80`), a case-insensitive hash lookup. It writes a terminator into
  the argument's buffer, so pass an engine string RMM owns.
- Spawn: `createObjectOnAllClients(gs, tmpl, x,y,z, yaw,pitch,roll, 1, 0)`,
  which is what `GameServer::spawnObject` (vtable `+0x50`) does. Verified: a
  `German_Assault` kit and a `T34` were created, both networked, and appear
  in the grid map at once. `ObjectSpawner::spawnObject` (`0831b110`) uses
  the same call through `game+0x50`, then calls these on the vehicle's
  `IPlayerControlObject` (`queryInterface(0xc4c5)`, whose address point is
  `+0x1b0` in the PlayerControlObject vtable):
  `+0x84` setObjectSpawnerId, `+0x80` setTimeToLiveUnused(float),
  `+0x6c` setTeam, and `+0x8c` setObjectSpawnerHolding(1) if the template
  says so. The constructor sets TTL = -1.0, which `handleFrameUpdate`
  treats as "never expire" (`831fdf2`). Spawner id = -1, team 0. RMM leaves
  all of these at their defaults.
- Vehicles killed at spawn: a T34 spawned at 1780,1950 (crates and
  planks nearby) was destroyed on its first physics frame, at any height
  and rotation. gdb showed
  `ResponsePhysics::checkObjectVsObject -> Spring::handleCollision ->
  SimpleObject::handleCollision -> giveDamage(1e10)` on the wheels. The same
  call 40 m away (1805,1910) took only 2.19 landing damage and the tank
  stayed. Pick clear spots. `gdb_spawn_damage.sh` backtraces every
  `_giveDamage` around a console command.
- Remove: `GameServer::destroyObject(IObject*)` (`08138a90`, vtable
  `+0x54`) kills players in it whose `+0x6c == -1`, sends a
  `DestroyObjectEvent` to each client, removes the ghost, then calls
  `obj+0x18` (destroy, deferred: `ObjectManager::destroyObject` queues it at
  `om+0xb8`). It returns false if `obj+0x20` is already true. Verified on a
  spawned kit and on a map T34; the spawner later respawned the map T34.
- Terrain: `geom::terrainBase` (`086f10f0`) is a `PatchTerrain` once a
  map is loaded. `getHeight(float x, float z)` is vtable `+0x44` and
  `getHeightAndNormal` is `+0x54` (`dropKit` uses it). Verified: terrain
  under a crate at y 39.9 reads 39.70, and under a sandbag at 38.2 reads 38.25.
- Rotation: `setRotation<float>(Mat4&, const Vec3&)` resets the 3x3
  part and applies `yaw`, `pitch`, `roll` through `rotateAboutLine`, which
  is `rotateZDeg`, so the angles are degrees. The translation row is
  untouched. Then `world::setTransformation(IObject*, const Mat4&)`. The
  matrix comes from `IObject` vtable `+0x40` (16 floats). The call returns
  without error live; VERIFY the client-side effect.
- Teleport (BF1942 has no console command for it): `setAbsolutePosition`
  (`+0x3c`) on the soldier, or on the root of their vehicle, then zero the
  physics speed. VERIFY with a client.

## AI bots

Live-tested on co-op Berlin. RMM drives bots through the engine's own AI
console (gamedata `bot_*` templates), so the same data works for BFV and the
Windows server.

- Finding the commands: console methods are static `ConsoleObject`
  records built in `__static_initialization_and_destruction_0` (`084a90c0`,
  too big for Ghidra's decompiler): `+0xc` method name, `+0x14` argument
  count, `+0x4c` return type name, `+0x50..` argument type names.
  `conobjs.py` rebuilds them from the disassembly into `ai_conobjs.txt`
  (372 AI methods). Object names were found by trial: `ai.*` (AIConsole and
  the bot queries), `aiSettings.*`, `aiStrategy.*`.
- Only in co-op. In other modes, `ai.getBotCount` prints "Unknown object
  or method", but `ai.getAllBots` segfaults the server. RMM's
  `BotsAvailable()` sends `bot_count` first, and sends nothing else unless
  that printed a number.
- Ids: `ai.getAllBots` prints `<bot id> <name>` (bot ids 0..n-1). Bot
  players are numbered from 255 down (`game.listPlayers`: "is an AI bot").
  RMM matches the two by name.
- Queries (all take the bot id): `getBotPosition` / `getBotGoalPosition`
  return `x/y/z`, `getBotSide` returns the team, `getBotKit` returns
  `KTAssault` etc., and `getBotVehicle` returns `NoVehicle` or a template.
  `getActiveBehaviour` returns an index into `getBotBehaviourList` (`0
  Avoid`, `1 MoveTo`, `2 Idle`, `3 Fire`, `4 Scout`, `5 TakeCover`, `6
  Change`, `7 Special`). `getActiveStrategy <team>` and `listSAIStrategies
  <team>` are per team.
- Orders: `ai.botMoveTo <radius> x/y/z <bot>`
  (`AIConsole::botMoveTo(float, Pos3, int)` builds a `WPMoveTo` waypoint)
  sets the goal at once. The strategic AI replaces it within a few seconds.
  `ai.botSetWaypoints_Patrol <bot> <radius> <count> <p1> <p2>` builds a
  `WPPatrol`. `ai.botGiveOrder <bot> <behaviour> <modifier set>` looks up
  the behaviour id and the modifier set at `AIConsole+0x10`, and does
  nothing if either is unknown.
- Strategy and skill: `aiStrategy.forceStrategy <name> <team>` takes
  effect at the next SAI update (about 1 s). `aiStrategy.listStrategies`
  lists every defined strategy, but RMM only accepts the team's own list.
  `aiSettings.get/setBotSkill` (0..1) is live; `game.serverCoopAiSkill` (%)
  only applies at round start.
- Don't toggle `ai.saiEnable`: setting it to 1 after 0 at runtime
  segfaulted the server.

### Adding

The engine's recipe, from `ai.rfa` (`ai/spawnSoldier.con`, extracted with
`rfa_extract.py`): `game.createAIPlayer <name> <team> <kit> [x/y/z]`, then
`ai.addBot <name> <team> default StandardWeights NoEquipment`. `addBot`
(`08473540`) finds the player by name, but only from the next frame on
(it prints -1 before that). It returns `BotManager count - 1`, the new bot
id. `BotManager::addBot` refuses past `IAISettings::getMaxNBots`, and
reuses a deactivated `BotMain` from its pool (`+0x1c`) while appending a
new id.

Hazard: after a bot had been removed/killed and a new one was added
(id 32 on a full 32-bot server), the server first segfaulted, and in a
second run spun forever in `SAI::distributeResources ->
SAI::BotInfo::assign -> std::set<int>::insert_unique` (a corrupt tree).
The shipped gamedata therefore has `[capabilities] bot_add = 0`.

### Removing

Live-tested with 1, 3 and all 32 bots, then `admin.restartMap`. No console
command removes a bot. `ai.killBotId` only kills it (`game` vtable `+0xb4`),
and `admin.kickPlayer` ignores bots. The engine's own teardown is
`~BotMain -> AIPlayer::removePlayerFromGame` (`085e3d80`):
`game->exitVehicle(player, false)` (`+0x70`),
`game->destroyObject(player+0x74)` (`+0x54`; `+0x74` is the default camera,
set by `BFPlayer::setDefaultCamera`), then `queryInterface(0x1d4c1)`
(= GameServer+0xf8) `+0x18c` = `GameServer::destroyPlayer`. RMM first
detaches the AI with `ai.deactivateBot <player id>`
(`AIMain::deactivateBot(int)` -> `BotManager::deactivateBotFromPlayerId`,
which unhooks the bot from its team's SAI and clears the pid table), then
runs those three calls.

- Only dead bots can be detached. BotManager keeps bot ids in
  std::lists: `+0x30` dead (actionDeadBots moves the ones that are no
  longer dead to `+0x2c`), `+0x2c` alive, `+0x34` deactivated pool;
  `+0x38` is a `vector<int>` player id -> bot id (-1 = none). Only bots on
  `+0x30`, or with `BotMain::isDead()` (vtable `+0xbc`), are detached, so
  `ai.deactivateBot` on a living bot prints `0`. RMM kills living bots
  first and finishes when they're dead (`[bots] remove_wait`).
- Co-op refills at once. `GameServer::gameStatusPreGame / FirstPlaying
  / Playing` loop while players (`GameServer+0x18`, decremented by
  destroyPlayer) < max players (`+0x1c`, `setMaxNrOfPlayers`) and
  `getMaxNBots != BotManager::getBotCount`. Each pass creates an AI player
  (`game` vtable `+0x13c`) and calls `IAIMain::activateBot` ->
  `BotManager::reactivateBot`, which reuses a detached brain. RMM keeps
  removed slots empty by raising `+0x18` by the number removed, only while
  those functions run (gamedata `status_playing` hooks `gameStatusPlaying`
  for this). `+0x18` is read only by that loop there; `+0x1c` also drives
  ticket bleed, so it stays. The count resets at pre-game.
- `ai.getAllBots` segfaults while a brain is detached:
  `SmartFunctionContainer::getAllBots -> AIPlayer::getName()` on a NULL
  player. RMM's bot list on Linux is native (player list + `+0x38`).
- Don't change `aiSettings.setMaxNBots` at runtime. It's a plain field
  (`AISettings+8`), but `SAI::distributeResources`, `AIRadio::update` and
  others size per-bot data by it. Setting 31 with 32 bots active
  segfaulted at once.
- `IAIMain` (what `ai::IAIMain::instance` points at, AIMain+4) vtable:
  `+0x8` addNewPlayer, `+0xc` popPlayer, `+0x10` activateBot, `+0x14`
  deactivateBot(int&, botId), `+0x18` deactivateBot(playerId), `+0x1c`
  isInitialised, `+0x24` tick, `+0x28` action. BotManager vtable: `+0x8`
  getBotCount, `+0xc` getNumberOfActiveBots, `+0x18` getBotFromId,
  `+0x1c` getBotFromPlayerId. BotMain: `+0xac` getPlayerId, `+0xb4`
  getPlayer (AIPlayer*), `+0xbc` isDead, `+0xcc` setAsDeactivated,
  `+0xe0` getBotId.

### Input

A bot plays through the same input record as a client.
`BotMain::updatePlayerAction` -> `AIPlayer::addInput(PlayerInput*)`
(`085e3f80`, AIPlayer+0 = BFPlayer*) -> `PlayerAction::set` ->
`ActionBuffer::pushBack(BFPlayer+0x144)`. `GameServer::simulatePlayerUpdate`
pops one action per player update (it repeats the last one when the queue
is empty). `PlayerInput` = `Input<PlayerInputMap, 55>`: `float value[55]`,
then a 64-bit "mapped" mask at `+0xdc`; `PlayerAction::set` reads a value
only if its bit is set. Axes 0..5 go to 12.4 fixed point (+-16); buttons
count above 0.5. Slot numbers come from the `addConstantHelper("c_PI...",
n)` calls in `io::Module::init` (`c_PIYaw` 0 ... `c_PIFire` 8, `c_PIUse`
10, `c_PIAltFire` 23, `c_PILie` 28, `c_PICrouch` 29 ... `c_PIVoteNo` 54).

RMM hooks addInput: it records the AI's record (`bots->get_input`) and,
for controlled bots, passes a modified copy. Live: about 35 ticks/s per bot
(every frame); exclusive with no input freezes the bot in place, and
`throttle=1` moves it. What each axis means for soldiers (sign, speed,
look) still needs a client to watch (VERIFY).

## Kits, round end, score and tickets

From the Ghidra decompilation (`gg1.c`, `gg2.c`). Live-tested on Berlin with
one client: `set_kit` works for any kit template, whatever the map's teams
are (Rus_AT, US_AT, US_Assault, Jap_Assault). `gdb_kit_templates.sh` lists
every kit template the server knows: 40 faction kits in 8 sets, plus base
templates such as `GerKit`. `end_round 2` ran without errors. Blocking kit
pickups and vehicle entry works with a client. Not yet checked with a
client: the end screen, and score on the scoreboard.

### Kit hand-out

`GameServer::changeKit(BFPlayer*, uchar kit)` (`0815f770`) is a live kit
swap. It only acts when `player->getVehicle()` (vtable `+0x3c`) has template
class `0x9493` (on foot). It looks up `team*256 + kit` in the kit map, gets
the template through `objectTemplateManager` `+0x1c`, and calls
`createObjectOnAllClients(tmpl, 0,0,0, 0,0,0, 1, 0)`. It reads the old kit:
soldier vtable `+0x17c` = `BFSoldier::getKitId()` = `soldier+0x408`, looked
up with `objectManager` `+0x20` (`getObjectFromGrid`), then `+0x24`. Then
`this->vtable+0x40(kit->queryInterface(0xc378), player, soldier, true)`,
which is `GameServer::handlePickup`, then `player+0x80 = kit`,
`destroyObject(old)` (vtable `+0x54`), and
`BFSoldier::selectBestLoadedWeapon(player+0x68)`. `spawnPlayer` hands out the
spawn-menu kit with the same create + `handlePickup` pair. `Game::changeKit`
is a stub ("TBM: Not in use!"). RMM's `set_kit` repeats changeKit with any
kit template (class `CID_KitTemplate` = `0x9496`, read from `.rodata`).

### Kit swaps on living bots (crash, fixed)

Gun game levels bots up mid-life, and two crashes were caught with
`gdb_attach_crash.sh`:
1. SIGSEGV at eip 0 in `BAPAChangeWeapon::execute` (`085456b0`, +0x28d:
   `call *0x68(%edx)` on `this+0x1c`). The action's constructor caches the
   target item from the soldier's AI info (Bot vfunc `+0xd0` = info handle,
   resolved through `ai::informationManager`: entry at `table + (h&0xffff)*8`,
   object at `-8`, u16 serial at `-2`; info `+0x34` = soldier interface, vfunc
   `+0x1c(slot)` = item, `+0x14` = held slot; item `+0xc` = `ai::WeaponTemplate`,
   `+0x40` = its input id). The old kit's items are freed by the swap.
2. `__cxa_pure_virtual` in `BAPConWeaponMagAmmo::evaluate`: the plan's
   `BAPVariable<ai::Weapon*>` still pointed at a freed `ai::Weapon` wrapper
   (refcounted wrappers around FireArms, `ai::Weapon::setObject`).

The whole bot plan holds stale weapon pointers, not one action. RMM's fix:
`set_kit` on a bot is always deferred to RMM's frame (outside
`AIMain::action`), and right after the swap RMM calls `BotMain::reset()`
(`08533210`, BotMain vtable `+0x2c`; the bot comes from
`BotManager::getBotFromPlayerId`, `084a34d0`, which doesn't range-check the
pid table). reset deletes the plan (`+0x74`, virtual delete), flags a replan
(`+0x14`), and clears behaviours, pathfinding, vision memory and fire objects.
As a second guard, `BAPAChangeWeapon::execute` is detoured to re-resolve
`this+0x1c` every call (ending the action if the slot is now empty).
Verified: 32 bots in gun game ran 10+ minutes without a crash (before the
fix: a crash within 2 minutes), and bots kept fighting after their resets.

### Pickup

`GameServer::handlePickup(IPlayerObject* item, IPlayer* player,
IPlayerObject* soldier, bool)` (`081518c0`) logs `pickupFlag` (class `0x949a`)
or `pickupKit` (`0x9496`) and sends a pickup event to every client. It then
calls the item's IPickup (`0xc49c`) `+0xc`. For kits that is
`Kit::handlePickup` → soldier vtable `+0x174` = `BFSoldier::pickupKit`.
pickupKit drops the current kit first (`+0x178` = `dropKit`) if
`+0x408 != -1`, attaches the kit parts, sets `+0x408 = kit+0x48` (the object
id) and `+0x240` = the kit's slot, and selects a weapon. RMM's hook raises
KIT_PICKUP. Blocking it means not calling the original, so nothing reaches
the clients. The pickup-key path is `checkPlayerTriggers` (see "Vehicle
entry, exit and seats"); the walk-over caller is a virtual call not found
yet. Blocking was tested live and the client doesn't predict the pickup.

### Vehicle entry

`GameServer::enterVehicle` (`081558d0`) returns 0 without side effects when
the object has no IPlayerControlObject. RMM's blocked entry returns 0 before
the original runs.

### Score

`ScoreManager::scoreEvent` adds the per-type points to `BFPlayer+0xc0` and
`+0x114` (and team totals). `BFPlayer::updateStateMask` diffs `+0xc0` against
the last baseline (bit `0x800`, sent as a signed 16-bit value by
`getNetUpdate`), so writing the field replicates. Kills `+0xa4` and deaths
`+0xa0` replicate the same way (0..255), as does the kit slot `+0x80`.

### Round end

`handleScore`, when a team reaches the score limit:
`scoreManager->setWinner(team)` (`+0x54`), `setVictoryType(1)` (`+0x5c`), then
`GameServer::setGameStatus(2)` (vtable `+0x78`; 2 = EndGame). This logs
`endRound` and sends a GameStatus event. `gameStatusPlaying` does the same for
tickets and time (winner 0 with victory type 0 = draw; Conquest grades 1..3
by the ticket gap). RMM's `end_round` replays the score-limit sequence from
the frame hook. `ROUND_END` reads `getWinner()` / `getVictoryType()`.

### Tickets

A team's current tickets are `scoreManager->getTeamScore(team)` (`+0x10`)
`+0x48`, which is what gameStatusPlaying tests.
`GameServer::setCurrentNumberOfTickets(team, n)` → `TeamScore::setTickets`.
`Game::getNumberOfTickets(team)` (`+0xc0 + team*4`) is the round's starting
count, not the current one.

## Vehicle entry, exit and seats

From the Ghidra decompilation (`veh1.c`, `veh2.c`). Live-tested on Berlin with
one client:
- `rmm putin` (plugin) works.
- The use key and F-key switches were blocked as `player` / `switch` with no
  client glitch reported. A blocked switch leaves the player where they were.
- `rmm eject` works while exits are blocked.
- Unblocked F1–F5 in a Hanomag gives exit+enter pairs with seats
  0→1→2→3→4→0, and the seat numbers match `rmm objects`.
- A held key retries up to ~15 times a second.
- Suiciding in a PanzerIV while exits were blocked: the exit went through
  with reason `game`. Afterwards BFPlayer+0x4c is the death camera
  (`MultiPlayerFreeCamera`), so `get_info` only reports a vehicle for living
  players.
- A natural ticket win reported `ROUND_END winner 1 reason 3`
  (getWinner/getVictoryType).

Findings:

- `GameServer::checkPlayerTriggers(BFPlayer*, PlayerInput)` (`08156330`)
  handles a player's (or bot's) input each tick. It takes the PlayerInput
  by value (0xec bytes, the same size as `playerinput_size`; the mask
  sits at `+0xdc`).
  - Use-key bit (`mask >> 10`): calls `toggleEntryPoint(player, false)`.
  - Kit-pickup bit: `findKitObject(pos, 1.1)` → `handlePickup` (vtable
    `+0x40`). The live KIT_PICKUP block went through this path.
  - F1–F8 bits 14..22 while in a vehicle: finds the root's
    IPlayerControlObject. If the current position (`pco +0x2c`) isn't the
    target, and the target seat is in the root's position map (`+0x40`) and
    free (seat `+0x14` == 0), it calls `exitVehicle(player, false)` (vtable
    `+0x70`) and then `enterVehicle(player, seat->queryInterface(0xc378),
    false, false)` (vtable `+0x6c`). It doesn't check the exit's result,
    so RMM swallows that enter when it blocks the exit.
- `GameServer::toggleEntryPoint(BFPlayer*, bool)` (`08155ee0`):
  - In a vehicle (not the soldier, class `0xc4c2`): if
    `PlayerControlObject::isAllowedToExit()`, it calls `exitVehicle`. If that
    returns 0 it returns at once; otherwise it stamps `player+0x98`, a
    1-second debounce that only applies after a success.
  - On foot: `BFfindEntryPoint(pos, 3.0, team, ...)`, the team check
    (`pco +0x74` == player team or 0), then `enterVehicle`.
  - Also called from `spawnPlayer` and `reSpawnPlayer`, for spawn points in
    a vehicle.
- `GameServer::exitVehicle` (`08155330`) returns 0 with no side effects
  when no player-control object is found. Every other caller (`reSpawnPlayer`,
  `exitAndPlaceCameraIfApplicable`, deaths, RMM's bot removal) is the game's
  own exit.
- RMM tags each enter/exit with a reason through these two hooks: PLAYER
  inside `toggleEntryPoint` from the input handler, SWITCH elsewhere in the
  input handler, SPAWN for `toggleEntryPoint` outside it, PLUGIN for RMM's
  own calls, and GAME otherwise. `enter_vehicle` repeats the F1–F8 sequence
  with any seat of the PCO map under the vehicle's root. Seat numbers are
  the `+0x2c` position ids.

## Weapon switching

From the decompiled `BFSoldier::enableItem`, `enableItemForceNetwork`,
`disableItem`, `removeItem` and `handleMessage`, and
`BFSoldierNetworkable::updateStateMask`, `getNetUpdate` and `setNetUpdate`.
Slots are from `vtslot.py` on `vtable for BFSoldier` (`086dcb40`).

Soldier fields:
- `+0x3ac`: `int*`, one item object id per slot (-1 = empty).
- `+0x3b8`: the held slot (-1 = none).
- The template at `+0x4c` has `+0x2d4`, the slot count (enableItem's range check).
- An item's slot is its template's `itemIndex`: 1 knife, 2 pistol, 3 main
  weapon, 4 grenade or explosive, 5 and up for tools (`docs/BF1942_WEAPONS.md`).

Vtable slots (address point):

| Slot | Function | Address |
|---|---|---|
| `+0xec` | `getItem(int)` | `0827fba0` |
| `+0xf0` | `removeItem(int)` | `0827fcd0` |
| `+0x100` | `enableItem(char)` | `0827f4d0` |
| `+0x104` | `enableItemForceNetwork(char)` | `0827f180` |
| `+0x108` | `disableItem(char)` | `0827f9e0` |

`enableItem(slot)`:
- Returns early when the slot is already held or the soldier is in an
  animation state with flag 2.
- Looks the item up with `getWeaponFromId(ids[slot])`. If that object is
  gone, it clears the slot (`ids[slot] = -1`).
- Asks the item's IPickup (`0xc49c`) `+0x24` whether it can be selected.
- Disables the old slot, stores the new one at `+0x3b8`, and plays
  `Ub_*RaiseWeapon`.
- `enableItemForceNetwork` is the same switch minus some firing checks.

Who switches:
- `handleMessage(TemplateMessage, IPlayer*)` handles the weapon keys:
  messages 10..17 call `enableItem(1..8)`. Message 13 with the detonator held
  triggers explosive packs. Other messages switch too, e.g. to slot 11 (the
  detonator) after an explosive pack is placed.
- The game also switches by itself through `selectBestLoadedWeapon`,
  `pickupKit`, `Kit::enable` and `useRepairPack`.

Replication:
- The server replicates the held slot. `updateStateMask` copies `+0x3b8`
  into the baseline and sets state bit `0x100` when it changes, and
  `getNetUpdate` writes it as 32 bits.
- On the receiving side, `setNetUpdate` calls `enableItemForceNetwork(slot)`,
  but only when the networkable's `+0x14` byte is 0. That byte also guards
  `networkForceAnimationState`, so it is most likely "controlled locally".
  Live, however, the owner's client does follow the server's held slot (see
  below).
- It is unknown whether a client's switch reaches the server as a key
  message (the `enableItem` path) or as soldier state (the
  `enableItemForceNetwork` path). RMM hooks both and reports them as reasons
  PLAYER and NETWORK.

Live results:
- The owner's client follows the server's held slot, despite the `+0x14`
  guard.
- With `lock 3`, pressing 1, 2 or 4 plays the client's switch animation,
  then the client goes back to slot 3. Every attempt was logged as blocked.
- `select 2` switched the owner's client.

`removeItem(slot)` detaches the item (vtable `+0x84`), clears the slot and
calls the root networkable's `+0x30`, but does not destroy the item. RMM's
`remove_item` was built as:
1. switch away from the slot if it is held;
2. call `removeItem`;
3. destroy the item with `GameServer::destroyObject`, which tells every client.

The expectation was that the owner's client would find the slot's object
gone the next time `enableItem` looks, and clear the slot itself. It
doesn't. Live:
- The first press of 2 after the removal did nothing.
- The second press crashed the server with `Fatal error: No physicsNodeComp!`.
  That message comes from `FireArms::fireBarrel` when the projectile's `+0x60`
  physics node is null, so the detached pistol was still being fired.

`remove_item` is therefore disabled on BF1942 (`kRemoveItemEnabled` in
`r1_native.cpp`).

A related client bug: `set_kit` followed by `select_item` (slot 2) in the
same server tick left the client stuck (view pointed at the ground, soldier
jumping diagonally). Gun game selects the weapon 1 s after the kit
(`kHoldDelay`); verified live with a real client.

RMM's hooks: `enableItem`, `enableItemForceNetwork` and `handleMessage`
(which only sets the reason: messages 10..17 are PLAYER, any other message
is GAME). Gamedata keys are `soldier_enable_item`, `soldier_enable_item_net`,
`soldier_get_item`, `soldier_remove_item` and `soldier_handle_message`, plus
the offsets `soldier_item_ids`, `soldier_active_item` and
`soldiertemplate_item_slots`.

## Map spawners, spawn timing, sounds

### Spawners

- `ObjectSpawner::handleFrameUpdate` (`0831a930`) calls `spawnObject()`
  (`0831b110`) for each empty slot once the timer at `+0x120` reaches 0 (and
  the spawner has no use button).
- `spawnObject()`:
  1. Looks up the team's template (std::map at `+0x13c`, keyed by `+0x134`).
  2. Calls `GameServer::spawnObject(tmpl, Pos3, Vec3)` (`081394b0`, which is
     just `createObjectOnAllClients(..., 1, 0)`) through `game+0x50`.
  3. Only if that returns an object: sets spawner id, TTL and team, and calls
     `calcSpawnDelay()` (`0831b4a0`; min/max delay at template
     `+0x14c`/`+0x150`, scaled by the player count).
  4. Returns the object id, or -1.
- On -1 the slot stays empty, and the next frame tries again. RMM's block
  therefore returns null from the GameServer call (only inside the spawner's
  call: `g_spawnerContext`), then runs `calcSpawnDelay()` itself, with a 5 s
  minimum.
- A removed vehicle's slot is cleared when the spawner finds the object
  gone; it respawns once the timer runs out.

### Spawn timing

- GameServer fields:
  - `+0x200` per-player countdowns on.
  - `+0x201` spawn waves (`setSpawnWaves`).
  - `+0x204` delay (`setSpawnDelay`, from `game.serverSpawnDelay`, 3 here).
  - `+0x208`/`+0x20c` wave countdowns.
  - `+0x210` wave window (`setSpawnWaveTime`, 5).
  - `+0x214` time between waves (`setTimeToNextWave`, from `game.serverSpawnTime`, 20).
- `killPlayer` (`08154xxx`) sets the dead player's countdown (BFPlayer
  `+0x8c`): `+0x204` with waves off, or the time to the next wave with them on.
- `gameStatusPlaying` counts it down. Every third frame it spawns players
  whose countdown is 0 and who have picked a spawn point (`+0x90`).
- Setting `game.serverSpawnTime/serverSpawnDelay` on the console did not
  change the fields, not even after `admin.restartMap`. RMM writes the
  fields directly. All setters are virtual (no direct callers), so where the
  settings get copied in is unknown.

### Sounds

The server has no "play sound" event: the full list of GameEvents is
`*Event::serialize` in the symbols.
- The only sound a server can make a client play is a radio message:
  `RadioMessageEvent` {u16 message, bool team, u8 sender}.
  `GameServer::radioMessage(int player, int msg, bool team)` sends it to the
  sender's team or to anyone within 70 m. The client plays the comm-rose
  voice and shows the line in chat.
- Weapon sounds, like the M1 Garand's clip ping (`M1_Final_Reload.wav` in the
  Garand's `.ssc` patches), are played by each client from its own copy of
  the weapon's state. The server can't trigger one alone.

## Live-test checklist

`[x]` = verified (in this file or in `bf1942-windows.md`, "Live tests"),
`[ ]` = open. Most open items need a human client.

- [x] 1. Start with `LD_PRELOAD=…/librmm.so ./bf1942_lnxded.dynamic …`. The
  log should show the symbol count, then "native Linux backend attached",
  then plugins loading.
- [x] 2. `rmm version`, `rmm caps`, `rmm events` from the server console.
  This confirms `executeLine` interception reaches the local console.
- [ ] 3. Same from BFRM remote admin and the in-game console.
- [ ] 4. Join with two clients: connect/team events, `!help`, `who`/`!who`
  names and teams, then `kick`/`ban`.
- [ ] 5. Confirm every `VERIFY` in `gamedata/bf1942.ini` (object prefixes).
  The `listMaps` output format is known (see "Console"); `ParseMaplist`
  doesn't parse it.
- [ ] 6. Map change, then check that the player list survives level
  reloads. (`MAP_LOAD` and RMM surviving a server restart on
  `admin.changeMap` are verified on Windows.)
- 7. Kill, damage and world objects:
  - [x] `rmm caps` shows `kill_player`, `damage_player`, `world_objects` and
    `object_velocity`.
  - [x] `slay`/`slap` a bot.
  - [ ] `rmm objects` on a naval map: handles, roots vs seats, and occupants
    when someone mans a gun.
  - [ ] `rmm object <h> stop` and `rmm object <h> pos …` on an empty boat:
    does it stay put and do clients see it move?
- 8. Knock-back:
  - [x] `rmm caps` shows `player_push`; `slap` launches a soldier and the
    client follows (`!slap`).
  - [ ] `slap <human> 0 12`: does the soldier leave the ground on that
    player's own client (their movement may be client-predicted)?
  - [ ] Does a big launch (`0 30`) cause fall damage, and does a player in a
    jeep only take damage?
- 9. Spawning objects (server side verified, see "Find, spawn, remove,
  ground"). With a client:
  - [ ] `rmm spawn German_Assault @me` makes a kit appear on the ground. Can
    it be picked up (walk over it, or use the pickup key)? How long does it
    stay?
  - [ ] `rmm spawn T34 @me` in the open: does the tank appear and can you
    drive it?
  - [ ] `rmm object <h> rot 90 0 0` / `pos ...`: does the client follow?
  - [ ] `rmm remove <h>`: does it vanish for the client?
  - [ ] `teleport`: does the player's own view jump, or does client
    prediction snap them back?
- 10. Bots (server side verified on co-op Berlin, see "AI bots"):
  - [ ] With a client, `rmm bots move <bot> @me` and watch the bot head your
    way before the strategic AI re-plans.
  - [ ] `rmm bots strategy 2 <name>`: does the Axis visibly change plan?
  - [ ] RE: why adding a bot hung the SAI.
- 11. Bot input and removal (server side verified):
  - [ ] With a client: `rmm bots control <bot> exclusive`, then
    `rmm bots input <bot> throttle=1`, `throttle=-1`,
    `mouse_look_x=0.5 hold 500`, `fire=1`, `crouch=1`. Watch what each slot
    does to a soldier and fill in the `RMM_BI_*` comments.
  - [ ] `rmm bots remove <bot>` in view: the soldier falls dead, then
    vanishes for the client?
- 12. Game modes:
  - [x] `rmm caps` shows `set_kit`, `end_round`, `set_score`, `tickets`;
    `rmm events` shows `player_kit_pickup` and `vehicle_enter` as blockable.
  - [x] `rmm kit @me Rus_AT` (on foot) changes the kit, for any faction's kit
    template.
  - [ ] With an observer: the weapons change for them too, the old kit does
    not stay on the ground, and the scoreboard icon follows for a team's own
    kit.
  - [x] A plugin blocking KIT_PICKUP (reason pickup): picking up a kit does
    nothing, with no client-side glitch.
  - [x] A plugin blocking VEHICLE_ENTER: the enter key at a vehicle does
    nothing (a held key retries about 8 times a second).
  - [ ] Which events a death raises (expected: KIT_DROP reason other).
  - [ ] `rmm score @me 25` on the scoreboard.
  - [ ] `rmm endround 2`: the end screen shows team 2, ROUND_END winner 2,
    and the next round starts normally (`end_round 2` itself runs without
    errors). `rmm endround draw`.
- 13. Vehicles (see "Vehicle entry, exit and seats"):
  - [x] `rmm events` shows `vehicle_exit` blockable (if not, the log says
    why the checkPlayerTriggers hook is off).
  - [x] `rmm objects` on a vehicle: seat numbers match the F-key seats.
  - [x] `rmm putin @me <root> 0` and `rmm eject @me`.
  - [ ] `rmm putin` into the gunner seat: does the view follow?
  - [x] With exits blocked, the use key and F-keys keep you in with no
    client glitch reported, and dying in the tank still takes you out.
  - [x] Enter and exit spam per held key: up to ~15 per second.
- 14. Object damage and the gungame plugin:
  - [x] `rmm caps` shows `object_damage`; `rmm object <h> damage` and the
    gun game plugin run with bots (Windows).
  - [ ] `rmm spawn T34 @me`, then `rmm object <h> damage 100000`: does the
    tank explode for the client (effect, sound, wreck), does
    `VEHICLE_DESTROYED` follow, and does someone sitting inside die with the
    attacker credited?
  - [ ] Gun game with `gungame.ladder` naming a short ladder (kit, vehicle,
    kit): the vehicle appears beside you and you are put in at once; the use
    key keeps you in; dying inside explodes it; a level-up from a vehicle to
    a kit takes you out and removes it; `!gg stuck`.
  - [ ] Weapon names: per "Kill weapon names" a kill reports the hand
    weapon's template (`KnifeAxis`, `Bar1918`, `GrenadeAllies`) or, from any
    vehicle gun, the vehicle (`T34`). With eventmonitor's `player_killed`
    lines, confirm a knife, a main weapon, a pistol, a grenade, a landmine,
    an explosion pack, a tank's cannon and its MG, and a roadkill (is any of
    them the projectile or empty?). If they differ, fix `weapons=` in
    `configs/gungame/*.cfg` and `docs/BF1942_WEAPONS.md`.
  - [ ] A kill from your gun game vehicle counts and one on foot doesn't
    (with `gungame.vehicleExit 1`).
  - [ ] Vehicle templates per map: `rmm find Willy` on a map that doesn't
    spawn jeeps (liberation_of_caen). Does a map load only the vehicles it
    spawns?
- 15. Items, using eventmonitor's `WEAPON` lines and the log:
  - [x] `rmm caps` shows `items`, and `rmm events` shows
    `player_weapon_switch` as blockable.
  - [x] `rmm items @me` lists your slots (compare with
    `docs/BF1942_WEAPONS.md`).
  - [ ] Press 1-5 (and the mouse wheel). Which reason does each switch come
    with (player or network)? Does the mouse wheel arrive as slot messages?
  - [x] `rmm items @me lock 3`, then press 2: the log says "items lock: ...
    blocked", and the owner's client snaps back to the main weapon.
  - [ ] What does a second player watching you see?
  - [x] `rmm items @me unlock`.
  - [x] `rmm items @me select 2` while holding the main weapon: your own
    client follows.
  - [x] ~~`rmm items @me remove 2`~~: crashed the server; `remove_item` is
    disabled (see "Weapon switching").
  - [ ] Bots: does a bot's switch report reason player? (So far only their
    spawn-time switches, reason `game`, are in the events log.)
  - [x] `gungame.lockWeapons 1` in a ladder with a pistol level (played; the
    pistol level works with a real client).
- 16. Spawners and spawn timing (gun game on co-op Berlin):
  - [x] Without a client: `spawner_spawn` is supported and blockable. Gun
    game removed 7 map vehicles at round start, and none came back. The
    GameServer fields read waves=0 and delay=0 (`gdb_spawn_fields.sh`).
    Slain bots respawned 0.1 s after dying (previously 6-30 s).
  - [x] With a client: respawns are instant, gun game vehicles appear in the
    open (not inside buildings), and map vehicles stay away all round with
    the default `gungame.mapVehicles 0`.
  - [ ] `gungame.mapVehicles 1`: do the map's vehicles come back after their
    respawn time?
  - [ ] `gungame.enabled 0`: are spawn waves back (`gdb_spawn_fields.sh`:
    waves=1 delay=3)?
- 17. Control points and the rush plugin (conquest map; set
  `gungame.enabled 0`):
  - [ ] `rmm caps` shows `control_points`, `control_point_set` and
    `control_point_lock`. `rmm cps` lists the map's flags with sensible names,
    owners, positions and radii (an empty list = the class id or the template
    check is wrong on this platform).
  - [ ] `rmm cps team <cp> 2` on a team 1 flag: the flag changes on the
    client's map and in the world, team 2 can spawn there, team 1 can't, and
    the point's map vehicles change side when they next spawn.
  - [ ] `rmm cps lock <cp>`, then stand at it with the enemy team: nothing
    happens (no flag movement, no capture). `rmm cps unlock <cp>`: capturing
    works again.
  - [ ] Rush round (`rush.stageTime 120` to see the clock): both teams' ticket
    counters (attackers' tickets; defenders' seconds), capturing a stage's
    points, the fall-back message, spawning at captured points, the end
    screen for both endings.
  - [ ] The engine's own ticket bleed doesn't end the round early.
  - [ ] A map edited with `Rush_<stage>_<name>` control points in its conquest
    layer: Rush uses them (log: "the map's Rush_ points"). Played as conquest
    with `rush.maps` leaving it out: the Rush points show as neutral flags,
    give no spawns, can't be taken, and the ticket bleed ignores them.
- 18. Query rules, voice chat and FastDL (Windows and Linux; a client with the
  BF1942 patch's voice chat; the Interposer library in the server's folder or
  `rmm/bin`; UDP 27800 open):
  - [ ] `rmm caps` shows `query_rules`. With `fastdl.port 8080`, a GameSpy
    `\status\` query to the server (port 23000) has `\commsport\27800` and
    `\fastdlport\8080` before `\final\`, and the server browser still lists
    the server. (Windows: the WSOCK32 sendto import, ordinal 20, carries the
    replies; if not, find the module that sends them.)
  - [ ] `voice` after joining: 1 of 1 connected; the client found the port by
    itself (no `voiceChat.port`).
  - [ ] Two clients: proximity voice fades with distance, pans left/right
    with facing (`rmm_player_info::forward`; Windows uses
    getAbsoluteTransformation `+0x3c`), and also works for a gunner seat.
    Team radio reaches only the team; a dead player hears the radio only.
  - [ ] `voiceadmin players` / `kick` / `mute`; `voiceadmin restart` keeps
    clients able to reconnect; `rmm plugins unload voicechat` removes
    `\commsport\` from the replies.
  - [ ] FastDL: a client missing a map downloads it from `fastdl.host`.
