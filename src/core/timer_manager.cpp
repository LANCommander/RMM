#include "timer_manager.h"

#include "core.h"

#include <algorithm>

namespace rmm {

rmm_handle_t TimerManager::Create(Plugin* owner, double interval, uint32_t flags, rmm_timer_cb cb, void* user,
                                  double now) {
    if (!cb)
        return RMM_INVALID_HANDLE;

    if (interval < 0.01)
        interval = 0.01;

    Timer t{nextHandle_++, owner, interval, now + interval, flags, cb, user, false};
    timers_.push_back(t);  // Process() iterates by index, so growth is safe

    return t.h;
}

bool TimerManager::Kill(Plugin* owner, rmm_handle_t h) {
    for (auto& t : timers_)
        if (t.h == h && t.owner == owner && !t.dead) {
            t.dead = true;

            return true;
        }

    return false;
}

void TimerManager::RemoveAll(Plugin* owner) {
    for (auto& t : timers_)
        if (t.owner == owner)
            t.dead = true;
}

void TimerManager::OnMapChange() {
    for (auto& t : timers_)
        if (t.flags & RMM_TIMER_MAP_CHANGE)
            t.dead = true;
}

void TimerManager::Process(double now) {
    if (processing_)
        return;

    processing_ = true;
    Core& core = Core::Get();

    for (size_t i = 0; i < timers_.size(); ++i) {
        if (timers_[i].dead || now < timers_[i].next)
            continue;

        Timer t = timers_[i];

        if (t.owner && t.owner->state == PluginState::Paused) {
            timers_[i].next = now + t.interval;  // keep cadence, skip while paused
            continue;
        }

        if (t.owner && t.owner->state != PluginState::Running) {
            timers_[i].dead = true;
            continue;
        }

        rmm_bool keep = RMM_FALSE;
        bool ok = core.CallPlugin(t.owner, "timer", [&] { keep = t.cb(t.h, t.user); });
        // Re-index: the callback may have created timers (vector may have moved).
        Timer& cur = timers_[i];

        if (!ok || !(t.flags & RMM_TIMER_REPEAT) || !keep)
            cur.dead = true;
        else if (!cur.dead)
            cur.next = now + t.interval;
    }

    timers_.erase(std::remove_if(timers_.begin(), timers_.end(), [](const Timer& t) { return t.dead; }), timers_.end());
    processing_ = false;
}

}  // namespace rmm
