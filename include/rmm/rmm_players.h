/**
 * @file rmm_players.h
 * @brief Player enumeration, information, targeting and manipulation.
 */
#ifndef RMM_PLAYERS_H
#define RMM_PLAYERS_H

#include "rmm_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
     * Snapshot of a player. Set @c struct_size = sizeof(rmm_player_info) before
     * calling rmm_players_api::get_info(). Fields the running game cannot provide
     * are left zero/empty.
     */
typedef struct rmm_player_info {
    uint32_t struct_size;
    rmm_player_t id;
    char name[64];
    char address[64]; /**< "ip:port" */
    /** Stable identity used for admins/bans:
         *  BF2/2142: GameSpy/BF2Hub profile id (numeric string).
         *  BF1942/BFV: CD-key hash. Empty until RMM_EV_PLAYER_AUTH. */
    char auth_id[64];
    int32_t team;
    int32_t squad; /**< 0 = none (BF2/2142 only). */
    int32_t score;
    int32_t kills;
    int32_t deaths;
    int32_t ping;
    rmm_bool is_bot;
    rmm_bool is_alive;
    rmm_bool is_commander;
    rmm_bool is_squad_leader;
    char kit[64];
    rmm_object_t vehicle; /**< 0 when on foot. */
    char vehicle_template[64];
    rmm_vec3 position;
    int32_t seat; /**< Position in the vehicle (0 = driver), -1 on foot or unknown. */
} rmm_player_info;

/** One item (weapon or tool) a soldier carries (rmm_players_api::get_items). */
typedef struct rmm_item_info {
    int32_t slot;    /**< Item slot, the number the player selects it with. */
    char weapon[64]; /**< Template, e.g. "Bar1918" (the name kill events report). */
    rmm_bool active; /**< The item the player is holding. */
} rmm_item_info;

/** Flags for rmm_players_api::target(). */
#define RMM_TARGET_ALLOW_MULTI 0x01u /**< Allow @all/@team etc. */
#define RMM_TARGET_NO_BOTS     0x02u
#define RMM_TARGET_ALIVE_ONLY  0x04u
#define RMM_TARGET_NO_IMMUNITY 0x08u /**< Skip admin immunity checks. */

