/**
 * @file rmm_game.h
 * @brief Game identification, capabilities, map control and messaging.
 *
 * ## Capabilities
 * The four Refractor games differ a lot: BF2/2142 expose a Python event API
 * but can't block chat; BF1942/BFV have no scripting at all and are driven by
 * native hooks. Rather than pretend otherwise, RMM reports what the running
 * game+backend can do via has_cap(). A function whose capability is missing
 * simply returns false. Plugins that need a capability should check it in
 * RMM_Load() and fail with a clear message.
 */
#ifndef RMM_GAME_H
#define RMM_GAME_H

#include "rmm_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum rmm_cap {
    RMM_CAP_CONSOLE_EXEC = 0, /**< Run engine console commands.              */
    RMM_CAP_CONSOLE_OUTPUT,   /**< ...and capture their output.               */
    RMM_CAP_CONSOLE_HOOK,     /**< Intercept console commands (bare RMM cmds). */
    RMM_CAP_KICK,
    RMM_CAP_ENGINE_BAN, /**< Engine-side bans (RMM bans always work).   */
    RMM_CAP_KILL_PLAYER,
    RMM_CAP_SET_TEAM,
    RMM_CAP_PRIVATE_MESSAGE,
    RMM_CAP_SET_SCORE,
    RMM_CAP_TELEPORT,
    RMM_CAP_CHANGE_MAP,
    RMM_CAP_MAPLIST,
    RMM_CAP_TICKETS,
    RMM_CAP_PLAYER_POSITION,
    RMM_CAP_AUTH_ID, /**< Stable player identity is available.      */
    RMM_CAP_BOTS,    /**< Bot/AI controls via settings.             */
    RMM_CAP_CENTER_MESSAGE,
    RMM_CAP_DAMAGE_PLAYER,   /**< rmm_players_api::damage                    */
    RMM_CAP_PLAYER_HEALTH,   /**< rmm_players_api::get_health                */
    RMM_CAP_WORLD_OBJECTS,   /**< rmm_world_api find/info/occupants          */
    RMM_CAP_OBJECT_MOVE,     /**< rmm_world_api::set_position                */
    RMM_CAP_OBJECT_ROTATION, /**< rmm_world_api::set_rotation (info.rotation where reported; not BF1942) */
    RMM_CAP_OBJECT_VELOCITY, /**< rmm_world_api::set_velocity, info.velocity */
    RMM_CAP_PLAYER_PUSH,     /**< rmm_players_api::push                      */
    RMM_CAP_OBJECT_QUERY,    /**< rmm_world_api::find_objects, template_exists */
    RMM_CAP_OBJECT_SPAWN,    /**< rmm_world_api::spawn_object                */
    RMM_CAP_OBJECT_REMOVE,   /**< rmm_world_api::remove_object               */
    RMM_CAP_GROUND_HEIGHT,   /**< rmm_world_api::ground_height               */

    /* ---- bots (rmm_bots.h; also check rmm_bots_api::available) -------------- */
    RMM_CAP_BOT_QUERY,    /**< rmm_bots_api list/get_info/behaviours      */
    RMM_CAP_BOT_ADD,      /**< rmm_bots_api::add                          */
    RMM_CAP_BOT_REMOVE,   /**< rmm_bots_api::remove/remove_all            */
    RMM_CAP_BOT_ORDERS,   /**< rmm_bots_api move_to/patrol/order          */
    RMM_CAP_BOT_STRATEGY, /**< rmm_bots_api get/set_strategy, strategies  */
    RMM_CAP_BOT_SKILL,    /**< rmm_bots_api get/set_skill                 */
    RMM_CAP_BOT_INPUT,    /**< rmm_bots_api set_control/set_input/get_input */

    /* ---- players, world and round (continued) -------------------------------- */
    RMM_CAP_SET_KIT,            /**< rmm_players_api::set_kit                   */
    RMM_CAP_END_ROUND,          /**< rmm_game_api::end_round                    */
    RMM_CAP_PUT_IN_VEHICLE,     /**< rmm_players_api::enter_vehicle / exit_vehicle */
    RMM_CAP_OBJECT_DAMAGE,      /**< rmm_world_api::damage_object               */
    RMM_CAP_ITEMS,              /**< rmm_players_api::get_items / select_item / remove_item */
    RMM_CAP_SPAWN_TIMING,       /**< rmm_game_api::get_spawn_timing / set_spawn_timing */
    RMM_CAP_CONTROL_POINTS,     /**< rmm_world_api::control_points / get_control_point */
    RMM_CAP_CONTROL_POINT_SET,  /**< rmm_world_api::set_control_point */
    RMM_CAP_CONTROL_POINT_LOCK, /**< rmm_world_api::lock_control_point */

    RMM_CAP_COUNT
} rmm_cap;

