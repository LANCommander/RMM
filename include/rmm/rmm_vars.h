/**
 * @file rmm_vars.h
 * @brief Engine variables, portable game settings, and RMM variables (cvars).
 *
 * Refractor has no cvar system as such. Instead its console exposes
 * `object.property` commands: running `game.serverName` prints the value and
 * `game.serverName "My Server"` sets it (BF2/2142 use `sv.serverName`). RMM
 * exposes three layers on top of that:
 *
 *  1. **Engine variables** - raw `object.property` access via the console,
 *     e.g. `engine_get("sv.timeLimit", buf, sizeof buf)`.
 *  2. **Settings** - a portable, game-independent id (::rmm_setting) that RMM
 *     maps to the right engine variable for the running game using gamedata.
 *     A plugin that sets RMM_SETTING_GRAVITY works on every game that has it.
 *  3. **RMM variables** - plugin-owned variables (the ConVar equivalent) with
 *     defaults, bounds, change hooks and persistence. They use the same
 *     `object.property` naming, e.g. `rmm.chatTrigger`, `afk.kickTime`, and
 *     can be read/set from the console via `rmm var <name> [value]`, from
 *     plugin config files, or (where the adapter supports it) directly.
 */
#ifndef RMM_VARS_H
#define RMM_VARS_H

#include "rmm_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
     * Portable server settings. Mapping to engine variables lives in
     * rmm/gamedata/<game>.ini [settings] so new games/patches need no recompile.
     * Use rmm_vars_api::setting_var() to see whether the running game has one.
     */
typedef enum rmm_setting {
    RMM_SETTING_SERVER_NAME = 0,
    RMM_SETTING_PASSWORD,
    RMM_SETTING_MAX_PLAYERS,
    RMM_SETTING_TIME_LIMIT,
    RMM_SETTING_SCORE_LIMIT,
    RMM_SETTING_TICKET_RATIO,
    RMM_SETTING_ROUNDS_PER_MAP,
    RMM_SETTING_SPAWN_TIME,
    RMM_SETTING_MAN_DOWN_TIME,
    RMM_SETTING_FRIENDLY_FIRE, /**< On-foot friendly fire on/off or %. */
    RMM_SETTING_VEHICLE_FF,    /**< Vehicle friendly fire. */
    RMM_SETTING_TEAM_RATIO,    /**< Allowed team imbalance %. */
    RMM_SETTING_AUTOBALANCE,
    RMM_SETTING_GRAVITY,
    RMM_SETTING_GAME_SPEED, /**< Simulation speed / time scale. */
    RMM_SETTING_BOT_COUNT,  /**< Number of bots (coop). */
    RMM_SETTING_BOT_RATIO,  /**< Bot ratio % (BF2/2142 coop). */
    RMM_SETTING_BOT_SKILL,  /**< 0.0 - 1.0 */
    RMM_SETTING_WELCOME_MESSAGE,
    RMM_SETTING_VOIP_ENABLED,
    RMM_SETTING_PUNKBUSTER,
    RMM_SETTING_ALLOW_NOSE_CAM,
    RMM_SETTING_EXTERNAL_VIEWS,
    RMM_SETTING_KICK_TK_COUNT, /**< Teamkills before auto-kick. */

    RMM_SETTING_COUNT
} rmm_setting;

/** Flags for RMM variables. */
#define RMM_VAR_ARCHIVE   0x01u /**< Persist value in rmm/configs/rmm_vars.cfg.  */
#define RMM_VAR_PROTECTED 0x02u /**< Value hidden from non-root RCon/chat queries (passwords). */
#define RMM_VAR_READONLY  0x04u /**< Only the owning plugin can change it.      */
#define RMM_VAR_NOTIFY    0x08u /**< Announce changes in chat.                   */
#define RMM_VAR_NUMERIC   0x10u /**< Reject non-numeric values.                  */

/** Called after an RMM var changed. Values are only valid during the call. */
typedef void(RMM_CALL* rmm_var_change_cb)(rmm_handle_t var, const char* old_value, const char* new_value, void* user);

typedef struct rmm_vars_api {
    uint32_t struct_size;

    /* ---- engine variables ---------------------------------------------- */

    /** Read an engine `object.property`. @return false if the game does not
         *  know it or the console is unavailable. */
    rmm_bool(RMM_CALL* engine_get)(const char* name, char* out, size_t cap);
    /** Set an engine `object.property` (value is quoted automatically). */
    rmm_bool(RMM_CALL* engine_set)(const char* name, const char* value);
    /** Fire RMM_EV_VAR_CHANGED when this engine var changes (polled or hooked
         *  depending on the game). */
    rmm_bool(RMM_CALL* engine_watch)(rmm_plugin_t self, const char* name);

    /* ---- portable settings --------------------------------------------- */

    /** Engine variable backing @p setting on this game, or NULL if unsupported. */
    const char*(RMM_CALL* setting_var)(int32_t setting);
    rmm_bool(RMM_CALL* setting_get)(int32_t setting, char* out, size_t cap);
    rmm_bool(RMM_CALL* setting_set)(int32_t setting, const char* value);

    /* ---- RMM variables -------------------------------------------------- */

    /**
         * Create (or adopt) an RMM variable. If a persisted/config value exists it
         * is applied immediately. If the name exists and is owned by another
         * plugin, creation fails.
         */
    rmm_handle_t(RMM_CALL* create)(rmm_plugin_t self, const char* name, const char* def_value, const char* description,
                                   uint32_t flags);
    rmm_handle_t(RMM_CALL* find)(const char* name);
    rmm_bool(RMM_CALL* set_bounds)(rmm_handle_t var, rmm_bool has_min, double min_value, rmm_bool has_max,
                                   double max_value);

    /** Copy value into @p out. @return length of the full value. */
    size_t(RMM_CALL* get_string)(rmm_handle_t var, char* out, size_t cap);
    int32_t(RMM_CALL* get_int)(rmm_handle_t var);
    double(RMM_CALL* get_float)(rmm_handle_t var);
    rmm_bool(RMM_CALL* get_bool)(rmm_handle_t var);
    /** @return false if rejected (bounds, NUMERIC, READONLY). */
    rmm_bool(RMM_CALL* set_string)(rmm_plugin_t self, rmm_handle_t var, const char* value);
    rmm_bool(RMM_CALL* reset)(rmm_plugin_t self, rmm_handle_t var);

    rmm_handle_t(RMM_CALL* hook_change)(rmm_plugin_t self, rmm_handle_t var, rmm_var_change_cb cb, void* user);
    rmm_bool(RMM_CALL* unhook_change)(rmm_plugin_t self, rmm_handle_t hook);

    /* ---- convenience ---------------------------------------------------- */

    /** Read any variable by name: RMM vars first, then engine vars. */
    rmm_bool(RMM_CALL* get)(const char* name, char* out, size_t cap);
    /** Write any variable by name: RMM vars first, then engine vars. */
    rmm_bool(RMM_CALL* set)(rmm_plugin_t self, const char* name, const char* value);
} rmm_vars_api;

#ifdef __cplusplus
}
#endif

#endif /* RMM_VARS_H */
