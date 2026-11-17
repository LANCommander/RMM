/**
 * @file rmm_events.h
 * @brief Event system: hook game events, block/modify them, define your own.
 *
 * ## Model
 * Every event is dispatched in two phases:
 *  - **PRE**  - before the engine acts. Handlers may return ::RMM_HANDLED or
 *               ::RMM_STOP to block the action (only if the event is
 *               ::RMM_EVF_BLOCKABLE on the running game) or modify the payload
 *               (only if ::RMM_EVF_MUTABLE) and return ::RMM_CHANGED.
 *  - **POST** - after the action happened. Return values are ignored. POST
 *               handlers are *not* called if a PRE handler blocked the event.
 *
 * Within a phase, handlers run in descending priority, then in registration
 * order. Not every game can produce every event, and not every game can block
 * them - always check rmm_events_api::flags() if your plugin depends on it.
 *
 * Events that can only be observed after the fact (e.g. most Battlefield 2
 * events, which come from the game's Python layer) are delivered in *both*
 * phases, but are never BLOCKABLE.
 */
#ifndef RMM_EVENTS_H
#define RMM_EVENTS_H

#include "rmm_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Built-in event identifiers. The payload type is listed for each. */
typedef enum rmm_event_id {
    RMM_EV_NONE = 0,

    /* ---- server / round lifecycle ------------------------------------- */
    RMM_EV_SERVER_START = 1,    /**< NULL. Core attached, all plugins loaded. */
    RMM_EV_SERVER_SHUTDOWN = 2, /**< NULL. */
    RMM_EV_FRAME = 3,           /**< ::rmm_ev_frame. Server tick (rate is game dependent). */
    RMM_EV_MAP_LOAD = 4,        /**< ::rmm_ev_map. A level is being loaded. */
    RMM_EV_ROUND_START = 5,     /**< ::rmm_ev_map. */
    RMM_EV_ROUND_END = 6,       /**< ::rmm_ev_round_end. */
    RMM_EV_GAME_STATUS = 7,     /**< ::rmm_ev_game_status. Pre-game/playing/end-game/paused. */

    /* ---- players ------------------------------------------------------- */
    RMM_EV_PLAYER_CONNECT = 20,    /**< ::rmm_ev_player_connect. BLOCKABLE where supported (rejects player). */
    RMM_EV_PLAYER_AUTH = 21,       /**< ::rmm_ev_player. Player's auth_id (BF2 profile / R1 CD-key hash) is known. */
    RMM_EV_PLAYER_DISCONNECT = 22, /**< ::rmm_ev_player. */
    RMM_EV_PLAYER_SPAWN = 23,      /**< ::rmm_ev_player_spawn. */
    RMM_EV_PLAYER_KILLED = 24,     /**< ::rmm_ev_player_killed. */
    RMM_EV_PLAYER_DAMAGE = 25,     /**< ::rmm_ev_player_damage. MUTABLE/BLOCKABLE where supported. */
    RMM_EV_PLAYER_TEAM = 26,       /**< ::rmm_ev_player_change. Team change. */
    RMM_EV_PLAYER_SQUAD = 27,      /**< ::rmm_ev_player_change. Squad change (BF2/2142). */
    RMM_EV_PLAYER_SCORE = 28,      /**< ::rmm_ev_player_score. */
    RMM_EV_PLAYER_KIT_PICKUP =
        29, /**< ::rmm_ev_player_kit. BLOCKABLE where supported, for reason ::RMM_KIT_REASON_PICKUP only. */
    RMM_EV_PLAYER_KIT_DROP = 30, /**< ::rmm_ev_player_kit. */
    RMM_EV_PLAYER_REVIVED = 31,  /**< ::rmm_ev_player_killed (victim = revived, attacker = medic). */
    RMM_EV_PLAYER_WEAPON_SWITCH =
        32, /**< ::rmm_ev_weapon_switch. BLOCKABLE where supported, for reasons PLAYER and NETWORK. */

    /* ---- communication & administration -------------------------------- */
    RMM_EV_CHAT = 40,         /**< ::rmm_ev_chat. BLOCKABLE/MUTABLE where supported. */
    RMM_EV_RCON_COMMAND = 41, /**< ::rmm_ev_rcon. A console/RCon command is executed. */
    RMM_EV_VAR_CHANGED = 42,  /**< ::rmm_ev_var_changed. RMM var or watched engine var changed. */

    /* ---- vehicles & world --------------------------------------------- */
    RMM_EV_VEHICLE_ENTER = 60, /**< ::rmm_ev_vehicle. BLOCKABLE where supported (keeps the player out), not for
                                      reasons SWITCH/PLUGIN. */
    RMM_EV_VEHICLE_EXIT = 61,  /**< ::rmm_ev_vehicle. BLOCKABLE where supported, for reasons PLAYER and SWITCH only. */
    RMM_EV_VEHICLE_DESTROYED = 62, /**< ::rmm_ev_vehicle (player = attacker or NONE). */
    RMM_EV_CONTROL_POINT = 63,     /**< ::rmm_ev_control_point. Flag captured/neutralised. */
    RMM_EV_VEHICLE_DAMAGE = 64,    /**< ::rmm_ev_vehicle_damage. MUTABLE/BLOCKABLE where supported. */
    RMM_EV_OBJECT_DESTROYED = 65,  /**< ::rmm_ev_object_destroyed. Non-vehicle world object. */
    RMM_EV_SPAWNER_SPAWN = 66,     /**< ::rmm_ev_spawner. A map spawner creates its object (vehicle, gun...).
                                            BLOCKABLE where supported: the spawner tries again later. */

    /* ---- bots (see rmm_bots.h) ----------------------------------------- */
    RMM_EV_BOT_ADDED = 70,      /**< ::rmm_ev_bot. A bot joined the AI (any cause). */
    RMM_EV_BOT_REMOVED = 71,    /**< ::rmm_ev_bot. A bot left the AI (removed, kicked, round over). */
    RMM_EV_BOT_ADD_FAILED = 72, /**< ::rmm_ev_bot. An rmm_bots_api::add() request gave no bot (player = NONE). */

    /* ---- framework ---------------------------------------------------- */
    RMM_EV_PLUGIN_LOADED = 90,   /**< ::rmm_ev_plugin. */
    RMM_EV_PLUGIN_UNLOADED = 91, /**< ::rmm_ev_plugin. Drop any interface pointers you got from it. */

    /** Custom events created with rmm_events_api::register_custom start here. */
    RMM_EV_CUSTOM_BASE = 1000
} rmm_event_id;

