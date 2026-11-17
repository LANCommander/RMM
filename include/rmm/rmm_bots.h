/**
 * @file rmm_bots.h
 * @brief AI bots: list and inspect bots, add and remove them, give
 *        them orders, steer the team-level strategic AI, change their skill.
 *
 * ## Bots and players
 * A bot is a player driven by the engine's AI. It has a normal player slot, so
 * everything in rmm_players_api works on it too (rmm_player_info::is_bot is
 * set). This table adds what only makes sense for bots. Functions take the
 * bot's **player id**; the engine's own bot number is reported as
 * rmm_bot_info::bot_id for reference and logs.
 *
 * ## Availability
 * Two levels, like the rest of the SDK:
 *  - Capabilities (rmm_game_api::has_cap) say what the running game/backend
 *    can do at all: ::RMM_CAP_BOT_QUERY, ::RMM_CAP_BOT_ADD,
 *    ::RMM_CAP_BOT_REMOVE, ::RMM_CAP_BOT_ORDERS, ::RMM_CAP_BOT_STRATEGY,
 *    ::RMM_CAP_BOT_SKILL, ::RMM_CAP_BOT_INPUT.
 *  - available() says whether the AI is running *right now*. On Battlefield
 *    1942/Vietnam the AI only runs in co-op rounds; on other modes every
 *    function here fails (returns 0/false), and RMM never sends AI console
 *    commands then (some of them crash a non-co-op server).
 *
 * ## The strategic AI
 * Each team's bots are steered by a strategic AI ("SAI") that picks a
 * strategy (attack north, defend, final push...) and hands out goals. Orders
 * given to a single bot (move_to, patrol, order) are taken at once but the SAI
 * hands out new goals every few seconds, so they are short-lived nudges, not
 * scripts. To redirect a whole team, set its strategy instead.
 *
 * ## Replacing the AI
 * set_control() + set_input() take over a bot's controls at the input level,
 * the same record a client sends (move, look, fire, use...), see
 * ::rmm_bot_input. get_input() shows what the engine's own AI does each tick.
 * Perception comes from the rest of the SDK (players, world objects).
 *
 * ## Events
 * ::RMM_EV_BOT_ADDED / ::RMM_EV_BOT_REMOVED report bots appearing and going
 * away (whoever caused it); ::RMM_EV_BOT_ADD_FAILED reports an add() that did
 * not produce a bot. RMM looks for changes about once a second, and only while
 * a plugin has hooked one of these events or an add() is pending.
 *
 * @code
 * // Send every Axis bot towards a flag.
 * rmm_player_t bots[64];
 * int32_t n = api->bots->list(bots, 64);
 * for (int32_t i = 0; i < n && i < 64; ++i) {
 *     rmm_bot_info b = { sizeof b };
 *     if (api->bots->get_info(bots[i], &b) && b.team == 1)
 *         api->bots->move_to(bots[i], flag_pos, 10.0f);
 * }
 * @endcode
 */
#ifndef RMM_BOTS_H
#define RMM_BOTS_H

#include "rmm_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Snapshot of a bot. Set @c struct_size = sizeof(rmm_bot_info) before
     *  calling rmm_bots_api::get_info(). Fields the game can't provide are left
     *  zero/empty. */
typedef struct rmm_bot_info {
    uint32_t struct_size;
    rmm_player_t player; /**< Player slot of the bot. */
    int32_t bot_id;      /**< The engine's bot number (BF1942: index in the AI's bot list). */
    char name[64];
    int32_t team;
    char kit[32];     /**< Engine kit type, e.g. "KTAssault"; "" if unknown. */
    char vehicle[64]; /**< Vehicle the AI is driving/using, "" on foot. */
    rmm_vec3 position;
    rmm_vec3 goal;      /**< Where the bot is heading (its current path goal). */
    char behaviour[32]; /**< Active behaviour, e.g. "Idle", "MoveTo", "Fire"; "" if unknown. */
} rmm_bot_info;

/** rmm_bot_spawn::flags */
#define RMM_BOT_SPAWN_AT 0x01u /**< Spawn at rmm_bot_spawn::position (else the game picks a spawn point). */

