#include "bot_control.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace rmm {

namespace {

float Clean(float v) { return std::isfinite(v) ? v : 0.0f; }

bool MayChange(Plugin* current, int32_t mode, Plugin* caller) {
    return mode == RMM_BOT_CONTROL_ENGINE || current == caller || caller == nullptr;
}

}  // namespace

bool BotControl::SetMode(Plugin* owner, rmm_player_t p, int32_t mode, std::string* err) {
    if (mode < RMM_BOT_CONTROL_ENGINE || mode > RMM_BOT_CONTROL_EXCLUSIVE) {
        if (err)
            *err = "unknown control mode";

        return false;
    }

    auto it = slots_.find(p);

    if (it != slots_.end() && !MayChange(it->second.owner, it->second.mode, owner)) {
        if (err)
            *err = "another plugin controls this bot";

        return false;
    }

    Slot& s = slots_[p];

    if (mode == RMM_BOT_CONTROL_ENGINE) {
        s.owner = nullptr;
        s.mode = mode;
        s.hasInput = false;

        return true;
    }

    if (s.owner != owner || s.mode == RMM_BOT_CONTROL_ENGINE)
        s.hasInput = false;  // a new controller starts clean

    s.owner = owner;
    s.mode = mode;

    return true;
}

int32_t BotControl::Mode(rmm_player_t p, Plugin** owner) const {
    auto it = slots_.find(p);

    if (owner)
        *owner = it == slots_.end() ? nullptr : it->second.owner;

    return it == slots_.end() ? RMM_BOT_CONTROL_ENGINE : it->second.mode;
}

bool BotControl::SetInput(Plugin* owner, rmm_player_t p, const rmm_bot_input& in, double now, std::string* err) {
    auto it = slots_.find(p);

    if (it == slots_.end() || it->second.mode == RMM_BOT_CONTROL_ENGINE) {
        if (err)
            *err = "take control of the bot first (set_control)";

        return false;
    }

    Slot& s = it->second;

    if (s.owner != owner && owner != nullptr) {
        if (err)
            *err = "another plugin controls this bot";

        return false;
    }

    s.input.mask = in.mask;

    for (int i = 0; i < RMM_BOT_INPUT_SLOTS; ++i)
        s.input.value[i] = (in.mask >> i) & 1u ? Clean(in.value[i]) : 0.0f;

    s.hasInput = true;
    s.expires = in.hold_ms ? now + in.hold_ms / 1000.0 : 0;

    return true;
}

bool BotControl::EngineInput(rmm_player_t p, rmm_bot_input& out, double now) const {
    auto it = slots_.find(p);

    if (it == slots_.end() || it->second.engineAt < 0)
        return false;

    const Slot& s = it->second;
    out.mask = s.engine.mask;
    std::memcpy(out.value, s.engine.value, sizeof out.value);
    out.hold_ms = static_cast<uint32_t>(std::clamp((now - s.engineAt) * 1000.0, 0.0, 4.0e9));
    out.tick = s.ticks;

    return true;
}

bool BotControl::Filter(rmm_player_t p, Frame& f, double now) {
    Slot& s = slots_[p];
    s.engine = f;
    s.engineAt = now;
    ++s.ticks;

    if (s.mode == RMM_BOT_CONTROL_ENGINE)
        return false;

    bool live = s.hasInput && (s.expires <= 0 || now < s.expires);

    if (s.mode == RMM_BOT_CONTROL_EXCLUSIVE) {
        // Only the controller's slots are "mapped"; everything else reads as released.
        f = live ? s.input : Frame{};

        return true;
    }

    if (!live)
        return false;  // override with nothing to override: the AI's own input

    for (int i = 0; i < RMM_BOT_INPUT_SLOTS; ++i)
        if ((s.input.mask >> i) & 1u)
            f.value[i] = s.input.value[i];

    f.mask |= s.input.mask;

    return true;
}

void BotControl::Forget(rmm_player_t p) { slots_.erase(p); }

void BotControl::ReleaseAll(Plugin* owner) {
    for (auto& kv : slots_)
        if (kv.second.owner == owner && kv.second.mode != RMM_BOT_CONTROL_ENGINE) {
            kv.second.owner = nullptr;
            kv.second.mode = RMM_BOT_CONTROL_ENGINE;
            kv.second.hasInput = false;
        }
}

namespace {
const char* const kSlotNames[RMM_BI_COUNT_R1] = {
    "yaw",
    "pitch",
    "roll",
    "throttle",
    "mouse_look_x",
    "mouse_look_y",
    "camera_x",
    "camera_y",
    "fire",
    "action",
    "use",
    "mouse_look",
    "walk",
    "run",
    "menu_select_1",
    "menu_select_2",
    "menu_select_3",
    "menu_select_4",
    "menu_select_5",
    "menu_select_6",
    "menu_select_7",
    "menu_select_8",
    "menu_select_9",
    "alt_fire",
    "reload",
    "drop",
    "toggle_camera_mode",
    "toggle_camera",
    "lie",
    "crouch",
    "camera_mode_1",
    "camera_mode_2",
    "camera_mode_3",
    "camera_mode_4",
    "radio_1",
    "radio_2",
    "radio_3",
    "radio_4",
    "radio_5",
    "radio_6",
    "radio_7",
    "radio_8",
    "screenshot",
    "tooltip",
    "say_all",
    "say_team",
    "next_item",
    "prev_item",
    "communication",
    "show_scoreboard",
    "map",
    "zoom_map",
    "show_map_vote",
    "vote_yes",
    "vote_no",
};

static_assert(RMM_BI_FIRE == 8 && RMM_BI_ALT_FIRE == 23 && RMM_BI_LIE == 28 && RMM_BI_RADIO_1 == 34 &&
                  RMM_BI_SCREENSHOT == 42 && RMM_BI_NEXT_ITEM == 46 && RMM_BI_VOTE_NO == 54,
              "slot numbering");

std::string Squash(const std::string& s) {
    std::string t;

    for (char c : s)
        if (c != '_' && c != '-')
            t += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    return t;
}
}  // namespace

std::string BotInputSlotName(int slot) { return slot >= 0 && slot < RMM_BI_COUNT_R1 ? kSlotNames[slot] : ""; }

int BotInputSlotFromName(const std::string& name) {
    if (name.empty())
        return -1;

    if (std::all_of(name.begin(), name.end(), [](char c) { return c >= '0' && c <= '9'; })) {
        int n = std::atoi(name.c_str());

        return n < RMM_BOT_INPUT_SLOTS ? n : -1;
    }

    std::string want = Squash(name);

    for (int i = 0; i < RMM_BI_COUNT_R1; ++i)
        if (Squash(kSlotNames[i]) == want)
            return i;

    return -1;
}

size_t BotControl::Controlled() const {
    return static_cast<size_t>(std::count_if(slots_.begin(), slots_.end(),
                                             [](const auto& kv) { return kv.second.mode != RMM_BOT_CONTROL_ENGINE; }));
}

}  // namespace rmm
