cmake_minimum_required(VERSION 3.16)

# ---- LANCommander Interposer (optional) ----
# Plugins that use the Interposer's API (voicechat: its voice server) embed its
# static library with only the features they need, so no Interposer DLL/.so is
# deployed and nothing else of it (hooks, config, plugins) runs in the server.
# Its libraries come from vcpkg (vcpkg.json, feature "interposer"), built with
# the Interposer's own triplets. Without RMM_INTERPOSER_DIR those plugins are skipped.
set(RMM_INTERPOSER_DIR "" CACHE PATH "Checkout of LANCommander.Interposer, embedded into the plugins that use it")
set(RMM_INTERPOSER_FEATURES "VOICE" CACHE STRING
    "Interposer features embedded (INTERPOSER_FEATURE_<NAME> without the prefix); voicechat needs VOICE")
if(RMM_INTERPOSER_DIR)
    if(NOT DEFINED VCPKG_TARGET_TRIPLET)
        if(CMAKE_HOST_WIN32)
            set(VCPKG_TARGET_TRIPLET x86-windows-static CACHE STRING "")
        else()
            set(VCPKG_TARGET_TRIPLET x86-linux CACHE STRING "")
        endif()
    endif()
    if(NOT DEFINED VCPKG_OVERLAY_TRIPLETS)
        set(VCPKG_OVERLAY_TRIPLETS "${RMM_INTERPOSER_DIR}/triplets" CACHE STRING "")
    endif()
    set(VCPKG_MANIFEST_FEATURES "interposer" CACHE STRING "")
    if(NOT DEFINED CMAKE_TOOLCHAIN_FILE AND DEFINED ENV{VCPKG_ROOT})
        set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE FILEPATH "")
    endif()
    if(NOT CMAKE_TOOLCHAIN_FILE)
        message(FATAL_ERROR "RMM_INTERPOSER_DIR needs vcpkg: set VCPKG_ROOT or CMAKE_TOOLCHAIN_FILE")
    endif()
endif()

project(RefractorMultiMod VERSION 1.0.0 LANGUAGES C CXX)

# ---- Options ----
option(RMM_BUILD_SERVER_MODULE "Build the in-server module (rmm.dll / librmm.so)" ON)
option(RMM_BUILD_SIMHOST       "Build rmm_simhost (run plugins without a game)" ON)
option(RMM_BUILD_LAUNCHER      "Build rmm_launcher.exe (Windows)" ON)
option(RMM_BUILD_PLUGINS       "Build the bundled example plugins" ON)
if(UNIX)
    set(_rmm_funchook_default ON)
    set(_rmm_minhook_default OFF)
else()
    set(_rmm_funchook_default OFF)
    set(_rmm_minhook_default ON)
endif()
option(RMM_WITH_MINHOOK        "Fetch MinHook for inline detours on Windows (Refractor 1 native hooks)" ${_rmm_minhook_default})
option(RMM_WITH_FUNCHOOK       "Fetch funchook for inline detours on Linux (native engine hooks)" ${_rmm_funchook_default})

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
# Static CRT: old server boxes rarely have matching VC++ redistributables, and
# the plugin ABI is pure C so CRTs never need to match.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# The embedded Interposer. Before RMM's compile options, which it doesn't want,
# and before MinHook below: FetchContent would otherwise answer the Interposer's
# find_package(minhook) with the fetched copy, which has no minhook::minhook.
if(RMM_INTERPOSER_DIR)
    if(NOT EXISTS "${RMM_INTERPOSER_DIR}/CMakeLists.txt")
        message(FATAL_ERROR "LANCommander.Interposer not found at ${RMM_INTERPOSER_DIR}")
    endif()
    set(_rmm_interposer_all REGISTRY FILES FASTDL DIRECTINPUT AUDIO OSVERSION IDENTITY NETWORK RELAY RICH_PRESENCE
        MATCHES VOICE PLUGINS)
    foreach(_feature IN LISTS RMM_INTERPOSER_FEATURES)
        if(NOT _feature IN_LIST _rmm_interposer_all)
            message(FATAL_ERROR "RMM_INTERPOSER_FEATURES: unknown feature '${_feature}' (known: ${_rmm_interposer_all})")
        endif()
    endforeach()
    foreach(_feature IN LISTS _rmm_interposer_all)
        if(_feature IN_LIST RMM_INTERPOSER_FEATURES)
            set(INTERPOSER_FEATURE_${_feature} ON CACHE BOOL "" FORCE)
        else()
            set(INTERPOSER_FEATURE_${_feature} OFF CACHE BOOL "" FORCE)
        endif()
    endforeach()
    set(INTERPOSER_EMBEDDED_ONLY ON CACHE BOOL "" FORCE)
    add_subdirectory("${RMM_INTERPOSER_DIR}" interposer EXCLUDE_FROM_ALL)