typedef enum rmm_event_phase { RMM_PHASE_PRE = 0, RMM_PHASE_POST = 1 } rmm_event_phase;

/** Capability flags returned by rmm_events_api::flags(). */
#define RMM_EVF_SUPPORTED 0x01u /**< The running game can produce this event.      */
#define RMM_EVF_BLOCKABLE 0x02u /**< PRE handlers can block the engine action.      */
#define RMM_EVF_MUTABLE   0x04u /**< PRE handlers can modify payload fields marked [mutable]. */
#define RMM_EVF_CUSTOM    0x08u /**< Defined by a plugin.                           */

/** Default priority. Higher runs first. Core-internal hooks use +/-1000. */
#define RMM_PRIORITY_NORMAL 0
#define RMM_PRIORITY_HIGH   100
#define RMM_PRIORITY_LOW    (-100)

/* ---- Event payloads ---- */

typedef struct rmm_ev_frame {
    double dt;   /**< Seconds since the previous frame event. */
    double time; /**< Seconds since RMM started. */
} rmm_ev_frame;

typedef struct rmm_ev_map {
    const char* map;  /**< e.g. "strike_at_karkand", "el_alamein". */
    const char* mode; /**< e.g. "gpm_cq", "conquest". */
    const char* mod;  /**< e.g. "bf2", "bf1942", "DesertCombat". */
    int32_t size;     /**< BF2/2142 map size (16/32/64); 0 if not applicable. */
} rmm_ev_map;

typedef struct rmm_ev_round_end {
    int32_t winner; /**< Winning team, or RMM_TEAM_NONE for a draw. */
    int32_t reason; /**< Engine-specific victory type; 0 if unknown. */
} rmm_ev_round_end;

typedef enum rmm_game_status {
    RMM_STATUS_UNKNOWN = 0,
    RMM_STATUS_PREGAME = 1,
    RMM_STATUS_PLAYING = 2,
    RMM_STATUS_ENDGAME = 3,
    RMM_STATUS_PAUSED = 4,
    RMM_STATUS_LOADING = 5
} rmm_game_status;

typedef struct rmm_ev_game_status {
    int32_t old_status; /**< ::rmm_game_status */
    int32_t new_status; /**< ::rmm_game_status */
} rmm_ev_game_status;

