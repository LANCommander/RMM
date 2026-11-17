#include "chat_queue.h"

#include "core.h"

#include <algorithm>
#include <cstdlib>

namespace rmm {

double ChatQueue::DefaultInterval() const {
    double v = std::atof(Core::Get().CoreVarString("rmm.chatInterval").c_str());

    return v > 0 ? v : 0;
}

bool ChatQueue::Deliver(Kind kind, int32_t target, const Line& l) {
    IGameAdapter& game = Core::Get().Game();

    switch (kind) {
        case Kind::All:
            return game.SayAll(l.msg);
        case Kind::Team:
            return game.SayTeam(target, l.msg);
        case Kind::Player:
            if (game.PrivateMessage(target, l.msg))
                return true;

            return !l.fallbackPrefix.empty() && game.SayAll(l.fallbackPrefix + l.msg);
    }

    return false;
}

std::vector<std::string> ChatQueue::Split(const std::string& msg, size_t limit) {
    if (limit == 0 || msg.size() <= limit)
        return {msg};

    std::vector<std::string> out;
    size_t pos = 0;

    while (pos < msg.size()) {
        while (pos < msg.size() && msg[pos] == ' ')
            ++pos;  // no leading blanks on continuation lines

        if (pos >= msg.size())
            break;

        size_t len = std::min(limit, msg.size() - pos);

        if (pos + len < msg.size()) {
            // Cut at the last space in the chunk, unless that leaves it under half full.
            size_t sp = msg.rfind(' ', pos + len);

            if (sp != std::string::npos && sp > pos + limit / 2)
                len = sp - pos;
        }

        std::string piece = msg.substr(pos, len);

        while (!piece.empty() && piece.back() == ' ')
            piece.pop_back();

        out.push_back(piece);
        pos += len;
    }

    if (out.empty())
        out.push_back(std::string());

    return out;
}

bool ChatQueue::Send(Kind kind, int32_t target, const std::string& msg, double interval, bool immediate,
                     const std::string& fallbackPrefix) {
    double gap = interval < 0 ? DefaultInterval() : interval;
    bool ok = true;

    for (auto& piece : Split(msg, Core::Get().Game().ChatLineLimit()))
        ok = SendLine(kind, target, Line{piece, fallbackPrefix, gap}, immediate) && ok;

    return ok;
}

bool ChatQueue::SendLine(Kind kind, int32_t target, Line line, bool immediate) {
    if (bypass_ || immediate)
        return Deliver(kind, target, line);

    Core& core = Core::Get();
    double now = core.Time();
    Channel& ch = channels_[Key(static_cast<int>(kind), target)];

    if (ch.lines.empty() && now >= ch.next) {
        ch.next = now + line.interval;

        return Deliver(kind, target, line);
    }

    if (ch.lines.size() >= kMaxQueued) {
        core.Log().Warn("chat: more than %zu lines queued for one destination; dropping \"%s\"", kMaxQueued,
                        line.msg.c_str());

        return false;
    }

    ch.lines.push_back(std::move(line));

    return true;
}

void ChatQueue::Process(double now) {
    for (auto it = channels_.begin(); it != channels_.end();) {
        Channel& ch = it->second;
        Kind kind = static_cast<Kind>(it->first.first);

        // Deliver() may re-enter Send() (a plugin reacting to the message); the
        // map node stays valid and the line is moved out before delivery.
        while (!ch.lines.empty() && now >= ch.next) {
            Line line = std::move(ch.lines.front());
            ch.lines.pop_front();
            ch.next = now + line.interval;
            Deliver(kind, it->first.second, line);
        }

        if (ch.lines.empty() && now >= ch.next)
            it = channels_.erase(it);
        else
            ++it;
    }
}

void ChatQueue::Flush() {
    auto channels = std::move(channels_);
    channels_.clear();

    for (auto& [key, ch] : channels)
        for (auto& line : ch.lines)
            Deliver(static_cast<Kind>(key.first), key.second, line);
}

void ChatQueue::PlayerLeft(rmm_player_t p) { channels_.erase(Key(static_cast<int>(Kind::Player), p)); }

size_t ChatQueue::Pending() const {
    size_t n = 0;

    for (auto& kv : channels_)
        n += kv.second.lines.size();

    return n;
}

}  // namespace rmm