typedef struct rmm_map_info {
    uint32_t struct_size;
    char map[128];
    char mode[64];
    int32_t size; /**< 16/32/64 on BF2/2142, 0 elsewhere. */
} rmm_map_info;

/** How soon dead players may spawn again (RMM_CAP_SPAWN_TIMING). */
typedef struct rmm_spawn_timing {
    uint32_t struct_size;
    /** Seconds a dead player waits before they may spawn, when waves are off. */
    float delay;
    /** Spawn waves: dead players wait for the next wave instead of the delay. */
    rmm_bool waves;
    /** Seconds between two waves. */
    float wave_interval;
} rmm_spawn_timing;

typedef struct rmm_game_api {
    uint32_t struct_size;

    /* ---- identification --------------------------------------------------- */
    int32_t(RMM_CALL* id)(void);             /**< ::rmm_game (single bit). */
    const char*(RMM_CALL* name)(void);       /**< "Battlefield 2" */
    const char*(RMM_CALL* short_name)(void); /**< "bf2" - matches gamedata file. */
    const char*(RMM_CALL* mod)(void);        /**< Active mod, e.g. "bf2", "pr", "DesertCombat". */
    const char*(RMM_CALL* backend)(void);    /**< "python-bridge", "native", "sim". */
    rmm_bool(RMM_CALL* has_cap)(int32_t cap);
    double(RMM_CALL* time)(void);    /**< Seconds since RMM start. */
    int32_t(RMM_CALL* status)(void); /**< ::rmm_game_status */

    /* ---- maps ------------------------------------------------------------- */
    rmm_bool(RMM_CALL* current_map)(rmm_map_info* out);
    /** Change level now. @p mode/@p size may be NULL/0 to keep current. */
    rmm_bool(RMM_CALL* change_map)(const char* map, const char* mode, int32_t size);
    rmm_bool(RMM_CALL* restart_map)(void);
    rmm_bool(RMM_CALL* run_next_map)(void);
    int32_t(RMM_CALL* maplist_count)(void);
    rmm_bool(RMM_CALL* maplist_get)(int32_t index, rmm_map_info* out);
    rmm_bool(RMM_CALL* maplist_append)(const char* map, const char* mode, int32_t size);
    rmm_bool(RMM_CALL* maplist_clear)(void);
    rmm_bool(RMM_CALL* set_next_map)(int32_t maplist_index);

    /* ---- messaging -------------------------------------------------------- */
    rmm_bool(RMM_CALL* say_all)(const char* message);
    rmm_bool(RMM_CALL* say_team)(int32_t team, const char* message);
    rmm_bool(RMM_CALL* center_message)(const char* message); /**< RMM_CAP_CENTER_MESSAGE */

    /* ---- round state ------------------------------------------------------ */
    int32_t(RMM_CALL* get_tickets)(int32_t team);
    rmm_bool(RMM_CALL* set_tickets)(int32_t team, int32_t tickets);

    /* ---- objects ---------------------------------------------------------- */
    /** Template name of an object (vehicle etc.). */
    rmm_bool(RMM_CALL* object_template)(rmm_object_t obj, char* out, size_t cap);

    /* ---- paced messaging ------------------------------------------------- */

    /** say_all / say_team with pacing options (see ::rmm_msg_opts; NULL =
         *  defaults). say_all/say_team themselves are paced with the defaults.
         *  All four return true once the line is sent or queued. */
    rmm_bool(RMM_CALL* say_all_ex)(const char* message, const rmm_msg_opts* opts);
    rmm_bool(RMM_CALL* say_team_ex)(int32_t team, const char* message, const rmm_msg_opts* opts);

    /* ---- round control ---------------------------------------------------- */

    /**
         * End the round now, as if the game's own victory condition had been met:
         * the end-of-round screen shows @p winner, then the server moves on as it
         * normally does after a round (next round or next map). @p winner is team
         * 1 or 2, or RMM_TEAM_NONE (0) for a draw. ::RMM_EV_ROUND_END follows with
         * that winner. Fails when no round is being played. RMM_CAP_END_ROUND.
         */
    rmm_bool(RMM_CALL* end_round)(int32_t winner);

    /**
         * Read or change how soon dead players spawn again, for the running round.
         * The game's own settings (BF1942: game.serverSpawnTime/serverSpawnDelay)
         * are applied when a level loads, so set this again after RMM_EV_ROUND_START.
         * Players already waiting keep their countdown. RMM_CAP_SPAWN_TIMING.
         */
    rmm_bool(RMM_CALL* get_spawn_timing)(rmm_spawn_timing* out);
    rmm_bool(RMM_CALL* set_spawn_timing)(const rmm_spawn_timing* timing);
} rmm_game_api;

#ifdef __cplusplus
}
#endif

#endif /* RMM_GAME_H */
