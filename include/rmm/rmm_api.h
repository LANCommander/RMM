/**
 * @file rmm_api.h
 * @brief The root API table handed to every plugin, plus core services
 *        (logging, paths, timers, inter-plugin interfaces).
 */
#ifndef RMM_API_H
#define RMM_API_H

#include "rmm_types.h"
#include "rmm_events.h"
#include "rmm_vars.h"
#include "rmm_commands.h"
#include "rmm_players.h"
#include "rmm_admin.h"
#include "rmm_game.h"
#include "rmm_world.h"
#include "rmm_bots.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Directories for rmm_core_api::build_path. All live under the RMM root
     *  (default: "<server>/rmm"). */
typedef enum rmm_path_kind {
    RMM_PATH_ROOT = 0,
    RMM_PATH_PLUGINS = 1,
    RMM_PATH_CONFIGS = 2,
    RMM_PATH_DATA = 3, /**< Persistent plugin data. */
    RMM_PATH_LOGS = 4,
    RMM_PATH_GAMEDATA = 5
} rmm_path_kind;

#define RMM_TIMER_REPEAT     0x01u /**< Fire every interval until killed.          */
#define RMM_TIMER_MAP_CHANGE 0x02u /**< Kill automatically on map load.            */

/** Timer callback. Return RMM_FALSE from a repeating timer to stop it. */
typedef rmm_bool(RMM_CALL* rmm_timer_cb)(rmm_handle_t timer, void* user);

typedef struct rmm_core_api {
    uint32_t struct_size;

    /** RMM version string, e.g. "1.0.0". */
    const char*(RMM_CALL* version)(void);

    /** Write a line to the RMM log (and server console), tagged with the
         *  plugin's log tag. Format with snprintf yourself or use rmm_logf(). */
    void(RMM_CALL* log)(rmm_plugin_t self, int32_t level, const char* message);

    /** Build "<rmm root>/<kind dir>/<relative>". @return full length. */
    size_t(RMM_CALL* build_path)(int32_t kind, const char* relative, char* out, size_t cap);

    /* ---- plugins -------------------------------------------------------- */
    const char*(RMM_CALL* plugin_name)(rmm_plugin_t plugin);
    rmm_plugin_t(RMM_CALL* find_plugin)(const char* name_or_file);
    /** Ask to be unloaded at the end of the current frame (e.g. on fatal
         *  config error). */
    void(RMM_CALL* request_unload)(rmm_plugin_t self, const char* reason);

    /* ---- timers ---------------------------------------------------------- */
    rmm_handle_t(RMM_CALL* timer_create)(rmm_plugin_t self, double interval_sec, uint32_t flags, rmm_timer_cb cb,
                                         void* user);
    rmm_bool(RMM_CALL* timer_kill)(rmm_plugin_t self, rmm_handle_t timer);

    /* ---- inter-plugin interfaces (Metamod-style factories) -------------- */
    /**
         * Publish an interface (usually a struct of function pointers) so other
         * plugins can call you. Removed automatically when you unload; consumers
         * get RMM_EV_PLUGIN_UNLOADED and must drop the pointer.
         */
    rmm_bool(RMM_CALL* iface_register)(rmm_plugin_t self, const char* name, uint32_t version, void* iface);
    /** @return the interface if registered with version >= @p min_version. */
    void*(RMM_CALL* iface_query)(const char* name, uint32_t min_version, uint32_t* out_version);
} rmm_core_api;

/** Root table passed to RMM_Load(). Valid for the plugin's whole lifetime. */
typedef struct rmm_api {
    uint32_t struct_size;
    uint16_t version_major;
    uint16_t version_minor;

    const rmm_core_api* core;
    const rmm_events_api* events;
    const rmm_vars_api* vars;
    const rmm_cmds_api* cmds;
    const rmm_players_api* players;
    const rmm_admin_api* admin;
    const rmm_game_api* game;
    /** Optional; check with RMM_HAS(api, rmm_api, world). */
    const rmm_world_api* world;
    /** Optional; check with RMM_HAS(api, rmm_api, bots). */
    const rmm_bots_api* bots;
} rmm_api;

#ifdef __cplusplus
}
#endif

#endif /* RMM_API_H */
