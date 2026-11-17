# Architecture

## Background

RMM takes most of its design from existing plugin frameworks:

| Idea | From | In RMM |
|---|---|---|
| A loader between the engine and plugins, with C entry points (`Query`/`Attach`/`Detach`) and PRE/POST result codes (IGNORED/OVERRIDE/SUPERCEDE) | Metamod, QMM2 | `RMM_Query`/`RMM_Load`/`RMM_Unload`, `rmm_result` (CONTINUE/CHANGED/HANDLED/STOP) |
| One support module per game behind a shared plugin API | QMM2 (`game_*.cpp`) | `IGameAdapter` implementations in `src/games/` |
| Gamedata files with signatures and offsets, so a game patch doesn't need a rebuild | SourceMod | `gamedata/<game>.ini` |
| Admin flags, immunity, target strings, `!`/`/` chat triggers, auto-executed plugin configs, `sm plugins ...` | SourceMod | `rmm_admin.h`, `players->target`, `rmm.chatTrigger`, `configs/plugins/*.cfg`, `rmm plugins ...` |
| Inter-plugin interfaces looked up by name and version | Metamod factories | `core->iface_register` / `iface_query` |

## Layers

```
 ┌─────────────────────────────────────────────────────────────────┐
 │ Plugins (.dll/.so): C ABI only, never link against RMM          │
 │   hello.c   basecommands.cpp   eventmonitor.cpp   gungame.cpp   │
 └───────────────▲─────────────────────────────────────────────────┘
                 │ rmm_api function tables (include/rmm/*.h)
 ┌───────────────┴─────────────────────────────────────────────────┐
 │ Core (src/core): game-independent                               │
 │  PluginManager  EventBus  VarRegistry  CommandRegistry          │
 │  TimerManager   AdminManager  interface registry  Logger        │
 └───────────────▲─────────────────────────────────────────────────┘
                 │ IGameAdapter (src/games/game_adapter.h)
 ┌───────────────┴─────────────────────────────────────────────────┐
 │ AdapterBase (gamedata-driven commands, settings, capabilities)  │
 │   R2Adapter "python-bridge"   R1Adapter "native"   SimAdapter   │
 └───────────────▲─────────────────────────────────────────────────┘
                 │ Python host API / native hooks / simulated console
 ┌───────────────┴─────────────────────────────────────────────────┐
 │ Game server process (bf2_w32ded.exe, BF1942_w32ded.exe, ...)    │
 └─────────────────────────────────────────────────────────────────┘
```

## Loading into the server

| Platform | Mechanism |
|---|---|
| Windows, BF1942 | A `winmm.dll` proxy next to the server executable (`src/loader`). The server imports `winmm.dll`, and Windows searches the application folder first. The proxy forwards every export to the system DLL. From `DllMain` it redirects the executable's entry point to a stub that loads `rmm\bin\rmm.dll` and calls `RMM_Bootstrap` on the main thread, outside the loader lock and before any server code. When the server restarts itself as a new process (BF1942 does this after a map change), the new process loads the proxy again. |
| Windows, other games | `rmm_launcher.exe` starts the server suspended, loads `rmm.dll` into it with `LoadLibraryA` in a remote thread, calls `RMM_Bootstrap`, then resumes the server. RMM is in place before any server code runs, so it can patch imports such as `Py_Initialize`. If any step fails, the server starts without RMM (`--strict` makes it fail instead). Antivirus behaviour monitors may flag the injection. |
| Linux | `LD_PRELOAD=librmm.so`. A library constructor calls `RMM_Bootstrap`. `librmm.so` exports `Py_Initialize` to interpose the game's. |

`RMM_Bootstrap` works out the game from the executable name (override with
`RMM_GAME=bf2|bf2142|bf1942|bfv`), finds the RMM folder (`RMM_ROOT`, or
`<cwd>/rmm`), creates the matching adapter and calls `Core::Start`.

### Startup

1. `Core::Start` creates RMM's folders, opens the log, loads saved variables
   from `configs/rmm_vars.cfg`, registers the core variables and built-in
   commands, and calls `adapter->Attach()`.
2. The adapter installs its hooks and calls `Core::OnGameReady()` once the
   engine can run console commands:
   - BF2/2142: when the admin script calls `rmm_bridge.init()`.
   - BF1942/BFV: right after the native hooks are installed.
   - Simulator: immediately.
3. `OnGameReady` loads `admins.cfg` and `bans.txt`, runs `configs/core.cfg`,
   loads every plugin in `plugins/` in alphabetical order, calls
   `RMM_AllLoaded` on each, and fires `RMM_EV_SERVER_START`.
4. From then on the adapter calls `Core::Frame()` every server frame (every
   100 ms through a `bf2.Timer` on BF2/2142). Each frame dispatches
   `RMM_EV_FRAME`, runs timers, polls watched engine variables and carries out
   deferred plugin operations.

## Event dispatch

`EventBus::Dispatch(id, payload)`:

1. **PRE** handlers run by priority (highest first, then in registration
   order). The highest `rmm_result` returned wins. `RMM_STOP` ends the PRE
   phase early, on blockable events only.
2. If the event isn't `RMM_EVF_BLOCKABLE` on this game, the result is capped at
   `RMM_CHANGED`. A plugin can't block something the engine will do anyway.
3. If the result is below `RMM_HANDLED`, the **POST** handlers run.
4. The adapter acts on the result: it drops a chat line, rejects a
   connection, applies the modified damage, and so on.

The payload is a plain C struct. Fields marked `[mutable]` in the headers may
be changed in PRE when the event is `RMM_EVF_MUTABLE` on the running game.