/** Description of a bot to add. Initialise with ::RMM_BOT_SPAWN_INIT. */
typedef struct rmm_bot_spawn {
    uint32_t struct_size;
    /** Player name. NULL/"" = "Bot<n>". Spaces and quotes are replaced with
         *  '_' (the engine's console splits names on them). Must not match a
         *  connected player's name. */
    const char* name;
    int32_t team; /**< 1 or 2. */
    /** Engine kit type ("KTScout", "KTAssault", "KTAT", "KTMedic",
         *  "KTEngineer" on BF1942). NULL/"" = gamedata default. */
    const char* kit;
    rmm_vec3 position; /**< Used with ::RMM_BOT_SPAWN_AT. */
    uint32_t flags;    /**< RMM_BOT_SPAWN_* */
    /** Behaviour weight set ("personality"). NULL/"" = gamedata default
         *  (BF1942: "StandardWeights"). */
    const char* personality;
    /** Echoed in ::rmm_ev_bot::tag of the ADDED / ADD_FAILED event so you can
         *  match the result to the request. Use a non-zero value. */
    int32_t tag;
} rmm_bot_spawn;

#define RMM_BOT_SPAWN_INIT {(uint32_t)sizeof(rmm_bot_spawn), NULL, 1, NULL, {0.0f, 0.0f, 0.0f}, 0u, NULL, 0}

/* ---- bot input ----
     * A bot is driven like a network client: every engine tick its AI fills the
     * same input record a player's keyboard/mouse produce, and the engine turns
     * it into movement, aiming, firing... rmm_bots_api::set_control +
     * set_input replace (part of) that record, so an AI written elsewhere (another
     * plugin, or a process at the other end of a pipe) can play the bot.
     *
     * Slots are the engine's own input numbers (its PlayerInputMap). Refractor 1
     * (BF1942/BFV) uses 0..54 below. */
typedef enum rmm_bot_input_slot {
    /* Axes 0..5. Which ones a soldier or each vehicle type reads comes from the
         * game's control maps; read get_input() of a bot doing the same thing. */
    RMM_BI_YAW = 0,      /**< axis */
    RMM_BI_PITCH,        /**< axis */
    RMM_BI_ROLL,         /**< axis */
    RMM_BI_THROTTLE,     /**< axis: forward/back */
    RMM_BI_MOUSE_LOOK_X, /**< axis: look left/right */
    RMM_BI_MOUSE_LOOK_Y, /**< axis: look up/down */
    RMM_BI_CAMERA_X,
    RMM_BI_CAMERA_Y,
    RMM_BI_FIRE,   /**< 8: buttons from here on (> 0.5 = pressed) */
    RMM_BI_ACTION, /**< jump */
    RMM_BI_USE,    /**< enter/exit vehicle, pick up kit */
    RMM_BI_MOUSE_LOOK,
    RMM_BI_WALK,
    RMM_BI_RUN,           /**< sprint */
    RMM_BI_MENU_SELECT_1, /**< 14..22: weapon slots 1..9 */
    RMM_BI_MENU_SELECT_9 = RMM_BI_MENU_SELECT_1 + 8,
    RMM_BI_ALT_FIRE, /**< 23 */
    RMM_BI_RELOAD,
    RMM_BI_DROP,
    RMM_BI_TOGGLE_CAMERA_MODE,
    RMM_BI_TOGGLE_CAMERA,
    RMM_BI_LIE,                                /**< 28: go prone */
    RMM_BI_CROUCH,                             /**< 29 */
    RMM_BI_CAMERA_MODE_1,                      /**< 30..33 */
    RMM_BI_RADIO_1 = RMM_BI_CAMERA_MODE_1 + 4, /**< 34..41 */
    RMM_BI_SCREENSHOT = RMM_BI_RADIO_1 + 8,    /**< 42 */
    RMM_BI_TOOLTIP,
    RMM_BI_SAY_ALL,
    RMM_BI_SAY_TEAM,
    RMM_BI_NEXT_ITEM, /**< 46: next weapon */
    RMM_BI_PREV_ITEM,
    RMM_BI_COMMUNICATION,
    RMM_BI_SHOW_SCOREBOARD,
    RMM_BI_MAP,
    RMM_BI_ZOOM_MAP,
    RMM_BI_SHOW_MAP_VOTE,
    RMM_BI_VOTE_YES,
    RMM_BI_VOTE_NO, /**< 54 */
    RMM_BI_COUNT_R1 /**< 55 slots on Refractor 1 */
} rmm_bot_input_slot;

