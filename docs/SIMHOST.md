# rmm_simhost: plugins without a game server

`rmm_simhost` runs the real RMM core and real plugin binaries against a
simulated Refractor server. Use it to develop and debug plugins, reproduce bug
reports and run automated tests.

```
rmm_simhost [--root <rmm dir>] [--game bf1942|bfv|bf2|bf2142] [--script <file>]
```

- `--root`: the RMM folder (containing `plugins/`, `configs/` and
  `gamedata/`). Defaults to `./rmm`.
- `--game`: the game reported to plugins (default `bf2`), so that
  game-specific plugins load. Console commands always come from
  `gamedata/sim.ini`.
- `--script`: run the lines in a file, then exit. CTest uses this.

The simulator runs at about 20 frames a second, so timers keep firing while it
waits for input.

## Input

Lines starting with `.` are simulated player and game actions. Every other
line goes to the server console: RMM commands (`rmm plugins list`, `kick Bob`,
`help`), RMM variables (`rmm.chatTrigger`), or the simulated engine console
(`sv.serverName`, `sim.say "hi"`).

### Players

| Action | Effect |
|---|---|
| `.join <name> [team] [bot] [ip]` | Connects a player: CONNECT, AUTH and TEAM |
| `.leave <id>` | Disconnects a player |
| `.say <id> <text>`, `.teamsay <id> <text>` | Chat, including `!cmd` and `/cmd` commands |
| `.spawn <id>` | Spawns the player. They get an `Assault` kit (KIT_PICKUP with reason spawn, not blockable) |
| `.kill <victim> [attacker] [weapon]` | A kill, with teamkill detection, score and tickets |
| `.damage <victim> <attacker> <amount>` | A damage event (blockable, editable). Soldiers have 100 health and die at 0 |
| `.team <id> <team>` | Team change |
| `.weapon <id> <slot>` | The player presses a weapon key. Raises a blockable `PLAYER_WEAPON_SWITCH` with reason player |
| `.kititems <kit> <slot>=<weapon> ...` | Sets what a kit carries from now on. By default every kit carries `1=KnifeAllies 2=Colt 3=<kit>_Gun 4=GrenadeAllies`. A player given a new kit holds slot 3, or the lowest slot if there is no 3 |
| `.pickup <id> <kit handle>` | Picks up a kit lying on the ground (for example one placed with `rmm spawn`). Raises a blockable KIT_PICKUP, and the old kit is dropped as a new object |
| `.players` | Shows the player table |

`rmm kit` and `set_kit` only accept kit templates the simulator knows about;
add them with `.template`.

### Vehicles and objects

| Action | Effect |
|---|---|
| `.vehicle <template> [x y z] [vx vy vz]` | Places a vehicle and prints its handle (`0x1`, `0x2`, ...) |
| `.seat <parent handle> <template>` | Adds a seat object to a vehicle |
| `.enter <id> <vehicle> [seat]` | The player enters a placed vehicle with that template (roots first), or one placed at the player if there is none. ENTER is blockable, as on BF1942 |
| `.exit <id>` | The player presses the use key: EXIT with reason player, blockable |
| `.switch <id> <seat>` | The player presses F1-F8: EXIT, then ENTER, with reason switch. Blocking the EXIT cancels the switch. The root is seat 0, and `.seat` objects are 1, 2, ... in the order they were added |
| `.vdamage <handle> <attacker> <amount>` | A vehicle damage event (blockable, editable) |
| `.drift <seconds>` | Moves every vehicle by its velocity times the given time |
| `.template <name> [server\|vehicle]` | Makes a template available to `rmm spawn` and `spawn_object`. `server`: not networked. `vehicle`: spawned as a vehicle that `enter_vehicle` accepts. Objects have 1000 health; damage beyond that with `damage_object` destroys a vehicle and kills its occupants |
| `.spawner <template> <x y z> [team] [yaw]` | Adds a map spawner, which spawns its vehicle at once and raises a blockable `SPAWNER_SPAWN`. Spawn timing (`set_spawn_timing`) is only stored and printed |
| `.spawners` | Every spawner whose vehicle is gone, or was blocked, tries again |

### Game

| Action | Effect |
|---|---|
| `.map <map> [mode] [size]` | Map load and round start. The simulated AI (for the bots API) only runs if *mode* contains `coop`, as on BF1942, and a map change removes its bots |
| `.aibot <name> [team]` | A bot added by the game itself. `rmm bots add` goes through the simulated AI console instead (`simai.*` in `gamedata/sim.ini`, which uses BF1942's command names and formats). Bot names starting with `Reject` are refused, for testing failures |
| `.endround [winner]` | Ends the round |
| `.capture <cp> <team>` | A control point capture |
| `.wait <seconds>` | Lets time pass: timers fire and queued chat goes out |
| `.pacing on\|off` | Chat pacing. It starts off in scripts, so output stays in order, and on in interactive sessions |
| `.help`, `.quit` | |

## Example session

```
> .join Alice 1
[sim] Alice joined as #0 (team 1)
> .join Bob 2
> .say 1 you noob
[chat] (blocked) Bob: you noob
> .say 0 !hello
[game] (to #0) Hello from RMM 1.0.0 on Simulator (as Battlefield 2)!
> rmm plugins reload eventmonitor
Reloaded.
```

The chat line is blocked by the chat filter in the bundled `eventmonitor`
plugin.

To use admin commands from chat, add a name entry to `configs/admins.cfg`.
Name entries are fine for the simulator but not for a real server:

```
name "Alice" z 100
```
