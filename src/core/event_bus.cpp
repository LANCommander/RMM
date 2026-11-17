#include "event_bus.h"

#include "core.h"
#include "util.h"

#include <algorithm>

namespace rmm {

EventBus::EventBus() {
    static const struct {
        int32_t id;
        const char* name;
    } builtin[] = {
        {RMM_EV_SERVER_START, "server_start"},
        {RMM_EV_SERVER_SHUTDOWN, "server_shutdown"},
        {RMM_EV_FRAME, "frame"},
        {RMM_EV_MAP_LOAD, "map_load"},
        {RMM_EV_ROUND_START, "round_start"},
        {RMM_EV_ROUND_END, "round_end"},
        {RMM_EV_GAME_STATUS, "game_status"},
        {RMM_EV_PLAYER_CONNECT, "player_connect"},
        {RMM_EV_PLAYER_AUTH, "player_auth"},
        {RMM_EV_PLAYER_DISCONNECT, "player_disconnect"},
        {RMM_EV_PLAYER_SPAWN, "player_spawn"},
        {RMM_EV_PLAYER_KILLED, "player_killed"},
        {RMM_EV_PLAYER_DAMAGE, "player_damage"},
        {RMM_EV_PLAYER_TEAM, "player_team"},
        {RMM_EV_PLAYER_SQUAD, "player_squad"},
        {RMM_EV_PLAYER_SCORE, "player_score"},
        {RMM_EV_PLAYER_KIT_PICKUP, "player_kit_pickup"},
        {RMM_EV_PLAYER_KIT_DROP, "player_kit_drop"},
        {RMM_EV_PLAYER_REVIVED, "player_revived"},
        {RMM_EV_PLAYER_WEAPON_SWITCH, "player_weapon_switch"},
        {RMM_EV_CHAT, "chat"},
        {RMM_EV_RCON_COMMAND, "rcon_command"},
        {RMM_EV_VAR_CHANGED, "var_changed"},
        {RMM_EV_VEHICLE_ENTER, "vehicle_enter"},
        {RMM_EV_VEHICLE_EXIT, "vehicle_exit"},
        {RMM_EV_VEHICLE_DESTROYED, "vehicle_destroyed"},
        {RMM_EV_CONTROL_POINT, "control_point"},
        {RMM_EV_VEHICLE_DAMAGE, "vehicle_damage"},
        {RMM_EV_OBJECT_DESTROYED, "object_destroyed"},
        {RMM_EV_SPAWNER_SPAWN, "spawner_spawn"},
        {RMM_EV_BOT_ADDED, "bot_added"},
        {RMM_EV_BOT_REMOVED, "bot_removed"},
        {RMM_EV_BOT_ADD_FAILED, "bot_add_failed"},
        {RMM_EV_PLUGIN_LOADED, "plugin_loaded"},
        {RMM_EV_PLUGIN_UNLOADED, "plugin_unloaded"},
    };

    for (auto& b : builtin) {
        defs_[b.id] = EventDef{b.name, 0};
        byName_[b.name] = b.id;
    }
}

uint32_t EventBus::Flags(int32_t ev) const {
    auto it = defs_.find(ev);

    if (it == defs_.end())
        return 0;

    if (IsCustom(ev))
        return it->second.flags | RMM_EVF_SUPPORTED | RMM_EVF_CUSTOM;

    // Framework events are always available regardless of game.
    switch (ev) {
        case RMM_EV_SERVER_START:
        case RMM_EV_SERVER_SHUTDOWN:
        case RMM_EV_FRAME:
        case RMM_EV_VAR_CHANGED:
        case RMM_EV_PLUGIN_LOADED:
        case RMM_EV_PLUGIN_UNLOADED:
            return RMM_EVF_SUPPORTED;
        // Produced by the core's BotTracker from the adapter's bot list.
        case RMM_EV_BOT_ADDED:
        case RMM_EV_BOT_REMOVED:
            return Core::Get().Ready() && Core::Get().Game().HasCap(RMM_CAP_BOT_QUERY) ? RMM_EVF_SUPPORTED : 0;
        case RMM_EV_BOT_ADD_FAILED:
            return Core::Get().Ready() && Core::Get().Game().HasCap(RMM_CAP_BOT_ADD) ? RMM_EVF_SUPPORTED : 0;
        default:
            return flagsProvider_ ? flagsProvider_(ev) : 0;
    }
}

const char* EventBus::Name(int32_t ev) const {
    auto it = defs_.find(ev);

    return it == defs_.end() ? nullptr : it->second.name.c_str();
}

int32_t EventBus::Find(const std::string& name) const {
    auto it = byName_.find(ToLower(name));

    return it == byName_.end() ? RMM_EV_NONE : it->second;
}

std::vector<int32_t> EventBus::AllEvents() const {
    std::vector<int32_t> v;

    for (auto& d : defs_)
        v.push_back(d.first);

    std::sort(v.begin(), v.end());

    return v;
}

int32_t EventBus::RegisterCustom(Plugin* /*owner*/, const std::string& name, uint32_t flags) {
    std::string key = ToLower(name);
    auto it = byName_.find(key);

    if (it != byName_.end())
        return it->second;

    int32_t id = nextCustom_++;
    defs_[id] = EventDef{key, flags & (RMM_EVF_BLOCKABLE | RMM_EVF_MUTABLE)};
    byName_[key] = id;

    return id;
}

rmm_handle_t EventBus::Hook(Plugin* owner, int32_t ev, int32_t phase, int32_t priority, rmm_event_cb cb, void* user) {
    if (!cb || defs_.find(ev) == defs_.end())
        return RMM_INVALID_HANDLE;

    if (phase != RMM_PHASE_PRE && phase != RMM_PHASE_POST)
        return RMM_INVALID_HANDLE;

    HookRec r{nextHandle_++, owner, ev, phase, priority, seq_++, cb, user, false};

    if (depth_ > 0)
        pending_.push_back(r);  // never reallocate a vector being iterated
    else
        Insert(r);

    return r.h;
}

void EventBus::Insert(const HookRec& r) {
    auto& vec = hooks_[r.phase][r.ev];
    auto pos = std::find_if(vec.begin(), vec.end(), [&](const HookRec& o) {
        return o.priority < r.priority || (o.priority == r.priority && o.seq > r.seq);
    });
    vec.insert(pos, r);
}

bool EventBus::Unhook(Plugin* owner, rmm_handle_t h) {
    for (auto& phase : hooks_)
        for (auto& kv : phase)
            for (auto& r : kv.second)
                if (r.h == h && r.owner == owner && !r.dead) {
                    r.dead = true;
                    needCompact_ = true;

                    return true;
                }

    for (auto it = pending_.begin(); it != pending_.end(); ++it)
        if (it->h == h && it->owner == owner) {
            pending_.erase(it);

            return true;
        }

    return false;
}

void EventBus::RemoveAll(Plugin* owner) {
    for (auto& phase : hooks_)
        for (auto& kv : phase)
            for (auto& r : kv.second)
                if (r.owner == owner) {
                    r.dead = true;
                    needCompact_ = true;
                }

    pending_.erase(std::remove_if(pending_.begin(), pending_.end(), [&](const HookRec& r) { return r.owner == owner; }),
                   pending_.end());

    if (depth_ == 0)
        Compact();
}

void EventBus::Compact() {
    if (needCompact_) {
        for (auto& phase : hooks_)
            for (auto& kv : phase)
                kv.second.erase(
                    std::remove_if(kv.second.begin(), kv.second.end(), [](const HookRec& r) { return r.dead; }),
                    kv.second.end());

        needCompact_ = false;
    }

    for (auto& r : pending_)
        Insert(r);

    pending_.clear();
}

bool EventBus::HasHooks(int32_t ev) const {
    for (const auto& phase : hooks_) {
        auto it = phase.find(ev);

        if (it == phase.end())
            continue;

        for (const auto& r : it->second)
            if (!r.dead)
                return true;
    }

    for (const auto& r : pending_)
        if (r.ev == ev)
            return true;

    return false;
}

int32_t EventBus::RunPhase(int32_t ev, int32_t phase, void* data, bool stopOnStop) {
    auto it = hooks_[phase].find(ev);

    if (it == hooks_[phase].end())
        return RMM_CONTINUE;

    Core& core = Core::Get();
    int32_t best = RMM_CONTINUE;

    // Index-based: vector is never resized while depth_ > 0 (see Hook()).
    for (size_t i = 0; i < it->second.size(); ++i) {
        const HookRec r = it->second[i];

        if (r.dead)
            continue;

        if (r.owner && r.owner->state != PluginState::Running)
            continue;  // paused/failed

        int32_t res = RMM_CONTINUE;
        core.CallPlugin(r.owner, "event handler", [&] { res = r.cb(ev, phase, data, r.user); });

        if (res < RMM_CONTINUE || res > RMM_STOP)
            res = RMM_CONTINUE;

        if (res > best)
            best = res;

        if (stopOnStop && res == RMM_STOP)
            break;
    }

    return best;
}

int32_t EventBus::Dispatch(int32_t ev, void* data) {
    uint32_t flags = Flags(ev);
    ++depth_;
    int32_t result = RunPhase(ev, RMM_PHASE_PRE, data, (flags & RMM_EVF_BLOCKABLE) != 0);

    if (!(flags & RMM_EVF_BLOCKABLE) && result > RMM_CHANGED)
        result = RMM_CHANGED;

    if (result < RMM_HANDLED)
        RunPhase(ev, RMM_PHASE_POST, data, false);

    --depth_;

    if (depth_ == 0 && (needCompact_ || !pending_.empty()))
        Compact();

    return result;
}

int32_t EventBus::DispatchPre(int32_t ev, void* data, bool canBlock) {
    bool blockable = canBlock && (Flags(ev) & RMM_EVF_BLOCKABLE) != 0;
    ++depth_;
    int32_t result = RunPhase(ev, RMM_PHASE_PRE, data, blockable);

    if (!blockable && result > RMM_CHANGED)
        result = RMM_CHANGED;

    --depth_;

    if (depth_ == 0 && (needCompact_ || !pending_.empty()))
        Compact();

    return result;
}

void EventBus::DispatchPost(int32_t ev, void* data) {
    ++depth_;
    RunPhase(ev, RMM_PHASE_POST, data, false);
    --depth_;

    if (depth_ == 0 && (needCompact_ || !pending_.empty()))
        Compact();
}

}  // namespace rmm
