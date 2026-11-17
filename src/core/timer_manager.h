#pragma once

#include "plugin.h"

#include <rmm/rmm_api.h>

#include <vector>

namespace rmm {

class TimerManager {
public:
    rmm_handle_t Create(Plugin* owner, double interval, uint32_t flags, rmm_timer_cb cb, void* user, double now);
    bool Kill(Plugin* owner, rmm_handle_t h);
    void RemoveAll(Plugin* owner);
    void OnMapChange();
    void Process(double now);

private:
    struct Timer {
        rmm_handle_t h;
        Plugin* owner;
        double interval;
        double next;
        uint32_t flags;
        rmm_timer_cb cb;
        void* user;
        bool dead;
    };

    std::vector<Timer> timers_;
    rmm_handle_t nextHandle_ = 1;
    bool processing_ = false;
};

}  // namespace rmm
