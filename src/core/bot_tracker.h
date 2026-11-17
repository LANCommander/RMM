// Bot bookkeeping for rmm_bots_api: runs add() requests across
// frames and turns the adapter's bot list into BOT_ADDED / BOT_REMOVED events.
#pragma once

#include "../games/game_adapter.h"

#include <map>
#include <string>
#include <vector>

namespace rmm {

class BotTracker {
public:
    /// Validate and start an add request. @return false (err filled) if refused.
    bool Add(const IGameAdapter::BotSpawn& spawn, int32_t tag, std::string& err);
    /// Called from Core::Frame.
    void Process(double now);
    /// The bot behind a player id (fresh adapter query). False if not a bot / AI not running.
    bool Resolve(rmm_player_t player, IGameAdapter::BotRef& out);
    /// A console-safe, unused bot name based on @p wanted ("" = "Bot<n>").
    std::string UniqueName(const std::string& wanted);

    size_t PendingAdds() const { return pending_.size(); }

private:
    struct Request {
        IGameAdapter::BotSpawn spawn;
        int32_t tag = 0;
        int attempt = 0;
        double next = 0;
        double confirmUntil = 0;  // > 0: the engine accepted it, waiting to see the bot
    };

    struct Known {
        IGameAdapter::BotRef ref;
        int32_t team;
    };

    void Emit(int32_t ev, const IGameAdapter::BotRef& b, int32_t team, int32_t tag, const std::string& reason);
    bool Tracking() const;
    std::vector<Known> Snapshot();

    std::vector<Request> pending_;
    std::vector<Known> known_;
    bool primed_ = false;
    double nextDiff_ = 0;
    int nameSeq_ = 0;
};

}  // namespace rmm
