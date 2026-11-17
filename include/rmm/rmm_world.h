/**
 * @file rmm_world.h
 * @brief World objects: find vehicles, read their state, move or stop them;
 *        find any object, spawn, damage and remove objects, terrain height.
 *
 * ## Objects and roots
 * Refractor vehicles are trees of objects: a bomber is a root object with
 * child objects for each gunner seat, a carrier has gun emplacements, and so
 * on. Players sit in one of those objects. Every object handle reported by
 * RMM (event payloads, rmm_player_info::vehicle, find_vehicles) can be passed
 * here. The @c root of an object is the top-level vehicle, and the functions
 * that move things (set_position, set_velocity, ...) always act on the root.
 *
 * ## Handles
 * Handles are only valid while the object exists. Destroyed vehicles are
 * respawned as *new* objects with new handles. Every function validates its
 * handle and fails (returns false / 0) for a stale one, so it's safe to keep
 * handles across frames, but re-run find_vehicles after RMM_EV_MAP_LOAD.
 *
 * ## Capabilities
 * RMM_CAP_WORLD_OBJECTS (find/info/occupants), RMM_CAP_OBJECT_MOVE,
 * RMM_CAP_OBJECT_ROTATION, RMM_CAP_OBJECT_VELOCITY, RMM_CAP_OBJECT_QUERY,
 * RMM_CAP_OBJECT_SPAWN, RMM_CAP_OBJECT_REMOVE, RMM_CAP_OBJECT_DAMAGE and
 * RMM_CAP_GROUND_HEIGHT. Fields of
 * ::rmm_object_info that the game can't provide are left zero (vectors) or
 * -1 (health).
 *
 * @code
 * // Stop empty carriers from drifting.
 * rmm_object_t ships[16];
 * int32_t n = api->world->find_vehicles("*carrier*", RMM_FIND_ROOTS_ONLY | RMM_FIND_EMPTY_ONLY, ships, 16);
 * for (int32_t i = 0; i < n && i < 16; ++i)
 *     api->world->set_velocity(ships[i], (rmm_vec3){0, 0, 0}, (rmm_vec3){0, 0, 0});
 * @endcode
 */
#ifndef RMM_WORLD_H
#define RMM_WORLD_H

#include "rmm_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Snapshot of an object. Set @c struct_size = sizeof(rmm_object_info) before
     *  calling rmm_world_api::get_info(). */
typedef struct rmm_object_info {
    uint32_t struct_size;
    rmm_object_t object;
    rmm_object_t root; /**< Top-level vehicle (== object for a root). */
    char template_name[64];
    char root_template[64];
    rmm_vec3 position;         /**< World position of @c object. */
    rmm_vec3 rotation;         /**< Yaw/pitch/roll in degrees (RMM_CAP_OBJECT_ROTATION). */
    rmm_vec3 velocity;         /**< Root's linear velocity, m/s (RMM_CAP_OBJECT_VELOCITY). */
    rmm_vec3 angular_velocity; /**< Root's angular velocity, engine units (RMM_CAP_OBJECT_VELOCITY). */
    float health;              /**< Hit points of the root, -1 if unknown. */
    float max_health;          /**< -1 if unknown. */
    int32_t occupant_count;    /**< Players anywhere in the root vehicle. */
    rmm_bool is_vehicle;       /**< Player-controllable (has seats). */
    rmm_bool is_root;
    rmm_bool is_destroyed;
    /** The object is replicated to clients (it has a networkable). Objects
         *  without one exist only on the server: players can't see them, though
         *  the server still simulates them (collisions, damage...). */
    rmm_bool is_networked;
    /** Position number of this seat object in its vehicle (0 = driver), as
         *  rmm_players_api::enter_vehicle takes it; -1 if it isn't a seat. */
    int32_t seat;
    /** Player sitting in this very seat object, RMM_PLAYER_NONE if empty. */
    rmm_player_t seat_occupant;
} rmm_object_info;

/** Flags for rmm_world_api::find_vehicles(). */
#define RMM_FIND_ROOTS_ONLY 0x01u /**< Skip seats/sub-objects; return top-level vehicles. */
#define RMM_FIND_EMPTY_ONLY 0x02u /**< Only vehicles nobody is sitting in (any seat). */

/** Flags for rmm_world_api::find_objects() (in addition to the above). */
#define RMM_FIND_NO_VEHICLES 0x04u /**< Skip player-controllable objects. */
#define RMM_FIND_NO_SOLDIERS 0x08u /**< Skip players' soldiers. */

/** rmm_world_api::spawn_object() flag: @p position.y is a height above the
 *  terrain at (x, z), not an absolute height. Requires RMM_CAP_GROUND_HEIGHT; terrain only, so on a building
 *  site the object may end up inside the building. */
