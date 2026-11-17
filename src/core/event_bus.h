#pragma once

#include "plugin.h"

#include <rmm/rmm_events.h>

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace rmm {

class EventBus {
public:
    EventBus();

    /// Supplies RMM_EVF_* flags for built-in events (from the game adapter).
    void SetFlagsProvider(std::function<uint32_t(int32_t)> fn) { flagsProvider_ = std::move(fn); }

    rmm_handle_t Hook(Plugin* owner, int32_t ev, int32_t phase, int32_t priority, rmm_event_cb cb, void* user);
    bool Unhook(Plugin* owner, rmm_handle_t h);
    void RemoveAll(Plugin* owner);

    uint32_t Flags(int32_t ev) const;
    const char* Name(int32_t ev) const;
    int32_t Find(const std::string& name) const;
    int32_t RegisterCustom(Plugin* owner, const std::string& name, uint32_t flags);

    bool IsCustom(int32_t ev) const { return ev >= RMM_EV_CUSTOM_BASE; }

    std::vector<int32_t> AllEvents() const;

    /// Run PRE handlers then (unless blocked) POST handlers.
    /// @return the effective PRE result: never above RMM_CHANGED unless the
    ///         event is blockable on this game.
    int32_t Dispatch(int32_t ev, void* data);

    /// Split dispatch for hooks that run PRE handlers before the engine acts
    /// and POST handlers after it did: DispatchPre, then (only if the result
    /// is below RMM_HANDLED and the action happened) DispatchPost.
    /// @p canBlock = false makes this one instance unblockable even if the
    /// event is blockable on this game (e.g. a kit handed out at spawn).
    int32_t DispatchPre(int32_t ev, void* data, bool canBlock = true);
    void DispatchPost(int32_t ev, void* data);

    int Depth() const { return depth_; }

    /// Any live handler for @p ev (either phase)? Lets hot engine hooks skip
    /// building payloads nobody will read.
    bool HasHooks(int32_t ev) const;

private:
    struct HookRec {
        rmm_handle_t h;
        Plugin* owner;
        int32_t ev;
        int32_t phase;
        int32_t priority;
        uint64_t seq;
        rmm_event_cb cb;
        void* user;
        bool dead;
    };

    int32_t RunPhase(int32_t ev, int32_t phase, void* data, bool stopOnStop);
    void Insert(const HookRec& r);
    void Compact();

    std::unordered_map<int32_t, std::vector<HookRec>> hooks_[2];
    std::vector<HookRec> pending_;  // hooks added during dispatch

    struct EventDef {
        std::string name;
        uint32_t flags;
    };

    std::unordered_map<int32_t, EventDef> defs_;
    std::unordered_map<std::string, int32_t> byName_;
    std::function<uint32_t(int32_t)> flagsProvider_;
    int32_t nextCustom_ = RMM_EV_CUSTOM_BASE;
    rmm_handle_t nextHandle_ = 1;
    uint64_t seq_ = 0;
    int depth_ = 0;
    bool needCompact_ = false;
};

}  // namespace rmm
