/*
 * hello - the smallest useful RMM plugin, in plain C.
 * Greets players when they are authenticated and adds a "!hello" command.
 */
#include <rmm/rmm.h>

#include <string.h>

static const rmm_api* g_api;
static rmm_plugin_t g_self;
static rmm_handle_t g_greeting;

static const rmm_plugin_info g_info = {
    RMM_PLUGIN_INFO_HEADER,
    "Hello",                             /* name        */
    "1.0.0",                             /* version     */
    "RMM Team",                          /* author      */
    "Greets players; minimal C example", /* description */
    "",                                  /* url         */
    "hello",                             /* log tag     */
    RMM_GAME_ANY,                        /* games       */
    0                                    /* flags       */
};

static int32_t RMM_CALL on_auth(int32_t ev, int32_t phase, void* data, void* user) {
    rmm_ev_player* e = (rmm_ev_player*)data;
    rmm_player_info info;
    char greeting[128], msg[256];
    (void)ev;
    (void)phase;
    (void)user;

    memset(&info, 0, sizeof info);
    info.struct_size = sizeof info;

    if (!g_api->players->get_info(e->player, &info))
        return RMM_CONTINUE;

    g_api->vars->get_string(g_greeting, greeting, sizeof greeting);
    snprintf(msg, sizeof msg, "%s %s", greeting, info.name);
    g_api->game->say_all(msg);

    return RMM_CONTINUE;
}

static void RMM_CALL cmd_hello(const rmm_cmd_ctx* ctx, void* user) {
    (void)user;
    rmm_replyf(g_api, ctx, "Hello from RMM %s on %s!", g_api->core->version(), g_api->game->name());
}

RMM_PLUGIN_EXPORT const rmm_plugin_info* RMM_CALL RMM_Query(void) { return &g_info; }

RMM_PLUGIN_EXPORT rmm_bool RMM_CALL RMM_Load(const rmm_api* api, rmm_plugin_t self, rmm_bool late, char* error,
                                             size_t error_cap) {
    (void)late;
    (void)error;
    (void)error_cap;
    g_api = api;
    g_self = self;

    g_greeting = api->vars->create(self, "hello.greeting", "Welcome,", "Greeting prefix", RMM_VAR_ARCHIVE);
    api->events->hook(self, RMM_EV_PLAYER_AUTH, RMM_PHASE_POST, RMM_PRIORITY_NORMAL, on_auth, NULL);
    api->cmds->add(self, "hello", RMM_CMD_ALL, 0, "Say hello", "", cmd_hello, NULL);

    rmm_logf(api, self, RMM_LOG_INFO, "loaded on %s (%s backend)", api->game->name(), api->game->backend());

    return RMM_TRUE;
}

RMM_PLUGIN_EXPORT void RMM_CALL RMM_Unload(void) { /* Nothing to do: hooks, vars and commands are removed by RMM. */ }
