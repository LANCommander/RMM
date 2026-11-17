// Paced delivery of in-game chat: say_all, say_team, private messages and
// command replies that go to a player.
//
// Refractor's chat box shows a few lines for a few seconds, so a burst of
// lines (e.g. a multi-line command reply sent in one frame) scrolls away
// before it can be read. Lines are queued per destination (everyone, a team,
// one player) and released no faster than one per interval. The interval is
// rmm.chatInterval by default and can be overridden per line through the
// *_ex functions (rmm_msg_opts). Order per destination is preserved.
#pragma once

#include <rmm/rmm_types.h>

#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace rmm {

class ChatQueue {
public:
    enum class Kind { All = 0, Team = 1, Player = 2 };

    /// Queue or send a message. Messages longer than the game's chat line
    /// limit are split at word boundaries; every piece is paced. @p interval =
    /// seconds before the next line to the same destination may be shown
    /// (<0 = rmm.chatInterval). @p immediate sends now, ahead of anything
    /// already queued. For Kind::Player, if the private message fails at
    /// delivery time, @p fallbackPrefix + the line is said to everyone
    /// (empty = no fallback).
    /// @return false if a line was dropped or failed to send right away.
    bool Send(Kind kind, int32_t target, const std::string& msg, double interval, bool immediate,
              const std::string& fallbackPrefix = {});

    /// Split @p msg into lines of at most @p limit characters, preferring
    /// spaces (0 = no limit). Exposed for tests.
    static std::vector<std::string> Split(const std::string& msg, size_t limit);

    /// Release lines whose time has come. Call every frame.
    void Process(double now);
    /// Send everything still queued, ignoring intervals (shutdown).
    void Flush();
    /// Drop queued private messages to a player who left (slot ids are reused).
    void PlayerLeft(rmm_player_t p);

    /// Test hook: deliver every line immediately (simulator scripts, which
    /// run without real time passing).
    void SetBypass(bool b) { bypass_ = b; }

    bool Bypass() const { return bypass_; }

    size_t Pending() const;

private:
    struct Line {
        std::string msg, fallbackPrefix;
        double interval;
    };

    bool SendLine(Kind kind, int32_t target, Line line, bool immediate);

    struct Channel {
        std::deque<Line> lines;
        double next = 0;
    };

    using Key = std::pair<int, int32_t>;

    bool Deliver(Kind kind, int32_t target, const Line& l);
    double DefaultInterval() const;

    std::map<Key, Channel> channels_;
    bool bypass_ = false;
    static constexpr size_t kMaxQueued = 64;  // per destination
};

}  // namespace rmm
