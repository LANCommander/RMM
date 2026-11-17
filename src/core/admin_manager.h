#pragma once

#include <rmm/rmm_admin.h>
#include <rmm/rmm_players.h>

#include <string>
#include <unordered_map>
#include <vector>

namespace rmm {

class AdminManager {
public:
    void SetPaths(const std::string& adminsFile, const std::string& bansFile);
    void Load();

    uint32_t GetFlags(rmm_player_t p);
    int32_t GetImmunity(rmm_player_t p);
    bool HasFlags(rmm_player_t p, uint32_t flags);
    bool CanTarget(rmm_player_t admin, rmm_player_t target);
    bool Grant(rmm_player_t p, uint32_t flags, int32_t immunity);

    static uint32_t ParseFlags(const std::string& letters);
    static std::string FlagsToString(uint32_t flags);

    bool Kick(rmm_player_t p, const std::string& reason, rmm_player_t by);
    bool Ban(rmm_player_t p, int32_t minutes, const std::string& reason, rmm_player_t by);
    bool BanIdentity(int32_t kind, const std::string& value, int32_t minutes, const std::string& reason,
                     rmm_player_t by, const std::string& displayName = {});
    bool Unban(int32_t kind, const std::string& value);
    bool IsBanned(int32_t kind, const std::string& value, std::string* reason);

    /// Check a joining/authenticated player against the ban list.
    bool CheckPlayerBanned(const rmm_player_info& info, std::string& reason);

    /// Drop cached permissions (on disconnect / auth).
    void Invalidate(rmm_player_t p) { cache_.erase(p); }

    /// The player slot was freed (or newly taken): drop cached permissions and
    /// runtime grants, so the next occupant of the id doesn't inherit them.
    void PlayerLeft(rmm_player_t p) {
        cache_.erase(p);
        grants_.erase(p);
    }

    struct BanEntry {
        int32_t kind;
        std::string value;
        int64_t expires;
        int64_t created;
        std::string reason, by, name;
    };

    const std::vector<BanEntry>& Bans() const { return bans_; }

private:
    struct AdminEntry {
        int32_t kind;
        std::string value;
        uint32_t flags;
        int32_t immunity;
        std::string name;
    };

    struct Cached {
        uint32_t flags;
        int32_t immunity;
    };

    const Cached& Resolve(rmm_player_t p);
    void SaveBans();
    void PurgeExpired();
    static int32_t KindFromString(const std::string& s);
    static const char* KindToString(int32_t k);

    std::string adminsFile_, bansFile_;
    std::vector<AdminEntry> admins_;
    std::vector<BanEntry> bans_;
    std::unordered_map<rmm_player_t, Cached> cache_;
    std::unordered_map<rmm_player_t, Cached> grants_;
};

}  // namespace rmm
