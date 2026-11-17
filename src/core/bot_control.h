// Bot input control: who drives which bot, the input a plugin
// latched for it, and the input the engine's AI produced on its last tick.
//
// Adapters call Filter() from their engine hook once per bot input tick (BF1942:
// AIPlayer::addInput). That is the hot path: no allocation once a bot is known,
// no plugin calls, no console traffic.
#pragma once

#include "plugin.h"

#include <rmm/rmm_api.h>

#include <cstdint>
#include <map>
#include <string>

namespace rmm {

class BotControl {
public:
    /// One input record in engine slot numbering.
    struct Frame {
        uint64_t mask = 0;
        float value[RMM_BOT_INPUT_SLOTS] = {};
    };

    /// @p owner nullptr = the server console (may always take over).
    bool SetMode(Plugin* owner, rmm_player_t p, int32_t mode, std::string* err);
    int32_t Mode(rmm_player_t p, Plugin** owner = nullptr) const;
    bool SetInput(Plugin* owner, rmm_player_t p, const rmm_bot_input& in, double now, std::string* err);
    bool EngineInput(rmm_player_t p, rmm_bot_input& out, double now) const;

    /// Adapter hook: @p f holds the engine AI's input (recorded for
    /// EngineInput). @return true if @p f was replaced/changed and the adapter
    /// must hand the changed record to the engine instead.
    bool Filter(rmm_player_t p, Frame& f, double now);

    /// The player left (slot ids are reused): forget everything about it.
    void Forget(rmm_player_t p);
    /// Release every bot @p owner controls (plugin unload).
    void ReleaseAll(Plugin* owner);

    /// Map change: release all control, drop recorded input.
    void Clear() { slots_.clear(); }

    size_t Controlled() const;

private:
    struct Slot {
        Plugin* owner = nullptr;
        int32_t mode = RMM_BOT_CONTROL_ENGINE;
        Frame input;  // latched by set_input
        bool hasInput = false;
        double expires = 0;  // > 0: input stops applying at this core time
        Frame engine;        // last engine AI input
        double engineAt = -1;
        uint32_t ticks = 0;
    };

    std::map<rmm_player_t, Slot> slots_;
};

/// Lower-case name of a Refractor 1 input slot ("throttle", "fire", "menu_select_3"),
/// "" when out of range.
std::string BotInputSlotName(int slot);
/// Slot by name (case-insensitive, "_" optional) or number; -1 if unknown.
int BotInputSlotFromName(const std::string& name);

}  // namespace rmm
