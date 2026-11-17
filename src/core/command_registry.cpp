#include "command_registry.h"

#include "core.h"
#include "util.h"

#include <cctype>
#include <iterator>

namespace rmm {

rmm_handle_t CommandRegistry::Add(Plugin* owner, const std::string& name, uint32_t flags, uint32_t adminFlags,
                                  const std::string& desc, const std::string& usage, rmm_cmd_cb cb, void* user) {
    std::string key = ToLower(Trim(name));

    if (key.empty() || !cb || key.find(' ') != std::string::npos)
        return RMM_INVALID_HANDLE;

    if (cmds_.count(key))
        return RMM_INVALID_HANDLE;

    Command c;
    c.h = nextHandle_++;
    c.owner = owner;
    c.name = key;
    c.flags = flags ? flags : RMM_CMD_ALL;
    c.adminFlags = adminFlags;
    c.description = desc;
    c.usage = usage;
    c.cb = cb;
    c.user = user;
    cmds_[key] = c;

    return c.h;
}

bool CommandRegistry::Remove(Plugin* owner, rmm_handle_t h) {
    for (auto it = cmds_.begin(); it != cmds_.end(); ++it)
        if (it->second.h == h && it->second.owner == owner) {
            cmds_.erase(it);

            return true;
        }

    return false;
}

void CommandRegistry::RemoveAll(Plugin* owner) {
    for (auto it = cmds_.begin(); it != cmds_.end();)
        it = (it->second.owner == owner) ? cmds_.erase(it) : std::next(it);
}

const Command* CommandRegistry::Find(const std::string& name) const {
    auto it = cmds_.find(ToLower(name));

    return it == cmds_.end() ? nullptr : &it->second;
}

std::vector<const Command*> CommandRegistry::List() const {
    std::vector<const Command*> v;

    for (auto& kv : cmds_)
        v.push_back(&kv.second);

    return v;
}

static std::string AfterFirstToken(const std::string& line) {
    size_t i = 0, n = line.size();

    while (i < n && std::isspace(static_cast<unsigned char>(line[i])))
        ++i;

    if (i < n && line[i] == '"') {
        ++i;

        while (i < n && line[i] != '"')
            ++i;

        if (i < n)
            ++i;
    } else {
        while (i < n && !std::isspace(static_cast<unsigned char>(line[i])))
            ++i;
    }

    return Trim(line.substr(i));
}

bool CommandRegistry::Execute(int32_t source, rmm_player_t caller, const std::string& line, std::string* output,
                              bool silent) {
    auto argv = SplitArgs(line);

    if (argv.empty())
        return false;

    auto it = cmds_.find(ToLower(argv[0]));

    if (it == cmds_.end())
        return false;

    // Copy: the registry may change while the command runs (e.g. unload).
    const Command cmd = it->second;
    Core& core = Core::Get();

    rmm_cmd_ctx ctx;
    ctx.source = source;
    ctx.caller = caller;
    ctx.argv = std::move(argv);
    ctx.argv[0] = cmd.name;
    ctx.raw = AfterFirstToken(line);
    ctx.output = output;
    ctx.silent = silent;

    const bool fromChat = source == RMM_SRC_CHAT;

    if (fromChat && !(cmd.flags & RMM_CMD_CHAT))
        return false;  // let chat through untouched

    if (!fromChat && !(cmd.flags & RMM_CMD_CONSOLE)) {
        Reply(&ctx, "This command can only be used from chat.");

        return true;
    }

    if (caller != RMM_PLAYER_NONE && cmd.adminFlags && !core.Admin().HasFlags(caller, cmd.adminFlags)) {
        Reply(&ctx, "You do not have access to this command.");

        return true;
    }

    if (cmd.owner && cmd.owner->state != PluginState::Running) {
        Reply(&ctx, Format("Command '%s' is unavailable (plugin %s is %s).", cmd.name.c_str(), cmd.owner->name.c_str(),
                           PluginStateName(cmd.owner->state)));

        return true;
    }

    core.CallPlugin(cmd.owner, "command handler", [&] { cmd.cb(&ctx, cmd.user); });

    return true;
}

void CommandRegistry::Reply(const rmm_cmd_ctx* ctx, const std::string& msg, double interval, bool immediate) {
    Core& core = Core::Get();

    if (ctx->caller == RMM_PLAYER_NONE) {
        if (ctx->output) {
            if (!ctx->output->empty())
                *ctx->output += "\n";

            *ctx->output += msg;
        } else {
            core.Log().Write(RMM_LOG_INFO, "reply", msg);
        }

        return;
    }

    // No private messaging (or it fails): fall back according to rmm.replyFallback.
    std::string prefix;

    if (core.CoreVarInt("rmm.replyFallback") != 0) {
        rmm_player_info info{};
        info.struct_size = sizeof info;
        std::string who = core.Game().GetPlayer(ctx->caller, info) ? info.name : "player";
        prefix = Format("[RMM] @%s: ", who.c_str());
    }

    if (core.Game().HasCap(RMM_CAP_PRIVATE_MESSAGE))
        core.Chat().Send(ChatQueue::Kind::Player, ctx->caller, msg, interval, immediate, prefix);
    else if (!prefix.empty())
        core.Chat().Send(ChatQueue::Kind::All, 0, prefix + msg, interval, immediate);
}

bool CommandRegistry::HandleChat(rmm_player_t player, const std::string& text, bool& hide) {
    Core& core = Core::Get();
    std::string pub = core.CoreVarString("rmm.chatTrigger");
    std::string sil = core.CoreVarString("rmm.silentTrigger");
    std::string t = Trim(text);
    hide = false;
    std::string body;

    if (!sil.empty() && StartsWith(t, sil)) {
        body = t.substr(sil.size());
        hide = true;
    } else if (!pub.empty() && StartsWith(t, pub)) {
        body = t.substr(pub.size());
    } else
        return false;

    if (body.empty() || std::isspace(static_cast<unsigned char>(body[0]))) {
        hide = false;

        return false;
    }

    bool ran = Execute(RMM_SRC_CHAT, player, body, nullptr, hide);

    if (!ran)
        hide = false;

    return ran;
}

}  // namespace rmm