#define RMM_SPAWN_ON_GROUND 0x01u

/** What a control point is doing (rmm_control_point_info::state). */
typedef enum rmm_cp_state {
    RMM_CP_STATE_UNKNOWN = 0,
    RMM_CP_STATE_HELD = 1,   /**< Settled: owned by @c team, or neutral. */
    RMM_CP_STATE_LOSING = 2, /**< Enemies are at the point; the owner's flag is coming down. */
    RMM_CP_STATE_TAKING = 3  /**< A neutral point is being raised by @c capturing_team. */
} rmm_cp_state;

/** Snapshot of a control point (flag). Set @c struct_size first. */
typedef struct rmm_control_point_info {
    uint32_t struct_size;
    rmm_object_t object;
    char name[64]; /**< Template name, as in ::RMM_EV_CONTROL_POINT. */
    rmm_vec3 position;
    float radius;           /**< Capture radius in metres, 0 if unknown. */
    int32_t team;           /**< Owner, RMM_TEAM_NONE (0) when neutral. */
    int32_t capturing_team; /**< Team taking it while TAKING, else 0. */
    int32_t state;          /**< ::rmm_cp_state */
    rmm_bool locked;        /**< Locked with rmm_world_api::lock_control_point. */
    /** The map made it uncapturable (BF1942: unableToChangeTeam, e.g. a main base). */
    rmm_bool fixed;
    /** Only this team may take it (BF1942: onlyTakeableByTeam), 0 = any. */
    int32_t only_team;
} rmm_control_point_info;