endif()

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(WARNING
        "Building 64-bit. BF1942, BFV and the Windows BF2/BF2142 servers are 32-bit processes; "
        "configure with `-A Win32` (Visual Studio) or a 32-bit toolchain for in-server builds. "
        "64-bit is fine for rmm_simhost and plugin development.")
endif()

if(MSVC)
    add_compile_options(/W3 /utf-8 /permissive-)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX)
else()
    add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

# Everything is laid out exactly like a server install: dist/rmm/...
set(RMM_DIST ${CMAKE_BINARY_DIR}/dist/rmm)

# ---- SDK (header-only for plugins) ----
add_library(rmm_sdk INTERFACE)
target_include_directories(rmm_sdk INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
include(cmake/RmmPlugin.cmake)   # rmm_add_plugin(), rmm_output_dir()


# ---- Optional MinHook ----
if(RMM_WITH_MINHOOK AND WIN32)
    include(FetchContent)
    FetchContent_Declare(minhook
        GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
        GIT_TAG 8af6b4acae5a9388fd742b56fa79ece89d96f823)   # v1.3.4 + 17 (BSD-2-Clause)
    FetchContent_MakeAvailable(minhook)
endif()

# Optional funchook for Linux detours (GPLv2+ with a linking exception that
# permits use in any software; bundles the distorm disassembler, BSD).
# Pinned to v1.1.3.
if(RMM_WITH_FUNCHOOK AND UNIX)
    include(FetchContent)
    set(FUNCHOOK_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(FUNCHOOK_BUILD_STATIC ON CACHE BOOL "" FORCE)
    set(FUNCHOOK_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(FUNCHOOK_INSTALL OFF CACHE BOOL "" FORCE)
    set(FUNCHOOK_DISASM distorm CACHE STRING "" FORCE)
    FetchContent_Declare(funchook
        GIT_REPOSITORY https://github.com/kubo/funchook.git
        GIT_TAG 88388db3c69e16c1560fee65c6857d75f5ce6fd5)   # v1.1.3
    FetchContent_MakeAvailable(funchook)
    set_target_properties(funchook-static distorm PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

# ---- Core (static; linked into the server module and the simulator) ----
add_library(rmm_core STATIC
    src/core/admin_manager.cpp
    src/core/api_tables.cpp
    src/core/bot_control.cpp
    src/core/bot_tracker.cpp
    src/core/builtin_commands.cpp
    src/core/chat_queue.cpp
    src/core/command_registry.cpp
    src/core/core.cpp
    src/core/event_bus.cpp
    src/core/logger.cpp
    src/core/platform.cpp
    src/core/plugin_manager.cpp
    src/core/query_rules.cpp
    src/core/safe_call.cpp
    src/core/timer_manager.cpp
    src/core/util.cpp
    src/core/var_registry.cpp
    src/games/common/adapter_base.cpp
    src/games/common/adapter_bots.cpp
    src/games/common/elf_symbols.cpp
    src/games/common/hooks.cpp
)
target_link_libraries(rmm_core PUBLIC rmm_sdk)
target_include_directories(rmm_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
if(UNIX)
    target_link_libraries(rmm_core PUBLIC ${CMAKE_DL_LIBS})
endif()
if(RMM_WITH_MINHOOK AND WIN32)
    target_link_libraries(rmm_core PUBLIC minhook)
    target_compile_definitions(rmm_core PUBLIC RMM_WITH_MINHOOK)
endif()
if(RMM_WITH_FUNCHOOK AND UNIX)
    target_link_libraries(rmm_core PUBLIC funchook-static)
    target_include_directories(rmm_core PUBLIC ${funchook_SOURCE_DIR}/include)
    target_compile_definitions(rmm_core PUBLIC RMM_WITH_FUNCHOOK)
endif()

# ---- In-server module ----
if(RMM_BUILD_SERVER_MODULE)
    add_library(rmm SHARED
        src/host/bootstrap.cpp
        src/games/common/query_hook.cpp
        src/games/refractor1/gcc3_string.cpp
        src/games/refractor1/msvc7_string.cpp
        src/games/refractor1/r1_adapter.cpp
        src/games/refractor1/r1_native.cpp
        src/games/refractor2/pyapi.cpp
        src/games/refractor2/r2_adapter.cpp
        src/games/refractor2/r2_linux.cpp
        src/games/refractor2/r2_module.cpp
    )
    target_link_libraries(rmm PRIVATE rmm_core)
    if(UNIX)
        # librmm.so is LD_PRELOADed into servers that use an ancient
        # libstdc++.so.5. Link our own C++ runtime statically and export none
        # of its symbols, so we can never interpose the game's std::string etc.
        target_link_options(rmm PRIVATE -static-libstdc++ -static-libgcc -Wl,--exclude-libs,ALL)
    endif()
    rmm_output_dir(rmm ${RMM_DIST}/bin)

    if(WIN32)
        # Type-check the Linux-only BF2 native backend on Windows builds
        # (compiled as an object library, never linked). The Refractor 1
        # backend (r1_native.cpp) is built for real on both platforms.
        add_library(rmm_linux_syntax_check OBJECT src/games/refractor2/r2_linux.cpp)
        target_link_libraries(rmm_linux_syntax_check PRIVATE rmm_core)
        target_compile_definitions(rmm_linux_syntax_check PRIVATE RMM_LINUX_SYNTAX_CHECK)
    endif()
endif()

# ---- Simulator ----
if(RMM_BUILD_SIMHOST)
    find_package(Threads REQUIRED)
    add_executable(rmm_simhost src/simhost/main.cpp src/games/sim/sim_adapter.cpp)
    target_link_libraries(rmm_simhost PRIVATE rmm_core Threads::Threads)
    rmm_output_dir(rmm_simhost ${RMM_DIST}/bin)
endif()

# ---- Offline tools ----
add_executable(rmm_elfsym tools/elfsym.cpp)
target_link_libraries(rmm_elfsym PRIVATE rmm_core)
rmm_output_dir(rmm_elfsym ${CMAKE_BINARY_DIR}/tools)

# ---- Launcher (Windows) ----
if(RMM_BUILD_LAUNCHER AND WIN32)
    add_executable(rmm_launcher src/launcher/launcher_win.cpp src/host/inject_win.cpp)
    # winmm.dll proxy loader (no injection): sits next to the server
    # executable too. x86 only (naked jump stubs). Regenerate the export table
    # with tools/gen_winmm_proxy.py.
    if(CMAKE_SIZEOF_VOID_P EQUAL 4)
        add_library(rmm_winmm SHARED src/loader/winmm_proxy.cpp src/loader/winmm.def)
        # Its import library must not be called winmm.lib: that would shadow the
        # system one for everything linked in this build directory.
        set_target_properties(rmm_winmm PROPERTIES OUTPUT_NAME winmm ARCHIVE_OUTPUT_NAME rmm_winmm_proxy)
        rmm_output_dir(rmm_winmm ${CMAKE_BINARY_DIR}/dist)
    endif()
    # The launcher sits next to the server executable, not inside rmm/.
    rmm_output_dir(rmm_launcher ${CMAKE_BINARY_DIR}/dist)
endif()

# ---- Plugins ----
if(RMM_BUILD_PLUGINS)
    rmm_add_plugin(hello plugins/hello/hello.c)
    rmm_add_plugin(basecommands plugins/basecommands/basecommands.cpp)
    rmm_add_plugin(eventmonitor plugins/eventmonitor/eventmonitor.cpp)
    rmm_add_plugin(gungame plugins/gungame/gungame.cpp)
    rmm_add_plugin(rush plugins/rush/rush.cpp)
    rmm_add_plugin(fastdl plugins/fastdl/fastdl.cpp)
    # The Interposer's voice server, embedded (RMM_INTERPOSER_DIR, feature VOICE).
    if(TARGET LANCommander::Interposer::Embedded AND "VOICE" IN_LIST RMM_INTERPOSER_FEATURES)
        rmm_add_plugin(voicechat plugins/voicechat/voicechat.cpp)
        target_link_libraries(voicechat PRIVATE LANCommander::Interposer::Embedded)
        if(UNIX)
            # Old servers ship an ancient libstdc++; carry our own and export none of it.
            target_link_options(voicechat PRIVATE -static-libstdc++ -static-libgcc -Wl,--exclude-libs,ALL)
        endif()
    else()
        message(STATUS "voicechat plugin not built: it needs RMM_INTERPOSER_DIR with the VOICE feature")
    endif()
endif()

# ---- Data files -> dist ----
add_custom_target(rmm_data ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/gamedata ${RMM_DIST}/gamedata
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/python/rmm ${RMM_DIST}/python
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/configs ${RMM_DIST}/configs.example
    COMMENT "Copying gamedata, python bridge and example configs to dist/")

# ---- Tests: drive the real core + plugins through the simulator ----
if(RMM_BUILD_SIMHOST AND RMM_BUILD_PLUGINS)
    enable_testing()
    add_test(NAME sim_smoke
        COMMAND rmm_simhost --root ${RMM_DIST} --game bf2 --script ${CMAKE_CURRENT_SOURCE_DIR}/tests/smoke.rmmsim)
    set_tests_properties(sim_smoke PROPERTIES
        PASS_REGULAR_EXPRESSION "Loaded plugin #[0-9]+ Basic Commands"
        FAIL_REGULAR_EXPRESSION "crashed in|Failed to load")
    add_test(NAME sim_chat_filter
        COMMAND rmm_simhost --root ${RMM_DIST} --script ${CMAKE_CURRENT_SOURCE_DIR}/tests/chatfilter.rmmsim)
    set_tests_properties(sim_chat_filter PROPERTIES PASS_REGULAR_EXPRESSION "\\(blocked\\) Bob: you noob")
    # Paced lines arrive in order after the marker; long lines are split and their tail paced too.
    add_test(NAME sim_chat_pacing
        COMMAND rmm_simhost --root ${RMM_DIST} --game bfv --script ${CMAKE_CURRENT_SOURCE_DIR}/tests/chatpace.rmmsim)
    set_tests_properties(sim_chat_pacing PROPERTIES
        PASS_REGULAR_EXPRESSION "SERVER: \\[ADMIN\\] one[^~]*> say three[^~]*Refractor MultiMod[^~]*> \\.wait 4[^~]*SERVER: \\[ADMIN\\] two[^~]*SERVER: \\[ADMIN\\] three[^~]*SERVER: \\[ADMIN\\] the quick brown fox jumps over the lazy dog and keeps\n[^~]*> rmm version[^~]*> \\.wait 2[^~]*SERVER: running far away\n[^~]*> say four[^~]*SERVER: \\[ADMIN\\] four[^~]*> say five[^~]*SERVER: \\[ADMIN\\] five"
        FAIL_REGULAR_EXPRESSION "SERVER: \\[ADMIN\\] two[^~]*> \\.wait 4|SERVER: running far away[^~]*> \\.wait 2")
    # World objects and damage: key results in order ([^~]* = anything, including newlines).
    add_test(NAME sim_world
        COMMAND rmm_simhost --root ${RMM_DIST} --game bf1942 --script ${CMAKE_CURRENT_SOURCE_DIR}/tests/world.rmmsim)
    set_tests_properties(sim_world PROPERTIES
        PASS_REGULAR_EXPRESSION "carrier\\* empty[^~]*No matching vehicles[^~]*pos 110\\.0,0\\.0,205\\.0 rot[^~]*pos 10\\.0,20\\.0,30\\.0[^~]*willys took 50\\.0[^~]*vehicle 0x3 \\(#2\\) pushed by[^~]*slapped Carl for 30 \\(launched\\)[^~]*Carl has 70 health left[^~]*#1 pushed by -?[0-9.]+,12\\.0,-?[0-9.]+[^~]*slapped Bob \\(launched\\)[^~]*Carl +team 2 score 0 K/D 0/1 dead[^~]*No such object"
        FAIL_REGULAR_EXPRESSION "crashed in|Failed to load|Not supported|Could not slap")
    # Object find (radius, distance order, flags), terrain height, spawn and remove.
    add_test(NAME sim_objects
        COMMAND rmm_simhost --root ${RMM_DIST} --game bf1942 --script ${CMAKE_CURRENT_SOURCE_DIR}/tests/objects.rmmsim)
    set_tests_properties(sim_objects PROPERTIES
        PASS_REGULAR_EXPRESSION "object_spawn +yes[^~]*3 object\\(s\\)[^~]*> rmm find bunker\\*[^~]*bunker_small[^~]*1 object\\(s\\)[^~]*bunker_small[^~]*\\(0 m\\)[^~]*willys[^~]*\\(7 m\\)[^~]*2 object\\(s\\)[^~]*1 object\\(s\\)[^~]*Ground at 200\\.0,0\\.0 is 12\\.00[^~]*Spawned kit_assault 0x4 at 120\\.0,11\\.2,100\\.0 \\(networked\\)[^~]*kit_assault \\(root 0x4 kit_assault\\) networked[^~]*server-only: players won't see it[^~]*Unknown template 'nosuchthing'[^~]*Removed\\.[^~]*No such object[^~]*> rmm find kit\\*[^~]*0 object\\(s\\)[^~]*> rmm objects[^~]*willys"
        FAIL_REGULAR_EXPRESSION "crashed in|Failed to load|Not supported|not available|house_big[^\n]*\\([0-9]+ m\\)")
    # Bots via a fake BF1942 AI console; listing bots outside co-op prints SIMULATED CRASH.
    add_test(NAME sim_bots
        COMMAND rmm_simhost --root ${RMM_DIST} --game bf1942 --script ${CMAKE_CURRENT_SOURCE_DIR}/tests/bots.rmmsim)
    set_tests_properties(sim_bots PROPERTIES
        PASS_REGULAR_EXPRESSION "bot_skill +yes[^~]*AI is not running[^~]*bot_added: #1 Walter \\(bot 0, team 1, tag 0\\)[^~]*bot_added: #2 Ivan[^~]*2 bot\\(s\\)\\. Strategy: team 1 finalPush, team 2 breakOut\\. Skill 0\\.75[^~]*Walter +team 1 KTAssault +Idle[^~]*Behaviours: Avoid MoveTo Idle Fire[^~]*Walter +team 1 KTAssault +MoveTo +pos [^\n]* goal 100\\.0,20\\.0,300\\.0[^~]*Ordered 2 of 2[^~]*Ivan +team 2 KTAssault +Fire[^~]*Dance[^~]*Ivan +team 2 KTAssault +Fire[^~]*Team 1 strategy: north[^~]*Unknown strategy[^~]*Bot skill 0\\.40[^~]*Skill is 0\\.0 - 1\\.0[^~]*\\[sim\\] Walter input: throttle=1 fire=1[^~]*#1 Walter: engine input tick 1, [0-9]+ ms ago: throttle=1\\.00 fire=1\\.00[^~]*#1 Walter: take control of the bot first[^~]*#1 Walter: override by the console[^~]*Input latched for 1 of 1[^~]*\\[sim\\] Walter input \\(rmm\\): throttle=-1 mouse_look_x=0\\.25 fire=1\n[^~]*#1 Walter: exclusive by the console[^~]*\\[sim\\] Walter input \\(rmm\\): throttle=-1 mouse_look_x=0\\.25\n[^~]*Input latched for 1 of 1 bot\\(s\\) for 100 ms[^~]*\\[sim\\] Walter input \\(rmm\\): idle[^~]*#1 Walter: engine\n[^~]*\\[sim\\] Walter input: throttle=1 fire=1[^~]*Unknown input 'nonsense=1'[^~]*bot_added: #3 Doc \\(bot 2, team 2, tag 1\\)[^~]*bot_added: #4 Doc_2[^~]*Doc +team 2 KTMedic +Idle +pos 50\\.0,10\\.0,60\\.0[^~]*team must be 1 or 2[^~]*RejectMe left \\(kicked[^~]*bot_add_failed: #-1 RejectMe[^~]*bot_removed: #1 Walter[^~]*3 bot\\(s\\)[^~]*bot_removed: #4 Doc_2[^~]*AI is not running"
        FAIL_REGULAR_EXPRESSION "SIMULATED CRASH|crashed in|Failed to load")
    # Game-mode calls. The apitest fixture is built outside dist/plugins so it never ships or autoloads.
    add_library(apitest SHARED tests/plugins/apitest.cpp)
    target_link_libraries(apitest PRIVATE rmm_sdk)
    set_target_properties(apitest PROPERTIES PREFIX "")
    rmm_output_dir(apitest ${CMAKE_BINARY_DIR}/testplugins)
    add_test(NAME sim_gamemode
        COMMAND rmm_simhost --root ${RMM_DIST} --game bf1942 --script ${CMAKE_CURRENT_SOURCE_DIR}/tests/gamemode.rmmsim
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(sim_gamemode PROPERTIES
        PASS_REGULAR_EXPRESSION "set_kit +yes\n[^~]*end_round +yes[^~]*player_kit_pickup +supported blockable[^~]*vehicle_enter +supported blockable[^~]*apitest: kit_pickup #0 Assault \\(spawn\\)[^~]*apitest: kit_drop #0 Assault \\(plugin\\)[^~]*#0 now carries GermanAssault[^~]*apitest: kit_pickup #0 GermanAssault \\(plugin\\)[^~]*Gave GermanAssault to 1 of 1 player[^~]*Unknown template 'nosuchkit'[^~]*apitest: kit_pickup #1 Assault \\(spawn\\)[^~]*apitest: kit_pickup #1 Rus_AT \\(plugin\\)[^~]*apitest: spawn kit for #1: Rus_AT[^~]*#1 Bob [^\n]*kit Rus_AT[^~]*Spawned GermanAssault 0x1[^~]*apitest: blocking pickup of GermanAssault by #1[^~]*#1's pickup of GermanAssault blocked[^~]*apitest: kit_drop #1 Rus_AT \\(pickup\\)[^~]*Rus_AT dropped as 0x2[^~]*apitest: kit_pickup #1 GermanAssault \\(pickup\\)[^~]*#1 Bob [^\n]*kit GermanAssault[^~]*apitest: keeping #0 out of willys[^~]*#0 kept out of willys[^~]*apitest: entered #0 t34 seat 0 player \\(player now in t34 seat 0\\)[^~]*Gave Rus_AT to 0 of 1 player\\(s\\) \\(dead, in a vehicle[^~]*Score set for 1 of 1[^~]*#1 Bob +team 2 score 42[^~]*apitest: round_end winner 2[^~]*Round ended\\.[^~]*> rmm endround 1\n[^~]*refused"
        FAIL_REGULAR_EXPRESSION "crashed in|Failed to load|Load failed|SIMULATED CRASH|spawn kit for #0")
    # Vehicles: enter/exit, seats, reasons, blockable exits and seat switches.
    add_test(NAME sim_vehicles
        COMMAND rmm_simhost --root ${RMM_DIST} --game bf1942 --script ${CMAKE_CURRENT_SOURCE_DIR}/tests/vehicles.rmmsim
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(sim_vehicles PROPERTIES
        PASS_REGULAR_EXPRESSION "put_in_vehicle +yes[^~]*vehicle_enter +supported blockable\n[^~]*vehicle_exit +supported blockable[^~]*0x2 T34_gunner +seat 1[^~]*0x3 T34_mg +seat 2[^~]*apitest: entered #0 T34 seat 0 plugin \\(player now in T34 seat 0\\)[^~]*Put in T34 0x1\\.[^~]*apitest: entered #1 T34_mg seat 2 plugin[^~]*> rmm putin Bob 0x1 0\n[^~]*Refused[^~]*seat 1, free[^~]*seat 2, taken by #1[^~]*apitest: keeping #0 in T34 \\(player\\)[^~]*#0 kept in T34[^~]*apitest: keeping #0 in T34 \\(switch\\)[^~]*#0 Alice [^\n]* in T34[^~]*apitest: exited #0 T34 seat 0 switch[^~]*apitest: entered #0 T34_gunner seat 1 switch \\(player now in T34_gunner seat 1\\)[^~]*apitest: exited #0 T34_gunner seat 1 player \\(player now in nothing\\)[^~]*apitest: exited #1 T34_mg seat 2 game[^~]*Took 0 of 1[^~]*apitest: entered #0 T34_gunner seat 1 plugin[^~]*apitest: exited #0 T34_gunner seat 1 plugin[^~]*Took 1 of 1[^~]*> rmm putin Alice 0x1 7\n[^~]*Refused"
        FAIL_REGULAR_EXPRESSION "crashed in|Failed to load|Load failed|SIMULATED CRASH|keeping #1 in")
    # Gun game. The test ladder is a per-map file for "gg_sim", so the plugin stays idle in other tests.
    file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/tests/gungame/gg_sim.cfg DESTINATION ${RMM_DIST}/configs/gungame/maps)
    add_test(NAME sim_gungame
        COMMAND rmm_simhost --root ${RMM_DIST} --game bf1942 --script ${CMAKE_CURRENT_SOURCE_DIR}/tests/gungame.rmmsim)
    set_tests_properties(sim_gungame PROPERTIES
        PASS_REGULAR_EXPRESSION "object_damage +yes[^~]*nosuchkit\\) skipped[^~]*4 level\\(s\\)[^~]*1\\. Rus_AT \\(1 kill, PTRS\\*,AT_Rocket\\)\n2\\. T-34 \\[vehicle\\][^~]*> \\.kill 1 0 Colt\n[^~]*\\(to #0\\) \\[GG\\] Only kills with the Rus_AT count\\.\n> \\.spawn 1[^~]*> \\.kill 1 0 PTRS_Projectile\n[^~]*\\(to #0\\) \\[GG\\] Level 2/4: T-34[^~]*#0 in T34 seat 0[^~]*#0 Alice +team 1 score 2 [^\n]*alive in T34[^~]*You can't leave your vehicle[^~]*#0 kept in T34[^~]*no other vehicles[^~]*#1 kept out of Kubelwagen[^~]*#1's pickup of German_Assault blocked[^~]*T34 0x1 explodes[^~]*#1 in T34 seat 0[^~]*Knifed by Alice - down to Level 1/4[^~]*T34 0x4 explodes[^~]*#0 in Willys seat 0[^~]*> \\.kill 1 0 Colt\n\\[game\\] \\(to #0\\) \\[GG\\] Only kills from your Willys count\\.\n> \\.enter 0 Willys\n\\[sim\\] #0 in Willys seat 0[^~]*1/2 kills for the next level[^~]*Level 4/4: Sniper[^~]*#0 out of Willys[^~]*#0 now carries German_Scout[^~]*Alice wins Gun Game![^~]*Round end requested \\(winner: team 1\\)[^~]*Level must be 1-4"
        FAIL_REGULAR_EXPRESSION "crashed in|Failed to load|Load failed|SIMULATED CRASH|Could not give you|> \\.kill 1 0 Colt\n\\[game\\] \\(to #0\\) \\[GG\\] Level")
    # Items: get/select/remove, the blockable weapon switch, and gungame.lockWeapons.
    add_test(NAME sim_items
        COMMAND rmm_simhost --root ${RMM_DIST} --game bf1942 --script ${CMAKE_CURRENT_SOURCE_DIR}/tests/items.rmmsim)
    set_tests_properties(sim_items PROPERTIES
        PASS_REGULAR_EXPRESSION "items +yes[^~]*player_weapon_switch +supported blockable[^~]*Alice: 1=KnifeAllies 2=Colt \\[3=Assault_Gun\\] 4=GrenadeAllies[^~]*#0 holds 2:Colt[^~]*#0 holds 1:KnifeAllies\nLocked 1 player[^~]*Alice: \\[1=KnifeAllies\\] 2=Colt 3=Assault_Gun 4=GrenadeAllies  locked to 1 3[^~]*items lock: #0 blocked 1:KnifeAllies -> 2:Colt\n\\[sim\\] #0's switch to 2:Colt blocked[^~]*#0 holds 3:Assault_Gun[^~]*Unlocked[^~]*#0 holds 4:GrenadeAllies[^~]*#0 holds 2:Colt\nSelected slot 2 for 1 of 1[^~]*#0 holds 3:Assault_Gun\n\\[sim\\] #0 loses 2:Colt\n[^~]*Removed slot 2 for 1 of 1[^~]*Alice: 1=KnifeAllies \\[3=Assault_Gun\\] 4=GrenadeAllies[^~]*Removed slot 2 for 0 of 1[^~]*#1 now carries Rus_AT[^~]*#1 holds 5:PTRS_Rifle[^~]*\\[GG\\] This level: the Rus_AT only\\.\n\\[sim\\] #1's switch to 2:Colt blocked[^~]*#1's switch to 4:GrenadeAllies blocked[^~]*#1 holds 1:KnifeAllies[^~]*#1 holds 5:PTRS_Rifle[^~]*> gungame\\.lockWeapons 0\n> \\.weapon 1 2\n\\[sim\\] #1 holds 2:Colt"
        FAIL_REGULAR_EXPRESSION "crashed in|Failed to load|Load failed|SIMULATED CRASH")
    # Blockable map spawners and spawn timing via gun game (mapVehicles, spawn points, respawnTime).
    add_test(NAME sim_ggspawn
        COMMAND rmm_simhost --root ${RMM_DIST} --game bf1942 --script ${CMAKE_CURRENT_SOURCE_DIR}/tests/ggspawn.rmmsim)
    set_tests_properties(sim_ggspawn PROPERTIES
        PASS_REGULAR_EXPRESSION "spawn_timing +yes[^~]*spawner_spawn +supported blockable[^~]*\\[sim\\] spawn timing: delay 0\\.0, waves off, interval 20\\.0[^~]*spawner 0x10000: T34 blocked[^~]*spawner 0x10001: Boat blocked[^~]*spawned 0x1 T34 at 400\\.0,14\\.0,400\\.0 yaw 90[^~]*#0 in T34 seat 0[^~]*> \\.spawners\n\\[sim\\] vehicle 0x2 T34[^~]*vehicle 0x3 Boat[^~]*removed 2 map vehicle\\(s\\)[^~]*Alice +team 1 score 2 K/D 0/0 alive in T34[^~]*\\[sim\\] spawn timing: delay 3\\.0, waves on, interval 20\\.0"
        FAIL_REGULAR_EXPRESSION "crashed in|Failed to load|Load failed|SIMULATED CRASH|removed 0x1\n")
    # Rush: control point list/set/lock; stages from Rush_ points, a map file and the map; both endings;
    # rounds that aren't Rush (other mode, rush.maps) with the Rush points hidden.
    file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/tests/rush/rush_sim.cfg DESTINATION ${RMM_DIST}/configs/rush/maps)
    add_test(NAME sim_rush
        COMMAND rmm_simhost --root ${RMM_DIST} --game bf1942 --script ${CMAKE_CURRENT_SOURCE_DIR}/tests/rush.rmmsim)
    set_tests_properties(sim_rush PROPERTIES
        PASS_REGULAR_EXPRESSION "control_point_lock yes[^~]*Village set to team 1[^~]*rush_sim: team 2 attacks, 2 stage\\(s\\) \\([^~]*Stage 1/2: capture Village and Bridge \\(3 tickets\\)[^~]*0x20002 Village +team 1 held  at[^~]*0x20004 Factory +team 1 held LOCKED[^~]*capture of Factory by team 2 blocked \\(locked\\)[^~]*capture of AxisBase by team 2 blocked[^~]*\\(team 1\\): \\[Rush\\] Village is falling[^~]*captured Village - 1 to go[^~]*capture of Village by team 1 blocked \\(locked\\)[^~]*captured Bridge![^~]*Factory unlocked[^~]*The defenders fall back[^~]*Stage 2/2: capture Factory and Church \\(2 tickets\\)[^~]*Stage 2/2, team 2 attacks: 1 ticket\\.[^~]*The attackers took every objective! Team 2 wins[^~]*Round end requested \\(winner: team 2\\)[^~]*The attackers ran out of tickets! Team 1 wins[^~]*Round end requested \\(winner: team 1\\)[^~]*plain_sim: no Rush_ points[^~]*Rush is not running on this map[^~]*auto_sim: team 2 attacks, 2 stage\\(s\\) \\(made from the map\\)[^~]*Stage 1/2: capture Church and Factory \\(50 tickets\\)[^~]*Church set to team 2[^~]*Stage 2/2: capture Bridge and Village[^~]*conv_sim: team 2 attacks, 3 stage\\(s\\) \\(the map's Rush_ points\\)[^~]*Stage 1/3: capture Hill and Mill[^~]*captured Hill - 1 to go[^~]*Stage 2/3: capture Fort[^~]*conv_sim gpm_ctf: not Rush \\(the mode isn't in rush.modes\\)[^~]*0x20007 Rush_1_Hill +team 0 held LOCKED[^~]*capture of Rush_2_Fort by team 2 blocked[^~]*conv_sim gpm_cq: not Rush \\(the map isn't in rush.maps\\)[^~]*4 Rush point\\(s\\) neutral and locked[^~]*Rush is not running on this map"
        FAIL_REGULAR_EXPRESSION "crashed in|Failed to load|Load failed|SIMULATED CRASH|\\[rush\\] [^\n]*:[0-9]+: ")
    # Query rules: the fastdl plugin's keys in a GameSpy reply; voicechat without its library.
    add_test(NAME sim_query
        COMMAND rmm_simhost --root ${RMM_DIST} --game bf1942 --script ${CMAKE_CURRENT_SOURCE_DIR}/tests/query.rmmsim)
    set_tests_properties(sim_query PROPERTIES
        PASS_REGULAR_EXPRESSION "query_rules +yes[^~]*> \\.query\nquery reply: \\\\gamename\\\\bfield1942\\\\hostport\\\\14567\\\\final\\\\\\\\queryid\\\\1\\.1 \\(unchanged\\)\n> rmm query\nNo query rules\\.\n> fastdl\nFastDL is not advertised[^~]*> \\.query\nquery reply: \\\\gamename\\\\bfield1942\\\\hostport\\\\14567\\\\fastdlport\\\\8080\\\\fastdlpath\\\\/bf1942\\\\final\\\\\\\\queryid\\\\1\\.1\n> fastdl\nClients download from http://<this server>:8080/bf1942[^~]*query reply: \\\\hostname\\\\Test\\\\fastdlport\\\\1\\\\fastdlpath\\\\/bf1942\\\\final[^~]*query reply: \\\\hostname\\\\Test\\\\queryid\\\\3\\.1 \\(unchanged\\)[^~]*fastdlport\\\\8080\\\\fastdlhost\\\\dl\\.example\\.com\\\\fastdlpath\\\\/bf1942\\\\final[^~]*\\\\fastdlhost\\\\dl\\.example\\.com  \\(FastDL\\)[^~]*> fastdl\\.port 0\n[^~]*> \\.query\nquery reply: [^\n]*\\(unchanged\\)[^~]*Unloaded FastDL\\.\n> \\.query\nquery reply: [^\n]*\\(unchanged\\)\n> rmm query\nNo query rules\\.\n> voice\nThe voice server isn't running"
        FAIL_REGULAR_EXPRESSION "crashed in|Failed to load|Load failed|SIMULATED CRASH")
endif()

# Offline gamedata check against a real server binary (not redistributed):
#   cmake -DRMM_BF1942_LINUX_BINARY=/path/to/bf1942_lnxded.dynamic ...
set(RMM_BF1942_LINUX_BINARY "" CACHE FILEPATH "bf1942_lnxded binary for gamedata symbol tests")
if(RMM_BF1942_LINUX_BINARY)
    enable_testing()
    add_test(NAME gamedata_bf1942_linux_symbols
        COMMAND rmm_elfsym ${RMM_BF1942_LINUX_BINARY} ${CMAKE_CURRENT_SOURCE_DIR}/gamedata/bf1942.ini)
endif()
# BFV Linux server 1.21: bfv_linded.dynamic.
set(RMM_BFV_LINUX_BINARY "" CACHE FILEPATH "bfv_linded binary for gamedata symbol tests")
if(RMM_BFV_LINUX_BINARY)
    enable_testing()
    add_test(NAME gamedata_bfv_linux_symbols
        COMMAND rmm_elfsym ${RMM_BFV_LINUX_BINARY} ${CMAKE_CURRENT_SOURCE_DIR}/gamedata/bfv.ini)
endif()
# BF2 Linux server: point at bin/ia-32/bf2 or bin/amd-64/bf2 (names are identical).
set(RMM_BF2_LINUX_BINARY "" CACHE FILEPATH "bf2 Linux server binary for gamedata symbol tests")
if(RMM_BF2_LINUX_BINARY)
    enable_testing()
    add_test(NAME gamedata_bf2_linux_symbols
        COMMAND rmm_elfsym ${RMM_BF2_LINUX_BINARY} ${CMAKE_CURRENT_SOURCE_DIR}/gamedata/bf2.ini)
endif()
