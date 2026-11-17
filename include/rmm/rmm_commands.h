/**
 * @file rmm_commands.h
 * @brief Admin/console/chat commands, and raw engine console execution.
 *
 * A command registered with RMM is reachable from every entry point the
 * running game supports, with one implementation:
 *  - Server console / RCon:  `rmm <name> args...`  (and bare `<name>` where the
 *                            adapter can intercept unknown console commands)
 *  - In-game chat:           `!name args` (public)  or  `/name args` (silent,
 *                            hidden from chat where the game allows blocking)
 *    The triggers are configurable via `rmm.chatTrigger` / `rmm.silentTrigger`.
 *
 * Arguments are split Refractor-style: whitespace separated, with
 * double-quoted strings kept together.
 */
#ifndef RMM_COMMANDS_H
#define RMM_COMMANDS_H

#include "rmm_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Where a command may be invoked from. */
#define RMM_CMD_CONSOLE 0x01u /**< Server console and RCon. */
#define RMM_CMD_CHAT    0x02u /**< Chat triggers. */
#define RMM_CMD_ALL     (RMM_CMD_CONSOLE | RMM_CMD_CHAT)

/** Opaque invocation context, valid only during the callback. */
typedef struct rmm_cmd_ctx rmm_cmd_ctx;

typedef void(RMM_CALL* rmm_cmd_cb)(const rmm_cmd_ctx* ctx, void* user);

typedef struct rmm_cmds_api {
    uint32_t struct_size;

    /**
         * Register a command.
         * @param name        Lower-case name without trigger, e.g. "kick".
         * @param flags       RMM_CMD_* entry points.
         * @param admin_flags Required RMM_ADMFLAG_* bits (0 = anyone). Console and
         *                    RCon callers are treated as root.
         * @param description One line shown by `rmm cmds` and !help.
         * @param usage       e.g. "<target> [reason]".
         * @return handle, or RMM_INVALID_HANDLE if the name is taken.
         */
    rmm_handle_t(RMM_CALL* add)(rmm_plugin_t self, const char* name, uint32_t flags, uint32_t admin_flags,
                                const char* description, const char* usage, rmm_cmd_cb cb, void* user);
    rmm_bool(RMM_CALL* remove)(rmm_plugin_t self, rmm_handle_t cmd);

    /* ---- context accessors (only valid inside a command callback) ------ */

    /** Number of arguments, *excluding* the command name. */
    int32_t(RMM_CALL* argc)(const rmm_cmd_ctx* ctx);
    /** Argument @p i (0 = command name, 1.. = args). "" if out of range. */
    const char*(RMM_CALL* argv)(const rmm_cmd_ctx* ctx, int32_t i);
    /** Everything after the command name, unsplit. */
    const char*(RMM_CALL* args)(const rmm_cmd_ctx* ctx);
    /** Arguments from index @p first joined with spaces (e.g. a reason). */
    const char*(RMM_CALL* args_from)(const rmm_cmd_ctx* ctx, int32_t first);
    /** Calling player, or RMM_PLAYER_NONE for console/RCon/plugins. */
    rmm_player_t(RMM_CALL* caller)(const rmm_cmd_ctx* ctx);
    /** ::rmm_cmd_source */
    int32_t(RMM_CALL* source)(const rmm_cmd_ctx* ctx);
    /** Send a reply to whoever invoked the command (console output, RCon
         *  response, or a private/chat message to the player). Chat replies are
         *  paced with the defaults; use reply_ex to control that. */
    void(RMM_CALL* reply)(const rmm_cmd_ctx* ctx, const char* message);

    /* ---- execution ------------------------------------------------------ */

    /**
         * Execute a raw engine console command, e.g. "admin.restartMap".
         * @param out Optional buffer for the console output (may be NULL).
         * @return false if the console is unavailable on this game/backend.
         */
    rmm_bool(RMM_CALL* engine_exec)(const char* command, char* out, size_t cap);

    /**
         * Execute a command line through RMM as if typed at the console: RMM
         * commands and `rmm ...` are handled by RMM, anything else is passed to
         * the engine. Used for config files.
         */
    rmm_bool(RMM_CALL* exec)(rmm_plugin_t self, const char* command_line, char* out, size_t cap);

    /** reply() with pacing options (see ::rmm_msg_opts; NULL = defaults).
         *  Console and RCon replies ignore @p opts and are returned at once. */
    void(RMM_CALL* reply_ex)(const rmm_cmd_ctx* ctx, const char* message, const rmm_msg_opts* opts);
} rmm_cmds_api;

#ifdef __cplusplus
}
#endif

#endif /* RMM_COMMANDS_H */
