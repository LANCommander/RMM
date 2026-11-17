/**
 * @file rmm.h
 * @brief Umbrella header for Refractor MultiMod plugins (C API).
 *
 * Minimal plugin:
 * @code
 * #include <rmm/rmm.h>
 *
 * static const rmm_api* g_api;
 * static rmm_plugin_t   g_self;
 * static const rmm_plugin_info g_info = {
 *     RMM_PLUGIN_INFO_HEADER, "Hello", "1.0", "me", "Says hello", "", "hello",
 *     RMM_GAME_ANY, 0 };
 *
 * RMM_PLUGIN_EXPORT const rmm_plugin_info* RMM_CALL RMM_Query(void) { return &g_info; }
 * RMM_PLUGIN_EXPORT rmm_bool RMM_CALL RMM_Load(const rmm_api* api, rmm_plugin_t self,
 *                                              rmm_bool late, char* err, size_t cap) {
 *     g_api = api; g_self = self;
 *     rmm_logf(api, self, RMM_LOG_INFO, "Hello from %s", api->game->name());
 *     return RMM_TRUE;
 * }
 * @endcode
 *
 * C++ plugins may prefer <rmm/rmm.hpp>.
 */
#ifndef RMM_H
#define RMM_H

#include <stdarg.h>
#include <stdio.h>

#include "rmm_types.h"
#include "rmm_events.h"
#include "rmm_vars.h"
#include "rmm_commands.h"
#include "rmm_players.h"
#include "rmm_admin.h"
#include "rmm_game.h"
#include "rmm_world.h"
#include "rmm_bots.h"
#include "rmm_api.h"
#include "rmm_plugin.h"

#if defined(__GNUC__)
#    define RMM_PRINTF_ATTR(f, a) __attribute__((format(printf, f, a)))
#else
#    define RMM_PRINTF_ATTR(f, a)
#endif

/** printf-style logging helper. Formatting happens in the plugin, so no
 *  va_list ever crosses the ABI boundary. */
static inline void rmm_logf(const rmm_api* api, rmm_plugin_t self, int32_t level, const char* fmt, ...)
    RMM_PRINTF_ATTR(4, 5);

static inline void rmm_logf(const rmm_api* api, rmm_plugin_t self, int32_t level, const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    api->core->log(self, level, buf);
}

/** printf-style command reply helper. */
static inline void rmm_replyf(const rmm_api* api, const rmm_cmd_ctx* ctx, const char* fmt, ...) RMM_PRINTF_ATTR(3, 4);

static inline void rmm_replyf(const rmm_api* api, const rmm_cmd_ctx* ctx, const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    api->cmds->reply(ctx, buf);
}

#endif /* RMM_H */