#define RMM_BOT_INPUT_SLOTS 64               /**< size of rmm_bot_input::value */
#define RMM_BI_BIT(slot)    (1ull << (slot)) /**< rmm_bot_input::mask bit of a slot */
#define RMM_BI_AXES_R1      0x3full          /**< mask of the six fixed-point axes */

/** A full or partial input record. */
typedef struct rmm_bot_input {
    uint32_t struct_size;
    /** set_input: keep applying for this long (ms), then stop (0 = until
         *  replaced). get_input: age of the sample in ms. */
    uint32_t hold_ms;
    /** Which value[] slots are present (RMM_BI_BIT). */
    uint64_t mask;
    float value[RMM_BOT_INPUT_SLOTS];
    /** get_input: engine input ticks seen for this bot so far. */
    uint32_t tick;
} rmm_bot_input;

/** rmm_bots_api::set_control modes */
#define RMM_BOT_CONTROL_ENGINE   0 /**< the engine's AI drives (default); releases control */
#define RMM_BOT_CONTROL_OVERRIDE 1 /**< the AI drives, your set_input slots replace its values */
/** Only your input counts: slots you don't set (or after hold_ms ran out) are
 *  released/centred. The engine AI still runs (it picks spawn points and
 *  keeps planning) but its input is discarded. */
#define RMM_BOT_CONTROL_EXCLUSIVE 2