typedef struct rmm_ev_player {
    rmm_player_t player;
} rmm_ev_player;

typedef struct rmm_ev_player_connect {
    rmm_player_t player;
    const char* name;
    const char* address; /**< "ip:port" if known, else "". */
    char* reject_reason; /**< [mutable] Message shown to a rejected player. */
    size_t reject_reason_cap;
} rmm_ev_player_connect;

typedef struct rmm_ev_player_spawn {
    rmm_player_t player;
    rmm_vec3 position;
    const char* kit; /**< Kit/soldier template if known, else "". */
} rmm_ev_player_spawn;

typedef struct rmm_ev_player_killed {
    rmm_player_t victim;
    rmm_player_t attacker; /**< RMM_PLAYER_NONE for world/suicide-by-world. */
    const char* weapon;    /**< Weapon/projectile template, "" if unknown. */
    const char* vehicle;   /**< Attacker's vehicle template, "" if on foot/unknown. */
    rmm_bool teamkill;
    rmm_bool suicide;
} rmm_ev_player_killed;

/** Damage to a player's own soldier. Hits on a vehicle the player sits in are
     *  ::RMM_EV_VEHICLE_DAMAGE instead. Damage dealt by rmm_players_api::damage
     *  does not raise this event. */
typedef struct rmm_ev_player_damage {
    rmm_player_t victim;
    rmm_player_t attacker;
    float* damage; /**< [mutable] Damage about to be applied. */
    const char* weapon;
} rmm_ev_player_damage;

typedef struct rmm_ev_player_change {
    rmm_player_t player;
    int32_t old_value; /**< Old team/squad (-1 if unknown). */
    int32_t new_value; /**< New team/squad. */
} rmm_ev_player_change;

typedef struct rmm_ev_player_score {
    rmm_player_t player;
    int32_t delta;
    int32_t total;
} rmm_ev_player_score;

/** Why a player got or lost a kit (rmm_ev_player_kit::reason). */
typedef enum rmm_kit_reason {
    RMM_KIT_REASON_PICKUP = 0, /**< Picked up from the ground (the old kit is dropped for it).
                                        The only reason a KIT_PICKUP can be blocked for. */
    RMM_KIT_REASON_SPAWN = 1,  /**< The kit chosen in the spawn menu, handed out at spawn. */
    RMM_KIT_REASON_CHANGE = 2, /**< The game swapped the kit itself (e.g. a kit change while alive). */
    RMM_KIT_REASON_PLUGIN = 3, /**< rmm_players_api::set_kit. */
    RMM_KIT_REASON_OTHER = 4   /**< Anything else, e.g. a kit dropped by a dying soldier. */
} rmm_kit_reason;

typedef struct rmm_ev_player_kit {
    rmm_player_t player;
    const char* kit;         /**< Kit template, e.g. "GermanAssault". */
    rmm_object_t kit_object; /**< The kit object (rmm_world_api handle), 0 if unknown. */
    int32_t reason;          /**< ::rmm_kit_reason. Games that can't tell report PICKUP. */
} rmm_ev_player_kit;

typedef enum rmm_chat_channel {
    RMM_CHAT_ALL = 0,
    RMM_CHAT_TEAM = 1,
    RMM_CHAT_SQUAD = 2,
    RMM_CHAT_COMMANDER = 3,
    RMM_CHAT_SERVER = 4 /**< Message originated from the server/admin. */
} rmm_chat_channel;

typedef struct rmm_ev_chat {
    rmm_player_t player; /**< Sender, RMM_PLAYER_NONE for server messages. */
    int32_t channel;     /**< ::rmm_chat_channel */
    char* text;          /**< [mutable] Message text, NUL terminated. */
    size_t text_cap;     /**< Capacity of @c text including the NUL. */
} rmm_ev_chat;

typedef enum rmm_cmd_source {
    RMM_SRC_CONSOLE = 0, /**< Local server console.                    */
    RMM_SRC_RCON = 1,    /**< Remote console (TCP RCon).               */
    RMM_SRC_INGAME = 2,  /**< In-game client RCon (e.g. BF2 "rcon ..."). */
    RMM_SRC_CHAT = 3,    /**< Chat trigger (!cmd).                     */
    RMM_SRC_PLUGIN = 4,  /**< Executed by a plugin / config file.      */
    RMM_SRC_ENGINE = 5   /**< Engine-internal (.con scripts etc).      */
} rmm_cmd_source;

