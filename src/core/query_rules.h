// Extra \key\value pairs for the server's GameSpy query replies
// (rmm_game_api::set_query_rule). The adapter's send hook calls Rewrite() for
// every UDP packet the server sends, from whatever thread sends it.
#pragma once

#include "plugin.h"

#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace rmm {

class QueryRules {
public:
    static constexpr size_t kMaxKey = 32;
    static constexpr size_t kMaxValue = 128;
    static constexpr size_t kMaxRules = 32;

    /// Add, replace (@p value non-null) or remove (@p value null) @p owner's rule.
    /// Fails if the key is invalid or another plugin owns it.
    bool Set(Plugin* owner, const std::string& key, const char* value, std::string& err);
    void RemoveAll(Plugin* owner);
    /// (key, value) in order; @p owners gets the owning plugins' names.
    std::vector<std::pair<std::string, std::string>> List(std::vector<std::string>* owners = nullptr) const;

    /// A GameSpy query reply ("\key\value...\final\...") with the rules inserted
    /// before its first \final\, into @p out (@p cap bytes). @return the new
    /// length, or 0 to send @p packet unchanged (not a reply, no rules, the reply
    /// has the keys already, or no room).
    int Rewrite(const char* packet, int length, char* out, int cap) const;

private:
    struct Rule {
        Plugin* owner;
        std::string key, value;
    };

    mutable std::mutex mu_;
    std::vector<Rule> rules_;
};

}  // namespace rmm
