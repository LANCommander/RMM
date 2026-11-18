#include "query_rules.h"

#include <algorithm>
#include <cstring>

namespace rmm {
namespace {

// Printable ASCII without the pair separator.
bool ValidText(const std::string& s) {
    return std::all_of(s.begin(), s.end(), [](char c) { return c >= 0x20 && c < 0x7f && c != '\\'; });
}

// The next '\' at or after @p at, else @p length.
int NextSlash(const char* p, int at, int length) {
    while (at < length && p[at] != '\\')
        ++at;

    return at;
}

}  // namespace

bool QueryRules::Set(Plugin* owner, const std::string& key, const char* value, std::string& err) {
    if (key.empty() || key.size() > kMaxKey || !ValidText(key)) {
        err = "a key is 1-32 printable characters without '\\'";

        return false;
    }

    if (value && (std::strlen(value) > kMaxValue || !ValidText(value))) {
        err = "a value is up to 128 printable characters without '\\'";

        return false;
    }

    std::lock_guard<std::mutex> lk(mu_);
    auto it = std::find_if(rules_.begin(), rules_.end(), [&](const Rule& r) { return r.key == key; });

    if (it != rules_.end() && it->owner != owner) {
        err = "another plugin owns the key '" + key + "'";

        return false;
    }

    if (!value) {
        if (it != rules_.end())
            rules_.erase(it);

        return true;
    }

    if (it != rules_.end()) {
        it->value = value;

        return true;
    }

    if (rules_.size() >= kMaxRules) {
        err = "too many query rules";

        return false;
    }

    rules_.push_back(Rule{owner, key, value});

    return true;
}

void QueryRules::RemoveAll(Plugin* owner) {
    std::lock_guard<std::mutex> lk(mu_);
    rules_.erase(std::remove_if(rules_.begin(), rules_.end(), [&](const Rule& r) { return r.owner == owner; }),
                 rules_.end());
}

std::vector<std::pair<std::string, std::string>> QueryRules::List(std::vector<std::string>* owners) const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<std::pair<std::string, std::string>> out;

    for (const Rule& r : rules_) {
        out.emplace_back(r.key, r.value);

        if (owners)
            owners->push_back(r.owner ? r.owner->name : "core");
    }

    return out;
}

int QueryRules::Rewrite(const char* packet, int length, char* out, int cap) const {
    static const char kFinal[] = "\\final\\";
    constexpr int kFinalLen = sizeof kFinal - 1;

    // Cheap rejection first: this sees every packet the server sends.
    if (!packet || length < kFinalLen || packet[0] != '\\')
        return 0;

    const char* end = packet + length;

    if (std::search(packet, end, kFinal, kFinal + kFinalLen) == end)
        return 0;

    std::lock_guard<std::mutex> lk(mu_);

    if (rules_.empty())
        return 0;

    // Walk the \key\value pairs: find the first \final\ and the keys present.
    std::vector<bool> present(rules_.size(), false);
    int pos = 0, finalAt = -1;

    while (pos < length) {
        int keyEnd = NextSlash(packet, pos + 1, length);

        if (keyEnd == length)
            return 0;  // a key without a value: not a reply

        int valueEnd = NextSlash(packet, keyEnd + 1, length);
        size_t n = static_cast<size_t>(keyEnd - pos - 1);

        if (n == 5 && !std::memcmp(packet + pos + 1, "final", 5) && finalAt < 0)
            finalAt = pos;

        for (size_t i = 0; i < rules_.size(); ++i)
            if (rules_[i].key.size() == n && !std::memcmp(packet + pos + 1, rules_[i].key.data(), n))
                present[i] = true;

        pos = valueEnd;
    }

    if (finalAt < 0)
        return 0;

    std::string pairs;

    for (size_t i = 0; i < rules_.size(); ++i)
        if (!present[i])
            pairs += "\\" + rules_[i].key + "\\" + rules_[i].value;

    int added = static_cast<int>(pairs.size());

    if (!added || length + added > cap)
        return 0;

    std::memcpy(out, packet, static_cast<size_t>(finalAt));
    std::memcpy(out + finalAt, pairs.data(), pairs.size());
    std::memcpy(out + finalAt + added, packet + finalAt, static_cast<size_t>(length - finalAt));

    return length + added;
}

}  // namespace rmm