typedef struct rmm_ev_rcon {
    int32_t source;      /**< ::rmm_cmd_source */
    rmm_player_t player; /**< Issuing player for IN-GAME source, else NONE. */
    const char* command; /**< Full command line. */
} rmm_ev_rcon;

typedef struct rmm_ev_var_changed {
    rmm_handle_t var; /**< RMM var handle, or 0 for an engine variable. */
    const char* name;
    const char* old_value;
    const char* new_value;
} rmm_ev_var_changed;

/** Why a player got into or out of a vehicle (rmm_ev_vehicle::reason). */
typedef enum rmm_vehicle_reason {
    RMM_VEHICLE_REASON_PLAYER = 0, /**< The player's own use key (bots too). */
    RMM_VEHICLE_REASON_SWITCH = 1, /**< Changing position inside the vehicle (F1-F8): an EXIT from
                                            the old seat, then an ENTER into the new one. Block the EXIT
                                            to stop the switch; the ENTER can't be blocked. */
    RMM_VEHICLE_REASON_SPAWN = 2,  /**< Spawned straight into a vehicle (spawn point in a vehicle). */
    RMM_VEHICLE_REASON_GAME = 3,   /**< The game did it: death, respawn, vehicle destroyed or removed... */
    RMM_VEHICLE_REASON_PLUGIN = 4  /**< rmm_players_api::enter_vehicle / exit_vehicle. */
} rmm_vehicle_reason;

typedef struct rmm_ev_vehicle {
    rmm_player_t player;
    rmm_object_t vehicle;         /**< The object entered/left (may be a seat of a larger vehicle). */
    const char* vehicle_template; /**< e.g. "usjep_hmmwv", "Willys". */
    int32_t seat;                 /**< 0 = driver; -1 unknown. */
    rmm_bool is_driver;
    rmm_object_t root;         /**< Top-level vehicle containing @c vehicle (== vehicle if it is one). */
    const char* root_template; /**< Template of @c root, never NULL ("" if unknown). */
    int32_t reason;            /**< ENTER/EXIT: ::rmm_vehicle_reason (games that can't tell report
                             PLAYER). Unused for VEHICLE_DESTROYED. */
} rmm_ev_vehicle;

/** Why a soldier changed the item in their hands (rmm_ev_weapon_switch::reason). */
typedef enum rmm_weapon_reason {
    RMM_WEAPON_REASON_PLAYER = 0,  /**< The player's weapon key (bots too). */
    RMM_WEAPON_REASON_NETWORK = 1, /**< The game applied an item change from network state. */
    RMM_WEAPON_REASON_GAME = 2,    /**< The game did it: new kit, out of ammo, detonator after an explosive pack... */
    RMM_WEAPON_REASON_PLUGIN = 3   /**< rmm_players_api::select_item. */
} rmm_weapon_reason;

/** A soldier is about to hold another item. Blocking (PLAYER and
     *  NETWORK only) keeps the current one in their hands. */
typedef struct rmm_ev_weapon_switch {
    rmm_player_t player;
    int32_t from_slot; /**< -1 if none. */
    int32_t to_slot;
    const char* from_weapon; /**< Template, "" if none. */
    const char* to_weapon;   /**< Template, e.g. "Colt". */
    int32_t reason;          /**< ::rmm_weapon_reason. */
} rmm_ev_weapon_switch;

/** One of the map's object spawners is creating its object. Map
     *  vehicles, stationary guns and the like come from these. Blocking spawns
     *  nothing; the spawner tries again after its usual respawn time. The position
     *  is where the level designer placed it, so it is clear of buildings for that
     *  kind of object. */
typedef struct rmm_ev_spawner {
    rmm_object_t spawner;        /**< The spawner (not an object find_objects reports). */
    const char* object_template; /**< What it spawns, e.g. "T34". */
    rmm_vec3 position;
    rmm_vec3 rotation;   /**< Yaw, pitch, roll in degrees. */
    int32_t team;        /**< Team that owns the spawner (0 = none). */
    rmm_object_t object; /**< POST: the new object, 0 if the game failed to create it. */
} rmm_ev_spawner;

/** Damage to a vehicle (or any player-controllable object). Use the
     *  occupant list to decide whose vehicle it is; empty vehicles have none. */
