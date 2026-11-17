/**
 * @file rmm_types.h
 * @brief Fundamental types shared by every part of the Refractor MultiMod SDK.
 *
 * The RMM plugin ABI is pure C. Plugins never link against RMM; they receive
 * a table of function pointers (::rmm_api) at load time. This keeps plugins
 * compiler-agnostic (MSVC, MinGW, GCC, Clang) and lets the core evolve without
 * breaking already-compiled plugins.
 *
 * ABI rules (apply to every struct in the SDK):
 *  - Every table/struct that may grow starts with `uint32_t struct_size`.
 *  - New members are only ever appended. Use ::RMM_HAS to test for them.
 *  - Strings are UTF-8 (in practice: whatever the engine gives us; Refractor
 *    is mostly Latin-1) and NUL terminated.
 *  - Strings returned by RMM are only valid until the next call into RMM
 *    unless documented otherwise. Copy them if you need to keep them.
 *  - All functions are called on the game thread. RMM is not thread-safe.
 */
#ifndef RMM_TYPES_H
#define RMM_TYPES_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
#    define RMM_EXTERN_C extern "C"
#else
#    define RMM_EXTERN_C
#endif

#if defined(_WIN32)
#    define RMM_CALL      __cdecl
#    define RMM_DLLEXPORT __declspec(dllexport)
#else
#    define RMM_CALL
#    define RMM_DLLEXPORT __attribute__((visibility("default")))
#endif

/** Major version: bumped on any incompatible change. Plugins built against a
 *  different major version are refused. */
#define RMM_API_VERSION_MAJOR 1
/** Minor version: bumped when members are appended to tables. Plugins built
 *  against a *newer* minor than the running core are refused. */
#define RMM_API_VERSION_MINOR 0
/** Patch version: documentation and behaviour fixes with no ABI change. */
#define RMM_API_VERSION_PATCH 0

/**
 * Test whether an RMM table is large enough to contain @p member and that the
 * member is non-NULL. Use this for optional members, which a game backend may
 * leave unset, and for anything added after the API version you target.
 *
 * @code
 * if (RMM_HAS(api->players, rmm_players_api, teleport)) { ... }
 * @endcode
 */
#define RMM_HAS(ptr, type, member)                                                                                     \
    ((ptr) != NULL && (ptr)->struct_size >= (offsetof(type, member) + sizeof(((type*)0)->member)) &&                   \
     (ptr)->member != NULL)

typedef int32_t rmm_bool;
#define RMM_TRUE  1
#define RMM_FALSE 0

/** Opaque handle identifying a loaded plugin. Passed to the plugin in
 *  RMM_Load() and must be passed back to any function that registers
 *  something, so RMM can clean it up automatically on unload. */
typedef struct rmm_plugin_s* rmm_plugin_t;

/** Generic handle for hooks, commands, variables and timers. 0 = invalid. */
typedef uint32_t rmm_handle_t;
#define RMM_INVALID_HANDLE 0u

/** Player slot/index as used by the engine. */
typedef int32_t rmm_player_t;
/** "No player" - also used to mean the server console / RCon. */
#define RMM_PLAYER_NONE (-1)
/** Returned by lookups that matched more than one player. */
#define RMM_PLAYER_AMBIGUOUS (-2)

/** Opaque engine object reference (vehicle, control point, ...). 0 = none.
 *  Handles from events, rmm_player_info::vehicle and rmm_world_api are
 *  interchangeable. They may outlive the object: always check
 *  rmm_world_api::is_valid (or a call's return value) before relying on one. */
typedef uint64_t rmm_object_t;

typedef struct rmm_vec3 {
    float x, y, z;
} rmm_vec3;

/** Supported games. Also used as a bitmask in rmm_plugin_info::games. */
typedef enum rmm_game {
    RMM_GAME_UNKNOWN = 0,
    RMM_GAME_BF1942 = 1 << 0, /**< Battlefield 1942 (Refractor 1)      */
    RMM_GAME_BFV = 1 << 1,    /**< Battlefield Vietnam (Refractor 1.5) */
    RMM_GAME_BF2 = 1 << 2,    /**< Battlefield 2 (Refractor 2)         */
    RMM_GAME_BF2142 = 1 << 3, /**< Battlefield 2142 (Refractor 2)      */
    RMM_GAME_SIM = 1 << 15,   /**< rmm_simhost test harness            */

    RMM_GAME_REFRACTOR1 = RMM_GAME_BF1942 | RMM_GAME_BFV,
    RMM_GAME_REFRACTOR2 = RMM_GAME_BF2 | RMM_GAME_BF2142,
    RMM_GAME_ANY = 0xFFFF
} rmm_game;

/**
 * Return value of event handlers (modelled on Metamod's META_RES and
 * SourceMod's Action). The highest value returned by any handler wins.
 */
typedef enum rmm_result {
    RMM_CONTINUE = 0, /**< Not interested / observed only.                        */
    RMM_CHANGED = 1,  /**< Handler modified the (mutable) event data.             */
    RMM_HANDLED = 2,  /**< Block the engine action; other handlers still run.     */
    RMM_STOP = 3      /**< Block the engine action and skip remaining PRE hooks. */
} rmm_result;

/** Log levels for rmm_core_api::log. */
typedef enum rmm_log_level { RMM_LOG_DEBUG = 0, RMM_LOG_INFO = 1, RMM_LOG_WARN = 2, RMM_LOG_ERROR = 3 } rmm_log_level;

/** Team numbers are passed through from the engine. 0 is "none/spectator"
 *  on every supported game; 1 and 2 are the two playable teams. */
#define RMM_TEAM_NONE 0

/**
 * Pacing options for in-game chat output: rmm_cmds_api::reply_ex,
 * rmm_players_api::message_ex, rmm_game_api::say_all_ex / say_team_ex.
 *
 * Refractor shows only a few chat lines, briefly, so RMM queues chat per
 * destination (everyone, one team, one player) and releases one line per
 * interval. Lines to the same destination always arrive in order. A line to
 * an idle destination is sent at once; only bursts are spread out.
 *
 * Passing NULL (or using the non-_ex functions) gives the defaults:
 * interval = the `rmm.chatInterval` variable (1.0 s unless changed).
 * Console and RCon replies are never paced.
 *
 * @code
 * rmm_msg_opts o = RMM_MSG_OPTS_INIT;
 * o.interval = 2.5f;                       // give each line 2.5 s
 * api->cmds->reply_ex(ctx, "line", &o);
 * @endcode
 */
typedef struct rmm_msg_opts {
    uint32_t struct_size; /**< sizeof(rmm_msg_opts). */
    /** Seconds before the *next* line to the same destination may be shown.
     *  < 0 = rmm.chatInterval; 0 = no gap after this line. */
    float interval;
    uint32_t flags; /**< RMM_MSG_* */
} rmm_msg_opts;

/** Send now, ahead of anything already queued for the destination (urgent
 *  notices). The destination's pacing is not affected. */
#define RMM_MSG_IMMEDIATE 0x1u

#define RMM_MSG_OPTS_INIT {(uint32_t)sizeof(rmm_msg_opts), -1.0f, 0u}

#endif /* RMM_TYPES_H */
