#include "bot_tracker.h"

#include "core.h"
#include "util.h"

#include <algorithm>

namespace rmm {

namespace {
constexpr double kPollInterval = 0.25;   // between BotAddPoll attempts
constexpr double kConfirmSeconds = 3.0;  // engine accepted the bot; wait this long to see it listed
constexpr double kDiffInterval = 1.0;    // bot list comparison for ADDED/REMOVED
constexpr size_t kMaxPending = 8;
constexpr size_t kMaxNameLength = 31;
}  // namespace

bool BotTracker::Tracking() const {
    EventBus& ev = Core::Get().Events();

    return ev.HasHooks(RMM_EV_BOT_ADDED) || ev.HasHooks(RMM_EV_BOT_REMOVED);
}

std::vector<BotTracker::Known> BotTracker::Snapshot() {
    IGameAdapter& game = Core::Get().Game();
    std::vector<Known> v;

    if (!game.BotsAvailable())
        return v;

    for (auto& b : game.Bots()) {
        rmm_player_info pi{};
        pi.struct_size = sizeof pi;
        v.push_back(Known{b, game.GetPlayer(b.player, pi) ? pi.team : RMM_TEAM_NONE});
    }

    return v;
}

bool BotTracker::Resolve(rmm_player_t player, IGameAdapter::BotRef& out) {
    IGameAdapter& game = Core::Get().Game();

    if (player < 0 || !game.HasCap(RMM_CAP_BOT_QUERY) || !game.BotsAvailable())
        return false;

    for (auto& b : game.Bots())
        if (b.player == player) {
            out = b;

            return true;
        }

    return false;
}

std::string BotTracker::UniqueName(const std::string& wanted) {
    std::string base;

    for (char c : Trim(wanted)) {
        unsigned char u = static_cast<unsigned char>(c);

        if (u < 0x20 || u == 0x7f)
            continue;

        base += (c == ' ' || c == '"' || c == ';' || c == '\\') ? '_' : c;
    }

    if (base.size() > kMaxNameLength)
        base.resize(kMaxNameLength);

    IGameAdapter& game = Core::Get().Game();
    auto taken = [&](const std::string& n) {
        for (auto& r : pending_)
            if (IEquals(r.spawn.name, n))
                return true;

        for (rmm_player_t id : game.PlayerIds()) {
            rmm_player_info pi{};
            pi.struct_size = sizeof pi;

            if (game.GetPlayer(id, pi) && IEquals(pi.name, n))
                return true;
        }

        return false;
    };

    if (base.empty()) {
        std::string n;

        do
            n = "Bot" + std::to_string(++nameSeq_);
        while (taken(n));

        return n;
    }

    if (!taken(base))
        return base;

    for (int i = 2;; ++i) {
        std::string suffix = "_" + std::to_string(i);
        std::string n = base.substr(0, std::min(base.size(), kMaxNameLength - suffix.size())) + suffix;

        if (!taken(n))
            return n;
    }
}

bool BotTracker::Add(const IGameAdapter::BotSpawn& in, int32_t tag, std::string& err) {
    Core& core = Core::Get();
    IGameAdapter& game = core.Game();

    if (!game.HasCap(RMM_CAP_BOT_ADD)) {
        err = "adding bots is not supported (or not enabled) on this server";

        return false;
    }

    if (in.team != 1 && in.team != 2) {
        err = "team must be 1 or 2";

        return false;
    }

    if (pending_.size() >= kMaxPending) {
        err = "too many bot requests in progress";

        return false;
    }

    if (!game.BotsAvailable()) {
        err = "the AI is not running (BF1942/BFV: co-op rounds only)";

        return false;
    }

    if (game.PlayerIds().size() + pending_.size() >= static_cast<size_t>(std::max(game.MaxPlayers(), 0))) {
        err = "the server is full";

        return false;
    }

    IGameAdapter::BotSpawn s = in;
    s.name = UniqueName(in.name);

    if (!game.BotAddBegin(s, err)) {
        if (err.empty())
            err = "the game refused the bot";

        return false;
    }

    Request r;
    r.spawn = s;
    r.tag = tag;
    r.next = core.Time() + kPollInterval;
    pending_.push_back(r);
    core.Log().Info("Adding bot %s (team %d%s)", s.name.c_str(), s.team, s.kit.empty() ? "" : (", " + s.kit).c_str());

    return true;
}

void BotTracker::Emit(int32_t ev, const IGameAdapter::BotRef& b, int32_t team, int32_t tag, const std::string& reason) {
    rmm_ev_bot e{b.player, b.botId, b.name.c_str(), team, tag, reason.c_str()};
    Core::Get().Log().Debug("event %s: #%d %s (bot %d, team %d, tag %d)%s%s", Core::Get().Events().Name(ev), b.player,
                            b.name.c_str(), b.botId, team, tag, reason.empty() ? "" : ": ", reason.c_str());
    Core::Get().Dispatch(ev, &e);
}

void BotTracker::Process(double now) {
    Core& core = Core::Get();
    IGameAdapter& game = core.Game();

    // Pending add requests. Handlers of the events below may call add() again, so a
    // finished request is copied and erased before its event is dispatched.
    for (size_t i = 0; i < pending_.size();) {
        if (now < pending_[i].next) {
            ++i;
            continue;
        }

        pending_[i].next = now + kPollInterval;
        std::string err;

        if (pending_[i].confirmUntil <= 0) {
            auto st = game.BotAddPoll(pending_[i].spawn, ++pending_[i].attempt, err);

            if (st == IGameAdapter::BotAddState::Pending) {
                ++i;
                continue;
            }

            if (st == IGameAdapter::BotAddState::Failed) {
                Request r = pending_[i];
                pending_.erase(pending_.begin() + static_cast<std::ptrdiff_t>(i));

                if (err.empty())
                    err = "the game refused the bot (server or AI full?)";

                core.Log().Warn("Could not add bot %s: %s", r.spawn.name.c_str(), err.c_str());
                Emit(RMM_EV_BOT_ADD_FAILED, IGameAdapter::BotRef{RMM_PLAYER_NONE, -1, r.spawn.name}, r.spawn.team,
                     r.tag, err);
                continue;
            }

            pending_[i].confirmUntil = now + kConfirmSeconds;
        }

        IGameAdapter::BotRef found{RMM_PLAYER_NONE, -1, {}};

        if (game.BotsAvailable())
            for (auto& b : game.Bots())
                if (b.name == pending_[i].spawn.name) {
                    found = b;
                    break;
                }

        if (found.player != RMM_PLAYER_NONE) {
            Request r = pending_[i];
            pending_.erase(pending_.begin() + static_cast<std::ptrdiff_t>(i));
            rmm_player_info pi{};
            pi.struct_size = sizeof pi;
            int32_t team = game.GetPlayer(found.player, pi) ? pi.team : r.spawn.team;

            if (primed_)
                known_.push_back(Known{found, team});

            core.Log().Info("Bot %s added as player #%d (bot %d)", found.name.c_str(), found.player, found.botId);
            Emit(RMM_EV_BOT_ADDED, found, team, r.tag, "");
            continue;
        }

        if (now >= pending_[i].confirmUntil) {
            Request r = pending_[i];
            pending_.erase(pending_.begin() + static_cast<std::ptrdiff_t>(i));
            core.Log().Warn("Bot %s was accepted but never appeared", r.spawn.name.c_str());
            Emit(RMM_EV_BOT_ADD_FAILED, IGameAdapter::BotRef{RMM_PLAYER_NONE, -1, r.spawn.name}, r.spawn.team, r.tag,
                 "the bot did not appear");
            continue;
        }

        ++i;
    }

    // ADDED / REMOVED by comparing bot lists, only while someone listens.
    if (!Tracking()) {
        primed_ = false;
        known_.clear();

        return;
    }

    if (now < nextDiff_)
        return;

    nextDiff_ = now + kDiffInterval;
    std::vector<Known> cur = Snapshot();

    if (!primed_) {
        known_ = cur;
        primed_ = true;

        return;
    }  // bots already present are not "added"

    auto same = [](const Known& a, const Known& b) { return a.ref.player == b.ref.player && a.ref.name == b.ref.name; };
    std::vector<Known> added, removed;

    for (auto& c : cur)
        if (std::none_of(known_.begin(), known_.end(), [&](const Known& k) { return same(k, c); }))
            added.push_back(c);

    for (auto& k : known_)
        if (std::none_of(cur.begin(), cur.end(), [&](const Known& c) { return same(k, c); }))
            removed.push_back(k);

    known_ = cur;

    for (auto& k : removed)
        Emit(RMM_EV_BOT_REMOVED, k.ref, k.team, 0, "");

    for (auto& a : added) {
        // One of our own requests that the confirm step hasn't seen yet: report it with its tag.
        int32_t tag = 0;
        auto it = std::find_if(pending_.begin(), pending_.end(),
                               [&](const Request& r) { return r.confirmUntil > 0 && r.spawn.name == a.ref.name; });

        if (it != pending_.end()) {
            tag = it->tag;
            pending_.erase(it);
        }

        Emit(RMM_EV_BOT_ADDED, a.ref, a.team, tag, "");
    }
}

}  // namespace rmm