Core policies run around the event rather than in plugins. For example,
`Core::OnChat` runs chat commands first (and hides silent `/cmd` lines where
the game allows it), and `Core::OnPlayerConnect` and `OnPlayerAuth` enforce
RMM bans before plugins see the player.

Handlers may fire events, kick players (which fires a disconnect), and add or
remove hooks during a dispatch. Hooks added during a dispatch are merged when
the outermost dispatch ends; removed hooks are marked dead and compacted
later.

## Safety model

| Risk | Mitigation |
|---|---|
| Plugin built for a different API | `RMM_Query` is called first. `api_major` must match, and a plugin built for a newer `api_minor` than the core is refused. The info struct's size is checked. If any check fails, no other plugin code runs. |
| Plugin built for a different game | The `rmm_plugin_info::games` mask is checked before `RMM_Load`. |
| Plugin crashes | Every call into a plugin goes through `Core::CallPlugin` → `GuardedCall`. MSVC builds use SEH to catch access violations, stack overflows, division by zero and unhandled C++ exceptions. The plugin is marked **FAILED**: it gets no further calls, its registrations are released at the end of the frame, and its DLL stays loaded, since its state is unknown. `rmm plugins reload` loads a fresh copy. Linux builds catch C++ exceptions only. |
| Plugin leaks registrations | Every hook, command, variable, timer, watch and interface belongs to the plugin that registered it and is removed on unload or failure. RMM variable values survive a reload. |
| Unloading while plugin code is on the stack | Load, unload, reload and failure cleanup are deferred while any dispatch or plugin call is in progress, and run from `ProcessPending()` at a safe point. |
| Replacing a DLL while the server runs | Plugins are loaded from a shadow copy (`plugins/.cache/<name>.<id>.dll`), so the original can be overwritten and reloaded on Windows. |
| Invalid handles from plugins | Every API call that takes `self` validates it. Buffers are always size-bounded, and struct copies respect the caller's `struct_size`. |
| Data crossing C runtimes | No C++ types, allocations or `va_list`s cross the ABI. Strings are copied into caller-provided buffers. |
| RMM itself breaking the server | The loader and the launcher start the server without RMM if RMM can't load. On Windows the native adapter only attaches to the exact executable build its gamedata addresses are for (checked by PE timestamp), and only enables a feature when the gamedata has every address and offset that feature uses. |

## ABI evolution

- Every function table starts with `struct_size`, and new functions are only
  ever appended. Plugins check for them with `RMM_HAS(table, type, member)`.
- `rmm_player_info` and `rmm_map_info` are caller-sized. RMM copies at most
  `struct_size` bytes, so older plugins keep working when fields are added.
- New event ids and new `rmm_setting`/`rmm_cap` values can be added in minor
  versions. Existing event payloads are frozen; if a payload has to change,
  it becomes a new event.

## Threading

Everything runs on the server's main thread, and RMM functions must not be
called from any other thread. Plugins that start their own threads (for HTTP
or SQL, say) must hand results back to the main thread, for example through
a queue polled from a timer or `RMM_EV_FRAME`.

## Adapters

### AdapterBase

Implements kick, ban, kill, team changes, private messages, map control, the
map list, messages, tickets and engine variable access as console command
templates read from gamedata. By default a capability is reported if its
template exists; `[capabilities]` in gamedata can override that. A concrete
adapter only has to provide console execution, the player list and player
info, events, and its own startup and shutdown.

### R2Adapter: BF2 and BF2142 (`python-bridge`)

The Refractor 2 servers embed Python 2.3 and have an event API
(`host.registerHandler`) that BF2 admin tools have always used. RMM builds on
that API:

```
engine ──host events──▶ rmm_bridge.py ──_rmm.player_spawn(...)──▶ Core ──▶ plugins
plugins ──▶ Core ──▶ R2Adapter ──rmm_bridge.exec()/player_info()──▶ host.rcon_invoke / bf2.*
```

- RMM intercepts `Py_Initialize` (an import-table patch on Windows, symbol
  interposition on Linux) and registers the `_rmm` built-in module right
  after it.
- The Python C API is looked up at runtime in `dice_py`. Python 2.3 has no
  `Py_DecRef`, so reference counts are updated through the object header,
  whose layout is stable.
- Python handlers run after the fact, so chat and connections can't be
  blocked. RMM reports this through the `RMM_EVF_*` flags, and core policy
  adjusts: a banned player is kicked instead of rejected.
- On the Linux BF2 server, native hooks add what the Python API lacks
  (currently damage events). See [GAME_SUPPORT.md](GAME_SUPPORT.md).

### R1Adapter: BF1942 and BFV (`native`)

These games have no scripting layer, so all events come from inline hooks on
engine functions. One source file, `r1_native.cpp`, serves both platforms. On
Linux, functions are found by mangled name in the executable's symbol table;
on Windows, by address for the one supported build. `r1_abi.h` hides the
differences in calling convention and C++ runtime (`__thiscall`, and the
engine's own `std::string` and containers). Each hook is independent: a
missing address disables only the features that need it. See
[GAME_SUPPORT.md](GAME_SUPPORT.md).

### SimAdapter (`sim`)

An in-memory simulated server with players, a console, a map list and
tickets. It runs the real core and real plugin binaries, can report itself
as any game, and is driven by `rmm_simhost` interactively or from test
scripts.

## Roadmap

1. Test BF2 on a live server and resolve every `VERIFY` item, then BF2142.
2. BFV: finish and live-test the remaining hooks, and support the Windows
   server.
3. Crash isolation for plugins on Linux (`sigsetjmp` around plugin calls,
   opt-in).
4. A scripting layer on top of the C API (Lua or Python plugins).
5. SQL-backed admins and bans, and web-based administration, as plugins.
