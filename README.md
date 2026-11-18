# Refractor MultiMod (RMM)

RMM is a server-side plugin framework for games on DICE's Refractor engine. It
plays the role that [Metamod](https://www.metamodsource.net/) and
[SourceMod](https://www.sourcemod.net/) play for Source, or
[QMM2](https://github.com/thecybermind/qmm2) for id Tech 3: one plugin API
across several games, with crash-guarded plugin loading and shared admin
tools.

| Game                | Engine        | Backend         | Status |
|---------------------|---------------|-----------------|--------|
| Battlefield 1942    | Refractor 1   | `native`        | Linux and Windows 1.61 servers. Tested on live servers |
| Battlefield Vietnam | Refractor 1.5 | `native`        | Linux server only. Core events tested live; kills, vehicles and damage untested |
| Battlefield 2       | Refractor 2   | `python-bridge` | Implemented, not yet tested on a live server |
| Battlefield 2142    | Refractor 2   | `python-bridge` | Implemented, not yet tested on a live server |
| (none)              |               | `sim`           | Simulator for developing plugins without a game |

See [docs/GAME_SUPPORT.md](docs/GAME_SUPPORT.md) for what each backend
supports.

## Features

- **C ABI plugins.** Plugins are shared libraries written in C or C++ with any
  compiler. They never link against RMM. An optional header-only C++17 layer
  is included.
- **Safe loading.** The API version and target game are checked before any
  plugin code runs. Every call into a plugin is guarded: a plugin that crashes
  is disabled and the server keeps running. Plugins can be loaded, unloaded,
  reloaded and paused at runtime, and everything a plugin registered is
  released when it goes.
- **Events.** PRE and POST hooks with priorities. Where the game allows it, a
  PRE hook can block the action or change its data. Events cover players
  (connect, auth, spawn, kill, damage, team, squad, score, kits, weapon
  switches, revives), vehicles, control points, chat, console commands,
  rounds, maps and server frames. Plugins can also define their own events.
- **Variables.** Direct access to engine variables, portable settings
  (`RMM_SETTING_GRAVITY` maps to the right engine variable on each game), and
  plugin variables with defaults, bounds, change callbacks and persistence.
- **Administration.** SourceMod-style admin flags and immunity, RMM-managed
  timed bans, target strings (`@all`, `@t1`, `#3`, partial names), and
  commands that work the same from the console, RCon and chat (`!kick`, or
  `/kick` to hide the command).
- **Game control.** Map changes and the map list, chat and team messages,
  tickets, kits, score, round end, kill, team swap, teleport, damage, health
  and knock-back.
- **World objects.** Find vehicles and other objects, see who is in each
  seat, read and set position, rotation and velocity, spawn and remove
  objects, damage vehicles, and query terrain height. List control points,
  hand them to a team, and lock them against capture.
- **AI bots.** List and inspect bots, give them movement orders, steer each
  team's strategic AI, change skill, remove bots, and take over a bot's input
  from a plugin. On BF1942 this uses the engine's own AI and works in co-op
  rounds.
- **Capability reporting.** The games differ (BF2 can't block chat, BF1942 has
  no scripting layer), so plugins ask what the running game supports.
- **Gamedata files.** Console command names, setting names and engine
  addresses live in `gamedata/<game>.ini`, so supporting a new patch or mod is
  usually a text edit.
- **Plugin interfaces.** Plugins can publish versioned interfaces for other
  plugins (Metamod-style), and each plugin gets an auto-executed config file.

## Repository layout

```
include/rmm/        Public SDK: rmm.h (C), rmm.hpp (C++), one header per API area
src/core/           Game-independent runtime: plugins, events, vars, commands, admin
src/games/          Game adapters: common/, refractor1/, refractor2/, sim/
src/host/           In-server entry point (rmm.dll / librmm.so)
src/loader/         winmm.dll proxy that loads RMM into a Windows server
src/launcher/       rmm_launcher.exe: starts a Windows server with RMM injected
src/simhost/        rmm_simhost: runs the core and plugins against a simulated server
python/rmm/         rmm_bridge.py, the Python side of the BF2/2142 backend
gamedata/           Per-game commands, settings and addresses
configs/            Example configs (core.cfg, admins.cfg, plugins/*.cfg)
plugins/            Bundled plugins: hello (C), basecommands, eventmonitor, gungame, rush, voicechat, fastdl
tests/              Simulator test scripts run by CTest
docs/               Architecture, plugin guide, game support, reverse-engineering notes
```

## Building

You need CMake 3.16 or later and a C++17 compiler (Visual Studio 2019+, GCC 9+
or Clang 10+). The game servers are 32-bit, so RMM must be built as 32-bit.

```powershell
# Windows
cmake -S . -B build -A Win32
cmake --build build --config Release
ctest --test-dir build -C Release
```

```bash
# Linux
sudo apt install g++-multilib cmake git
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_FLAGS=-m32 -DCMAKE_CXX_FLAGS=-m32
cmake --build build && ctest --test-dir build
```

The tests run the bundled plugins in the simulator.

### Building with the LANCommander Interposer

The `voicechat` plugin embeds the voice server of the
[LANCommander Interposer](https://github.com/LANCommander/LANCommander.Interposer):
its embeddable static library, built with only the features listed in
`RMM_INTERPOSER_FEATURES` (default `VOICE`). Point `RMM_INTERPOSER_DIR` at a
checkout. Its libraries (yaml-cpp, ENet, Opus, miniaudio, MinHook) come from
vcpkg through `vcpkg.json`, so set `VCPKG_ROOT` (or `CMAKE_TOOLCHAIN_FILE`):

```powershell
$env:VCPKG_ROOT = "C:\vcpkg"
cmake -S . -B build -A Win32 -DRMM_INTERPOSER_DIR=C:/src/LANCommander.Interposer
```

The triplet is `x86-windows-static` or `x86-linux`, with the Interposer's own
triplets. Configure a fresh build directory: CMake reads the toolchain only
on the first configure. Without `RMM_INTERPOSER_DIR` everything else builds
and `voicechat` is skipped.

On Linux, `librmm.so` links its C++ runtime statically and exports none of its
own symbols, because it is preloaded into servers that use the much older
`libstdc++.so.5`. Inline hooks use [funchook](https://github.com/kubo/funchook)
on Linux and [MinHook](https://github.com/TsudaKageyu/minhook) on Windows; CMake
fetches both.

To check the BF1942 gamedata against a server binary, pass
`-DRMM_BF1942_LINUX_BINARY=/path/bf1942_lnxded.dynamic` to CMake (this adds a
CTest), or run `build/tools/rmm_elfsym <binary> gamedata/bf1942.ini`.

Build options: `RMM_WITH_MINHOOK` (on by default, required by the Windows
BF1942 backend), `RMM_BUILD_PLUGINS`, `RMM_BUILD_SIMHOST`, `RMM_BUILD_LAUNCHER`
and `RMM_BUILD_SERVER_MODULE`.

The build output in `build/dist/` mirrors a server install:

```
dist/winmm.dll, rmm_launcher.exe
dist/rmm/bin/rmm.dll, rmm_simhost(.exe)
dist/rmm/plugins/*.dll
dist/rmm/gamedata/*.ini
dist/rmm/python/rmm_bridge.py
dist/rmm/configs.example/
```

### Releases

Pushing a semver tag (`v1.2.3`, or `v1.2.3-rc.1` for a pre-release) runs
`.github/workflows/release.yml`. The tag must match the version in `CMakeLists.txt` and
`src/core/core.h`. The workflow builds and tests on Windows and Linux (32-bit), then
publishes a GitHub release with, for each platform, `rmm-<ver>-<platform>.zip` (loader,
core, gamedata, Python bridge, core configs) and one `rmm-plugin-<name>-<ver>-<platform>.zip`
per bundled plugin (the plugin plus its example configs). Every zip unpacks into the server
root. To build the same zips locally, run
`python tools/package_release.py --dist build/dist --version 1.2.3 --platform windows-x86 --out release`.

## Trying it without a game

```
cd build/dist
rmm/bin/rmm_simhost --game bf2
> .join Alice 1
> .join Bob 2
> .say 1 hello !hello
> rmm plugins list
> kick Bob "testing"
> .help
```

See [docs/SIMHOST.md](docs/SIMHOST.md).

## Installing on a server

1. Copy `dist/rmm/` into the server's root folder as `rmm/`, and rename
   `rmm/configs.example/` to `rmm/configs/`.
2. For BF2 and BF2142, add this to the end of `init()` in `admin/default.py`:
   ```python
   import rmm_bridge
   rmm_bridge.init()
   ```
3. Start the server with RMM:
   - **BF1942 on Windows:** copy `winmm.dll` next to `BF1942_w32ded.exe` and
     start the server as usual. The server loads the DLL from its own folder;
     it forwards everything to the system `winmm.dll` and loads
     `rmm\bin\rmm.dll` before any server code runs. This also covers the new
     process the server starts when it restarts itself after a map change. To
     run without RMM, remove the file or set `RMM_DISABLE=1`.
   - **Other games on Windows:** put `rmm_launcher.exe` next to the server
     executable and run `rmm_launcher.exe -- <usual server arguments>`. The
     launcher injects `rmm.dll` into the server process, which some antivirus
     products flag.
   - **Linux:** `LD_PRELOAD=$PWD/rmm/bin/librmm.so ./start.sh`. For BF1942 and
     BFV this must be the `.dynamic` server binary; the static one can't load
     preloaded libraries.
4. Add yourself to `rmm/configs/admins.cfg`, then run `rmm version` in the
   server console or type `!help` in chat.

## Documentation

- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md): how RMM is put together, and
  its safety model
- [docs/PLUGIN_API.md](docs/PLUGIN_API.md): writing plugins
- [docs/GAME_SUPPORT.md](docs/GAME_SUPPORT.md): what each game supports, and
  open work
- [docs/SIMHOST.md](docs/SIMHOST.md): the simulator
- [docs/BF1942_WEAPONS.md](docs/BF1942_WEAPONS.md): BF1942 kit, weapon and
  vehicle names
- [docs/re/](docs/re/): reverse-engineering notes for each server binary

## Status

RMM is pre-release. The MSVC (x86) and GCC (`-m32`) builds compile cleanly and
pass the simulator tests. The BF1942 backend has run on live Linux and Windows
servers; a few features on Windows still need a test with a human player (see
[docs/GAME_SUPPORT.md](docs/GAME_SUPPORT.md)). Entries marked `VERIFY` in the
gamedata and the Python bridge are best-known names that haven't been
confirmed on a live server. BF2 and BF2142 haven't been tested on a live
server. BFV doesn't load on Windows yet; the server then runs without RMM.