typedef struct rmm_bots_api {
    uint32_t struct_size;

    /** Is the AI running right now (BF1942/BFV: a co-op round)? Every other
         *  function fails while this is false. Cheap enough to call per command,
         *  but don't poll it every frame. */
    rmm_bool(RMM_CALL* available)(void);

    /** Number of bots in the game. */
    int32_t(RMM_CALL* count)(void);

    /** Fill @p out with the bots' player ids. @return total (may exceed @p cap). */
    int32_t(RMM_CALL* list)(rmm_player_t* out, int32_t cap);

    /** Is @p player a bot the AI controls? */
    rmm_bool(RMM_CALL* is_bot)(rmm_player_t player);

    /** Fill @p out (set @c struct_size first). RMM_CAP_BOT_QUERY. */
    rmm_bool(RMM_CALL* get_info)(rmm_player_t player, rmm_bot_info* out);

    /**
         * Behaviours this bot can be ordered into (see order()), as
         * newline-separated names in their engine order ("Avoid\nMoveTo\nIdle..").
         * @return length of the full list (may exceed @p cap - 1), 0 on failure.
         * RMM_CAP_BOT_QUERY.
         */
    size_t(RMM_CALL* behaviours)(rmm_player_t player, char* out, size_t cap);

    /* ---- adding and removing ------------------------------------------------- */

    /**
         * Ask the game for a new bot. RMM_CAP_BOT_ADD.
         *
         * Adding takes a few frames (the engine creates the player first), so the
         * call only starts it: it returns true if the request was accepted, and
         * the result arrives as ::RMM_EV_BOT_ADDED or ::RMM_EV_BOT_ADD_FAILED with
         * your @c tag. Requests fail when the server or the AI is full.
         *
         * BF1942: off unless the server admin opts in (gamedata `[bots]
         * allow_add = 1`). The engine's strategic AI is sized for the bots that
         * existed at round start, and adding bots on top of that can hang the
         * server.
         */
    rmm_bool(RMM_CALL* add)(const rmm_bot_spawn* spawn);

    /**
         * Remove a bot from the game: its AI is detached and its player is
         * destroyed like a disconnecting client (PLAYER_DISCONNECT, and
         * BOT_REMOVED when tracked). RMM_CAP_BOT_REMOVE. @return true if the
         * removal was done or started.
         *
         * BF1942 (Linux): the engine only detaches *dead* bots, so a living bot is
         * killed first (a normal PLAYER_KILLED) and removed within a frame or two
         * of dying. Co-op normally tops the server up with a new bot at once; RMM
         * keeps removed bots' slots empty until the round ends (gamedata [bots]
         * keep_removed). The next round starts with the full bot count again.
         */
    rmm_bool(RMM_CALL* remove)(rmm_player_t player);

    /** Remove every bot. @return number removed. RMM_CAP_BOT_REMOVE. */
    int32_t(RMM_CALL* remove_all)(void);

    /* ---- orders for one bot (RMM_CAP_BOT_ORDERS) ----------------------------- */

    /** Send the bot to @p position; it counts as arrived within @p radius
         *  metres (<= 0 = game default). Pathfinding picks the route. Short-lived:
         *  the strategic AI hands out its own goals again after a few seconds. */
    rmm_bool(RMM_CALL* move_to)(rmm_player_t player, rmm_vec3 position, float radius);

    /** Patrol between @p count points (1..2 on BF1942). Same lifetime caveat
         *  as move_to(). */
    rmm_bool(RMM_CALL* patrol)(rmm_player_t player, const rmm_vec3* points, int32_t count, float radius);

    /**
         * Switch the bot into a behaviour (a name from behaviours(), e.g.
         * "Idle"), weighted by a behaviour-modifier set (NULL/"" = gamedata
         * default). Unknown names are ignored by the engine, so this returns true
         * when the order was sent, not when it was obeyed: check get_info()'s
         * behaviour afterwards.
         */
    rmm_bool(RMM_CALL* order)(rmm_player_t player, const char* behaviour, const char* modifiers);

    /* ---- team strategy (RMM_CAP_BOT_STRATEGY) --------------------------------- */

    /** The active strategy of @p team's strategic AI (e.g. "finalPush"). */
    rmm_bool(RMM_CALL* get_strategy)(int32_t team, char* out, size_t cap);

    /** Strategies @p team can use, newline-separated. @return full length, 0 on failure. */
    size_t(RMM_CALL* strategies)(int32_t team, char* out, size_t cap);

    /** Force @p team onto a strategy (a name from strategies(); others are
         *  refused). It takes effect at the SAI's next update, about a second
         *  later, so get_strategy() may still show the old one right away. The SAI
         *  keeps it until it decides the strategy is done or failed, then picks
         *  again. */
    rmm_bool(RMM_CALL* set_strategy)(int32_t team, const char* strategy);

    /* ---- skill (RMM_CAP_BOT_SKILL) -------------------------------------------- */

    /** Current bot skill, 0.0 - 1.0. Applies to every bot at once. (The
         *  RMM_SETTING_BOT_SKILL server setting only applies from the next round.) */
    rmm_bool(RMM_CALL* get_skill)(float* skill);
    rmm_bool(RMM_CALL* set_skill)(float skill);

    /* ---- input control (RMM_CAP_BOT_INPUT) ------------------------------------ */

    /**
         * Take or give back a bot's controls (one of RMM_BOT_CONTROL_*).
         * ENGINE releases it. Only one plugin controls a bot at a time: this fails
         * if another plugin holds it (the server console may always take over).
         * Control ends by itself when the bot leaves the game, on map change, and
         * when your plugin unloads.
         */
    rmm_bool(RMM_CALL* set_control)(rmm_plugin_t self, rmm_player_t player, int32_t mode);

    /** Current RMM_BOT_CONTROL_* mode of a bot; @p owner (optional) receives
         *  the controlling plugin (NULL = the console or nobody). */
    int32_t(RMM_CALL* get_control)(rmm_player_t player, rmm_plugin_t* owner);

    /**
         * Latch the input your bot sends from now on (needs set_control first).
         * Slots set in @c mask are applied every engine input tick until the next
         * set_input(), until @c hold_ms runs out, or until control ends. Axes
         * (yaw .. mouse look y) are sent as-is (normally -1..1; the network format
         * caps them at +-16); buttons count as pressed above 0.5. NaN/inf become 0.
         */
    rmm_bool(RMM_CALL* set_input)(rmm_plugin_t self, rmm_player_t player, const rmm_bot_input* input);

    /**
         * The input the engine's AI produced for this bot on its latest tick,
         * before any override: what the built-in AI "wanted to do". Works for
         * every bot, controlled or not, so it can also be recorded as training
         * data. @c hold_ms receives the age of the sample in ms, @c tick the number
         * of input ticks seen for this bot.
         */
    rmm_bool(RMM_CALL* get_input)(rmm_player_t player, rmm_bot_input* out);
} rmm_bots_api;

#ifdef __cplusplus
}
#endif

#endif /* RMM_BOTS_H */
