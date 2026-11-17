/**
 * @file rmm_plugin.h
 * @brief What a plugin must export, and the plugin lifecycle.
 *
 * ## Lifecycle
 *  1. **RMM_Query()**  - Required. Return static info. Called before anything
 *     else; do no work here. RMM checks API version and game compatibility
 *     and refuses the plugin (without calling anything else) on mismatch.
 *  2. **RMM_Load()**   - Required. Store @p api and @p self, register hooks,
 *     commands and vars. Return RMM_FALSE and fill @p error to abort; anything
 *     you registered is cleaned up for you. @p late is true when loaded after
 *     the server was already running (players may already be connected).
 *  3. **RMM_AllLoaded()** - Optional. Every plugin in the initial batch has
 *     loaded; safe to query other plugins' interfaces.
 *  4. **RMM_Pause()**  - Optional. Notification that the plugin was
 *     (un)paused by an admin. While paused RMM does not deliver events,
 *     commands or timers to it.
 *  5. **RMM_Unload()** - Optional. Free your own resources. Hooks, commands,
 *     vars, timers and interfaces are removed automatically afterwards.
 *
 * ## Safety
 * Every call into a plugin is guarded. On Windows, a crash (access violation,
 * stack overflow, unhandled C++ exception, ...) inside a plugin callback is
 * caught, logged with the plugin name, and the plugin is put into the FAILED
 * state: all its registrations are removed and it receives no more calls. The
 * server keeps running. A faulted plugin's DLL is intentionally not unmapped
 * (its state is unknown); `rmm plugins reload` maps a fresh copy.
 */
#ifndef RMM_PLUGIN_H
#define RMM_PLUGIN_H

#include "rmm_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Plugin cannot be unloaded/reloaded at runtime (e.g. it patches memory). */
#define RMM_PLUGIN_FLAG_NO_UNLOAD 0x01u
/** Plugin cannot be paused. */
#define RMM_PLUGIN_FLAG_NO_PAUSE 0x02u

typedef struct rmm_plugin_info {
    uint32_t struct_size; /**< sizeof(rmm_plugin_info) */
    uint16_t api_major;   /**< RMM_API_VERSION_MAJOR at build time */
    uint16_t api_minor;   /**< RMM_API_VERSION_MINOR at build time */
    const char* name;     /**< Display name: "Basic Commands" */
    const char* version;  /**< "1.0.0" */
    const char* author;
    const char* description;
    const char* url;
    const char* log_tag; /**< Short tag for log lines: "basecmds" */
    uint32_t games;      /**< ::rmm_game mask; RMM_GAME_ANY for portable plugins */
    uint32_t flags;      /**< RMM_PLUGIN_FLAG_* */
} rmm_plugin_info;

/** Initialiser for rmm_plugin_info's header fields. */
#define RMM_PLUGIN_INFO_HEADER sizeof(rmm_plugin_info), RMM_API_VERSION_MAJOR, RMM_API_VERSION_MINOR

typedef const rmm_plugin_info*(RMM_CALL* rmm_query_fn)(void);
typedef rmm_bool(RMM_CALL* rmm_load_fn)(const rmm_api* api, rmm_plugin_t self, rmm_bool late, char* error,
                                        size_t error_cap);
typedef void(RMM_CALL* rmm_all_loaded_fn)(void);
typedef void(RMM_CALL* rmm_pause_fn)(rmm_bool paused);
typedef void(RMM_CALL* rmm_unload_fn)(void);

/** Use on each exported entry point. */
#define RMM_PLUGIN_EXPORT RMM_EXTERN_C RMM_DLLEXPORT

/* Exported symbol names (undecorated, cdecl). */
#define RMM_SYM_QUERY      "RMM_Query"
#define RMM_SYM_LOAD       "RMM_Load"
#define RMM_SYM_ALL_LOADED "RMM_AllLoaded"
#define RMM_SYM_PAUSE      "RMM_Pause"
#define RMM_SYM_UNLOAD     "RMM_Unload"

#ifdef __cplusplus
}
#endif

#endif /* RMM_PLUGIN_H */