typedef struct rmm_players_api {
    uint32_t struct_size;

    int32_t(RMM_CALL* max_players)(void);
    int32_t(RMM_CALL* count)(void);
    /** Fill @p out with connected player ids. @return total count (may exceed cap). */
    int32_t(RMM_CALL* list)(rmm_player_t* out, int32_t cap);
    rmm_bool(RMM_CALL* is_connected)(rmm_player_t player);
    rmm_bool(RMM_CALL* get_info)(rmm_player_t player, rmm_player_info* out);

    /** Case-insensitive name lookup. Exact match wins; otherwise a unique
         *  substring match. @return id, RMM_PLAYER_NONE, or RMM_PLAYER_AMBIGUOUS. */
    rmm_player_t(RMM_CALL* find_by_name)(const char* name);

    /**
         * Resolve a SourceMod-style target string:
         *   `#<id>`  slot id        `name`  partial name (must be unique)
         *   `@all` `@t1` `@t2` `@alive` `@dead` `@bots` `@humans` `@me` `@!me`
         * Respects admin immunity of @p caller unless RMM_TARGET_NO_IMMUNITY.
         * @return number of players written to @p out; 0 on failure with a
         *         human-readable message in @p err.
         */
    int32_t(RMM_CALL* target)(const char* pattern, rmm_player_t caller, uint32_t flags, rmm_player_t* out, int32_t cap,
                              char* err, size_t err_cap);

    /* ---- manipulation (check rmm_game_api::has_cap first) -------------- */

    rmm_bool(RMM_CALL* kill)(rmm_player_t player);                     /**< RMM_CAP_KILL_PLAYER */
    rmm_bool(RMM_CALL* set_team)(rmm_player_t player, int32_t team);   /**< RMM_CAP_SET_TEAM */
    rmm_bool(RMM_CALL* message)(rmm_player_t player, const char* msg); /**< RMM_CAP_PRIVATE_MESSAGE */
    rmm_bool(RMM_CALL* set_score)(rmm_player_t player, int32_t score); /**< RMM_CAP_SET_SCORE */
    rmm_bool(RMM_CALL* teleport)(rmm_player_t player, rmm_vec3 pos);   /**< RMM_CAP_TELEPORT */

    /**
         * Damage a living player's soldier by @p amount (engine hit points; a full
         * soldier has get_health()'s @c max_health). Lethal damage kills the
         * player. @p attacker is credited for the kill where the game supports
         * it; pass RMM_PLAYER_NONE for "world" damage. Does not raise
         * ::RMM_EV_PLAYER_DAMAGE, so it is safe to call from that event's handler
         * (e.g. to reflect friendly fire). RMM_CAP_DAMAGE_PLAYER.
         */
    rmm_bool(RMM_CALL* damage)(rmm_player_t player, float amount, rmm_player_t attacker);

    /** Current and maximum hit points of a living player's soldier. Either
         *  pointer may be NULL; @p max_health is -1 when the game doesn't report
         *  it. RMM_CAP_PLAYER_HEALTH. */
    rmm_bool(RMM_CALL* get_health)(rmm_player_t player, float* health, float* max_health);

    /**
         * Add @p velocity (m/s, world axes; Y is up on every Refractor game) to a
         * living player, e.g. {0, 8, 0} launches them upwards. On foot this moves
         * their soldier; in a vehicle it moves the whole vehicle, with
         * everyone in it. The engine then simulates the result (gravity, landing,
         * fall damage). Fails for dead players. RMM_CAP_PLAYER_PUSH.
         */
    rmm_bool(RMM_CALL* push)(rmm_player_t player, rmm_vec3 velocity);

    /** message() with pacing options (see ::rmm_msg_opts; NULL = defaults).
         *  message() itself is paced with the defaults. Both return true once the line is
         *  sent or queued; false without RMM_CAP_PRIVATE_MESSAGE or if the
         *  player's queue is full. Queued lines to a player who leaves are dropped. */
    rmm_bool(RMM_CALL* message_ex)(rmm_player_t player, const char* msg, const rmm_msg_opts* opts);

    /**
         * Replace a living, on-foot player's kit (their whole weapon loadout) with
         * a kit made from @p kit_template, the way the game hands out a kit at
         * spawn. Every client sees the change. The old kit is removed, not dropped.
         *
         * - @p kit_template must be a kit template of the running game or mod
         *   (BF1942: "GermanAssault", "Rus_AT"..., see rmm_player_info::kit and
         *   `rmm find`); any team's kit works, so a ladder can mix them.
         * - Fails for dead players and players in a vehicle. To arm a player as
         *   they spawn, call it from a ::RMM_EV_PLAYER_SPAWN handler.
         * - Called from inside the game's damage processing (a ::RMM_EV_PLAYER_KILLED
         *   or ::RMM_EV_PLAYER_DAMAGE handler on BF1942), the swap happens at the
         *   end of the server frame; it is dropped if the player died or got into
         *   a vehicle by then.
         * - Raises ::RMM_EV_PLAYER_KIT_DROP (old kit) and ::RMM_EV_PLAYER_KIT_PICKUP
         *   (new kit) with reason ::RMM_KIT_REASON_PLUGIN where the game reports them.
         * - Don't call it from inside a KIT_PICKUP/KIT_DROP handler.
         *
         * RMM_CAP_SET_KIT.
         */
    rmm_bool(RMM_CALL* set_kit)(rmm_player_t player, const char* kit_template);

    /**
         * Put a living player into @p vehicle (any handle of it: root or seat)
         * at position @p seat (0 = driver; rmm_object_info::seat numbers the
         * seat objects), or the first free one with @p seat = -1. A player
         * already in a vehicle leaves it first. No distance limit: the soldier is
         * moved into the vehicle, wherever it is. The team check that applies to
         * players doesn't apply here. Fails if that seat is taken.
         *
         * Raises VEHICLE_EXIT/VEHICLE_ENTER with reason ::RMM_VEHICLE_REASON_PLUGIN
         * (not blockable). Called from inside the game's damage processing (a
         * KILLED/DAMAGE handler on BF1942) it happens at the end of the server
         * frame. RMM_CAP_PUT_IN_VEHICLE.
         */
    rmm_bool(RMM_CALL* enter_vehicle)(rmm_player_t player, rmm_object_t vehicle, int32_t seat);

    /** Take a living player out of their vehicle, as if they had pressed the
         *  use key but without the game's "allowed to exit" checks (a plane in
         *  flight drops them). Reason ::RMM_VEHICLE_REASON_PLUGIN, not blockable.
         *  Deferred like enter_vehicle. RMM_CAP_PUT_IN_VEHICLE. */
    rmm_bool(RMM_CALL* exit_vehicle)(rmm_player_t player);

    /**
         * The weapons and tools a living, on-foot player carries, one per item
         * slot (BF1942: 1 knife, 2 pistol, 3 main weapon, 4 grenades/explosives,
         * 5+ tools; see docs/BF1942_WEAPONS.md). Fills up to @p cap entries of
         * @p out in slot order. @return the number of items (may exceed @p cap),
         * -1 for a dead or unknown player or one in a vehicle. RMM_CAP_ITEMS.
         */
    int32_t(RMM_CALL* get_items)(rmm_player_t player, rmm_item_info* out, int32_t cap);

    /**
         * Make a living, on-foot player hold the item in @p slot, as if they had
         * pressed its key. Raises ::RMM_EV_PLAYER_WEAPON_SWITCH with reason
         * ::RMM_WEAPON_REASON_PLUGIN (not blockable). Fails for an empty slot.
         * RMM_CAP_ITEMS.
         */
    rmm_bool(RMM_CALL* select_item)(rmm_player_t player, int32_t slot);

    /**
         * Take the item in @p slot away from a living, on-foot player for the
         * rest of this life (the next kit brings it back). If they were holding
         * it they switch to their best loaded weapon. RMM_CAP_ITEMS.
         * BF1942: experimental; the player's own client may not drop the item.
         */
    rmm_bool(RMM_CALL* remove_item)(rmm_player_t player, int32_t slot);
} rmm_players_api;

#ifdef __cplusplus
}
#endif

#endif /* RMM_PLAYERS_H */
