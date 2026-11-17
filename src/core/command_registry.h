#pragma once

#include "plugin.h"

#include <rmm/rmm_commands.h>

#include <map>
#include <string>
#include <vector>

/// Concrete invocation context behind the opaque rmm_cmd_ctx.
struct rmm_cmd_ctx {
    int32_t source = RMM_SRC_CONSOLE;
    rmm_player_t caller = RMM_PLAYER_NONE;
    std::vector<std::string> argv;  // argv[0] = command name
    std::string raw;                // everything after the name
    std::string* output = nullptr;  // console/RCon response buffer
    bool silent = false;
    mutable std::string scratch;  // backing store for args_from()
};

namespace rmm {

struct Command {
    rmm_handle_t h = 0;
    Plugin* owner = nullptr;
    std::string name;
    uint32_t flags = 0;
    uint32_t adminFlags = 0;
    std::string description, usage;
    rmm_cmd_cb cb = nullptr;
    void* user = nullptr;
};

class CommandRegistry {
public:
    rmm_handle_t Add(Plugin* owner, const std::string& name, uint32_t flags, uint32_t adminFlags,
                     const std::string& desc, const std::string& usage, rmm_cmd_cb cb, void* user);
    bool Remove(Plugin* owner, rmm_handle_t h);
    void RemoveAll(Plugin* owner);
    const Command* Find(const std::string& name) const;
    std::vector<const Command*> List() const;

    /// Try to run @p line as an RMM command. Returns false if the first token
    /// is not an RMM command (caller should pass it on to the engine).
    bool Execute(int32_t source, rmm_player_t caller, const std::string& line, std::string* output,
                 bool silent = false);

    /// Deliver a reply according to the context's source. Replies to a player
    /// go through the chat queue: @p interval < 0 = rmm.chatInterval,
    /// @p immediate skips the queue. Console/RCon replies are never paced.
    void Reply(const rmm_cmd_ctx* ctx, const std::string& msg, double interval = -1, bool immediate = false);

    /// Check whether a chat line is a command trigger and run it.
    /// @return true if it was a command; @p hide is set for silent triggers.
    bool HandleChat(rmm_player_t player, const std::string& text, bool& hide);

private:
    std::map<std::string, Command> cmds_;  // lower-case name
    rmm_handle_t nextHandle_ = 1;
};

}  // namespace rmm