typedef struct rmm_world_api {
    uint32_t struct_size;

    /**
         * Find player-controllable objects (vehicles and their seats).
         * @param pattern Case-insensitive template glob (`*` and `?`); several
         *                patterns may be separated by commas ("*carrier*,yorktown").
         *                NULL or "" matches everything.
         * @param flags   RMM_FIND_*.
         * @return total number of matches (may exceed @p cap); 0 if unsupported.
         */
    int32_t(RMM_CALL* find_vehicles)(const char* pattern, uint32_t flags, rmm_object_t* out, int32_t cap);

    /** Does the object still exist? */
    rmm_bool(RMM_CALL* is_valid)(rmm_object_t obj);

    /** Fill @p out (set @c struct_size first). */
    rmm_bool(RMM_CALL* get_info)(rmm_object_t obj, rmm_object_info* out);

    /** Players sitting anywhere in @p obj's root vehicle. @return total count (may exceed @p cap). */
    int32_t(RMM_CALL* occupants)(rmm_object_t obj, rmm_player_t* out, int32_t cap);

    /** Move the root of @p obj. RMM_CAP_OBJECT_MOVE. */
    rmm_bool(RMM_CALL* set_position)(rmm_object_t obj, rmm_vec3 position);

    /** Set the root's yaw/pitch/roll in degrees. RMM_CAP_OBJECT_ROTATION. */
    rmm_bool(RMM_CALL* set_rotation)(rmm_object_t obj, rmm_vec3 yaw_pitch_roll);

    /** Set the root's linear (m/s) and angular (engine units) velocity.
         *  Zero both to stop a drifting vehicle; physics resumes on the next
         *  tick, so call it periodically to keep something still.
         *  RMM_CAP_OBJECT_VELOCITY. */
    rmm_bool(RMM_CALL* set_velocity)(rmm_object_t obj, rmm_vec3 linear, rmm_vec3 angular);

    /**
         * Find top-level world objects of any kind: buildings, bunkers, props,
         * kits lying on the ground, vehicles, soldiers... RMM_CAP_OBJECT_QUERY.
         *
         * Only roots are listed (a vehicle, not its seats). Use the template
         * name to tell objects apart; `rmm find` on the console lists what a
         * map contains.
         *
         * @param pattern Template glob(s) as in find_vehicles. NULL/"" = all.
         * @param center  Search around this point, or NULL for the whole map.
         * @param radius  Metres from @p center, measured in 3D (ignored when
         *                @p center is NULL).
         * @param flags   RMM_FIND_NO_VEHICLES / RMM_FIND_NO_SOLDIERS.
         * @return total number of matches (may exceed @p cap). With a center,
         *         results are sorted nearest first.
         *
         * Walking a whole map visits every object (thousands on a big map), so
         * cache the results instead of searching every frame.
         */
    int32_t(RMM_CALL* find_objects)(const char* pattern, const rmm_vec3* center, float radius, uint32_t flags,
                                    rmm_object_t* out, int32_t cap);

    /**
         * Create an object from a template, the way the engine spawns vehicles
         * and kits, and announce it to every connected client.
         * RMM_CAP_OBJECT_SPAWN.
         *
         * - The template must exist on the clients too (vanilla or the running
         *   mod). A template made only on the server would desync the clients.
         * - Only networked templates (see rmm_object_info::is_networked) are
         *   seen by players. Vehicles and kits are networked; most static
         *   props and effects aren't, so they exist only on the server.
         * - Like the engine's vehicle spawners, a vehicle won't spawn within
         *   2 m of a living soldier (the call returns 0).
         * - Pick a clear spot. On BF1942 a vehicle whose wheels start inside
         *   some objects takes lethal collision damage on its first frame
         *   (VEHICLE_DESTROYED "by world") and its wreck vanishes soon after.
         *   find_objects with a small radius helps check the spot.
         * - Call it from a timer, frame event or command, not from inside an
         *   engine callback such as a damage handler.
         *
         * @param rotation yaw/pitch/roll in degrees.
         * @param flags    RMM_SPAWN_*.
         * @return handle of the new object, or 0 (unknown template, blocked,
         *         or unsupported).
         */
    rmm_object_t(RMM_CALL* spawn_object)(const char* template_name, rmm_vec3 position, rmm_vec3 rotation,
                                         uint32_t flags);

    /**
         * Remove an object (the root of @p obj) from the server and from every
         * client. The engine kills players sitting in a removed vehicle.
         * Players' soldiers can't be removed. Removal completes at the end of
         * the frame, so the handle may stay valid until then.
         * RMM_CAP_OBJECT_REMOVE.
         */
    rmm_bool(RMM_CALL* remove_object)(rmm_object_t obj);

    /** Height of the terrain (not of objects standing on it) at world
         *  (@p x, @p z). False when no map is loaded. RMM_CAP_GROUND_HEIGHT. */
    rmm_bool(RMM_CALL* ground_height)(float x, float z, float* height);

    /** Does the engine know a template of this name (case-insensitive)?
         *  RMM_CAP_OBJECT_QUERY. */
    rmm_bool(RMM_CALL* template_exists)(const char* template_name);

    /**
         * Damage the root of @p obj (a vehicle, or any armoured world object such
         * as a bridge) by @p amount hit points, the way a hit from a weapon does.
         * Damage beyond its health destroys it: a vehicle explodes, kills whoever
         * is still inside, and leaves a wreck like any destroyed vehicle; use
         * rmm_object_info::health for the amount needed (or pass a large value,
         * e.g. 100000). @p attacker is credited where the game supports it
         * (RMM_PLAYER_NONE = the world).
         *
         * Raises ::RMM_EV_VEHICLE_DESTROYED / ::RMM_EV_OBJECT_DESTROYED when it
         * destroys the object, but no damage event. Players' soldiers are refused
         * (use rmm_players_api::damage). Called from inside the game's damage
         * processing (a KILLED/DAMAGE handler on BF1942) it happens at the end of
         * the server frame. RMM_CAP_OBJECT_DAMAGE.
         *
         * @return false for an unknown/stale handle, an object without armour, an
         *         already destroyed one, or amount <= 0.
         */
    rmm_bool(RMM_CALL* damage_object)(rmm_object_t obj, float amount, rmm_player_t attacker);

    /* ---- control points ----------------------------------------------------- */

    /** Every control point of the running map, in the map's order.
         *  @return total count (may exceed @p cap). RMM_CAP_CONTROL_POINTS. */
    int32_t(RMM_CALL* control_points)(rmm_object_t* out, int32_t cap);

    /** Fill @p out (set @c struct_size first). RMM_CAP_CONTROL_POINTS. */
    rmm_bool(RMM_CALL* get_control_point)(rmm_object_t cp, rmm_control_point_info* out);

    /**
         * Give a control point to @p team (1 or 2) or make it neutral (0) at once,
         * the way a capture does: spawn points and map vehicle spawners attached to
         * it change sides with it. Works on fixed and locked points too; no player
         * scores. Raises ::RMM_EV_CONTROL_POINT (player_count 0) if the owner
         * changed. RMM_CAP_CONTROL_POINT_SET.
         */
    rmm_bool(RMM_CALL* set_control_point)(rmm_object_t cp, int32_t team);

    /**
         * Lock a control point: nobody can capture or neutralise it until it is
         * unlocked, whoever stands at it. Locking cancels a capture in progress.
         * Every lock is cleared before the next round starts, so lock again in
         * ::RMM_EV_ROUND_START. RMM_CAP_CONTROL_POINT_LOCK.
         */
    rmm_bool(RMM_CALL* lock_control_point)(rmm_object_t cp, rmm_bool locked);
} rmm_world_api;

#ifdef __cplusplus
}
#endif

#endif /* RMM_WORLD_H */