typedef struct rmm_ev_vehicle_damage {
    rmm_object_t vehicle;         /**< Root vehicle that was hit. */
    const char* vehicle_template; /**< Template of @c vehicle. */
    rmm_player_t attacker;        /**< RMM_PLAYER_NONE if unknown/world. */
    float* damage;                /**< [mutable] Damage about to be applied. */
    const char* weapon;           /**< Weapon/projectile template, "" if unknown. */
    int32_t occupant_count;
    const rmm_player_t* occupants; /**< Players in any seat of the vehicle. */
} rmm_ev_vehicle_damage;

typedef struct rmm_ev_control_point {
    rmm_object_t control_point;
    const char* name;
    int32_t old_team;
    int32_t new_team;            /**< RMM_TEAM_NONE when neutralised. */
    int32_t player_count;        /**< 0 if unknown (or nobody: e.g. a round reset). */
    const rmm_player_t* players; /**< Who took it: living players of the capturing team
                                               (neutralised: of the attacking team) inside the
                                               point's radius. NULL when player_count is 0. */
} rmm_ev_control_point;

/** A destroyable world object that is not a vehicle (bridge, building part...)
     *  was destroyed. Destroyed vehicles are ::RMM_EV_VEHICLE_DESTROYED. */
typedef struct rmm_ev_object_destroyed {
    rmm_object_t object;
    const char* object_template;
    rmm_player_t attacker; /**< RMM_PLAYER_NONE if unknown/world. */
    const char* weapon;    /**< Weapon/projectile template, "" if unknown. */
} rmm_ev_object_destroyed;

/** Bot events. Delivered in both phases; never blockable. */
typedef struct rmm_ev_bot {
    rmm_player_t player; /**< The bot's player slot; RMM_PLAYER_NONE for ADD_FAILED. */
    int32_t bot_id;      /**< Engine bot number, -1 if unknown. */
    const char* name;
    int32_t team;
    /** rmm_bot_spawn::tag of the add() request that produced (or failed to
         *  produce) this bot; 0 for bots the game added itself and for removals. */
    int32_t tag;
    const char* reason; /**< ADD_FAILED: why; "" otherwise. */
} rmm_ev_bot;

typedef struct rmm_ev_plugin {
    rmm_plugin_t plugin;
    const char* name;
} rmm_ev_plugin;

/* ---- Events API ---- */

/**
     * Event callback.
     * @param event_id ::rmm_event_id or a custom id.
     * @param phase    ::rmm_event_phase
     * @param data     Event payload (see the event's documentation); may be NULL.
     * @param user     The pointer you passed to hook().
     * @return ::rmm_result (ignored in POST).
     */
typedef int32_t(RMM_CALL* rmm_event_cb)(int32_t event_id, int32_t phase, void* data, void* user);

typedef struct rmm_events_api {
    uint32_t struct_size;

    /** Hook an event. @p priority: higher runs first (::RMM_PRIORITY_NORMAL).
         *  @return handle, or ::RMM_INVALID_HANDLE if the id is unknown. */
    rmm_handle_t(RMM_CALL* hook)(rmm_plugin_t self, int32_t event_id, int32_t phase, int32_t priority, rmm_event_cb cb,
                                 void* user);

    /** Remove a hook. Safe to call from inside the hook itself. */
    rmm_bool(RMM_CALL* unhook)(rmm_plugin_t self, rmm_handle_t hook);

    /** ::RMM_EVF_* flags for the running game. 0 = unknown/unsupported. */
    uint32_t(RMM_CALL* flags)(int32_t event_id);

    /** Name of an event ("player_spawn", ...), or NULL. */
    const char*(RMM_CALL* name)(int32_t event_id);

    /** Look up an event (built-in or custom) by name. @return id or RMM_EV_NONE. */
    int32_t(RMM_CALL* find)(const char* name);

    /**
         * Create a custom event other plugins can hook (like SourceMod global
         * forwards). Names should be namespaced: "myplugin.vote_started".
         * Registering a name that already exists returns the existing id.
         * @param flags RMM_EVF_BLOCKABLE / RMM_EVF_MUTABLE as you intend to honour.
         */
    int32_t(RMM_CALL* register_custom)(rmm_plugin_t self, const char* name, uint32_t flags);

    /** Fire a custom event (PRE then, unless blocked, POST).
         *  @return the highest ::rmm_result returned by PRE handlers. */
    int32_t(RMM_CALL* fire)(rmm_plugin_t self, int32_t event_id, void* data);
} rmm_events_api;

#ifdef __cplusplus
}
#endif

#endif /* RMM_EVENTS_H */
