/**
 * @file rmm_admin.h
 * @brief Admin permissions, kicking and RMM-managed bans.
 *
 * Admins are configured in rmm/configs/admins.cfg and identified by auth id
 * (preferred), IP, or name. Permissions are single-letter flags, deliberately
 * compatible with SourceMod's so admins feel at home.
 *
 * Bans are stored by RMM itself (rmm/data/bans.txt) and enforced on connect,
 * so they work identically across all games regardless of each engine's own
 * ban support, and support timed bans everywhere.
 */
#ifndef RMM_ADMIN_H
#define RMM_ADMIN_H

#include "rmm_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RMM_ADMFLAG_RESERVATION (1u << 0)  /**< a - reserved slot          */
#define RMM_ADMFLAG_GENERIC     (1u << 1)  /**< b - is an admin            */
#define RMM_ADMFLAG_KICK        (1u << 2)  /**< c                          */
#define RMM_ADMFLAG_BAN         (1u << 3)  /**< d                          */
#define RMM_ADMFLAG_UNBAN       (1u << 4)  /**< e                          */
#define RMM_ADMFLAG_SLAY        (1u << 5)  /**< f - kill/teleport/team swap */
#define RMM_ADMFLAG_CHANGEMAP   (1u << 6)  /**< g                          */
#define RMM_ADMFLAG_CONFIG      (1u << 7)  /**< h - server settings/vars   */
#define RMM_ADMFLAG_CHAT        (1u << 8)  /**< i - admin say, mute        */
#define RMM_ADMFLAG_VOTE        (1u << 9)  /**< j                          */
#define RMM_ADMFLAG_PASSWORD    (1u << 10) /**< k                          */
#define RMM_ADMFLAG_RCON        (1u << 11) /**< l - raw engine console     */
#define RMM_ADMFLAG_CHEATS      (1u << 12) /**< m - gravity, game speed... */
#define RMM_ADMFLAG_PLUGINS     (1u << 13) /**< n - load/unload plugins    */
#define RMM_ADMFLAG_ROOT        (1u << 25) /**< z - everything             */

/** Identity kinds for bans and admin entries. */
typedef enum rmm_identity {
    RMM_ID_AUTH = 0, /**< rmm_player_info::auth_id */
    RMM_ID_IP = 1,   /**< IP address without port  */
    RMM_ID_NAME = 2  /**< Player name (weak!)       */
} rmm_identity;

typedef struct rmm_admin_api {
    uint32_t struct_size;

    /** Effective flags for a player (ROOT expands to all). Console = all. */
    uint32_t(RMM_CALL* get_flags)(rmm_player_t player);
    /** True if the player has *all* of @p flags. */
    rmm_bool(RMM_CALL* has_flags)(rmm_player_t player, uint32_t flags);
    /** Immunity check: may @p admin act on @p target? */
    rmm_bool(RMM_CALL* can_target)(rmm_player_t admin, rmm_player_t target);
    /** Grant flags at runtime (not persisted), e.g. from a SQL plugin. */
    rmm_bool(RMM_CALL* grant)(rmm_player_t player, uint32_t flags, int32_t immunity);
    /** Convert "bcdz" <-> flags. */
    uint32_t(RMM_CALL* parse_flags)(const char* letters);

    rmm_bool(RMM_CALL* kick)(rmm_player_t player, const char* reason, rmm_player_t by);
    /** Ban a connected player by auth id (falls back to IP). @p minutes 0 = permanent. */
    rmm_bool(RMM_CALL* ban)(rmm_player_t player, int32_t minutes, const char* reason, rmm_player_t by);
    rmm_bool(RMM_CALL* ban_identity)(int32_t kind, const char* value, int32_t minutes, const char* reason,
                                     rmm_player_t by);
    rmm_bool(RMM_CALL* unban)(int32_t kind, const char* value);
    /** @return true if banned; reason copied to @p reason if non-NULL. */
    rmm_bool(RMM_CALL* is_banned)(int32_t kind, const char* value, char* reason, size_t cap);

    /** Re-read admins.cfg and bans.txt. */
    void(RMM_CALL* reload)(void);
} rmm_admin_api;

#ifdef __cplusplus
}
#endif

#endif /* RMM_ADMIN_H */
