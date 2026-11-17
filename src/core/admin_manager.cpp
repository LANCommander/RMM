#include "admin_manager.h"

#include "core.h"
#include "platform.h"
#include "util.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>

namespace rmm {

static constexpr uint32_t kAllFlags = 0x03FFFFFFu;

void AdminManager::SetPaths(const std::string& adminsFile, const std::string& bansFile) {
    adminsFile_ = adminsFile;
    bansFile_ = bansFile;
}

int32_t AdminManager::KindFromString(const std::string& s) {
    std::string k = ToLower(s);

    if (k == "auth" || k == "profile" || k == "hash" || k == "id")
        return RMM_ID_AUTH;

    if (k == "ip")
        return RMM_ID_IP;

    if (k == "name")
        return RMM_ID_NAME;

    return -1;
}

const char* AdminManager::KindToString(int32_t k) {
    switch (k) {
        case RMM_ID_AUTH:
            return "auth";
        case RMM_ID_IP:
            return "ip";
        default:
            return "name";
    }
}

uint32_t AdminManager::ParseFlags(const std::string& letters) {
    uint32_t f = 0;

    for (char c : letters) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        if (c >= 'a' && c <= 'z')
            f |= 1u << (c - 'a');
    }

    if (f & RMM_ADMFLAG_ROOT)
        f = kAllFlags;

    return f;
}

std::string AdminManager::FlagsToString(uint32_t flags) {
    std::string s;

    for (int i = 0; i < 26; ++i)
        if (flags & (1u << i))
            s += static_cast<char>('a' + i);

    return s;
}

void AdminManager::Load() {
    Core& core = Core::Get();
    admins_.clear();
    cache_.clear();
    {
        std::ifstream f(adminsFile_);
        std::string line;
        int lineNo = 0;

        while (std::getline(f, line)) {
            ++lineNo;
            line = Trim(line);

            if (line.empty() || line[0] == ';' || line[0] == '#' || StartsWith(line, "//"))
                continue;

            auto a = SplitArgs(line);

            if (a.size() < 3) {
                core.Log().Warn("admins.cfg:%d: expected <kind> <identity> <flags> [immunity] [name]", lineNo);
                continue;
            }

            int32_t kind = KindFromString(a[0]);

            if (kind < 0) {
                core.Log().Warn("admins.cfg:%d: unknown identity kind '%s'", lineNo, a[0].c_str());
                continue;
            }

            AdminEntry e{kind, a[1], ParseFlags(a[2]), a.size() > 3 ? std::atoi(a[3].c_str()) : 0,
                         a.size() > 4 ? a[4] : ""};

            if (kind == RMM_ID_NAME)
                core.Log().Warn("admins.cfg:%d: name-based admin '%s' can be spoofed; prefer auth ids", lineNo,
                                a[1].c_str());

            admins_.push_back(e);
        }
    }

    bans_.clear();
    {
        std::ifstream f(bansFile_);
        std::string line;

        while (std::getline(f, line)) {
            line = Trim(line);

            if (line.empty() || line[0] == ';' || line[0] == '#' || StartsWith(line, "//"))
                continue;

            auto a = SplitArgs(line);

            if (a.size() < 3)
                continue;

            int32_t kind = KindFromString(a[0]);

            if (kind < 0)
                continue;

            BanEntry b{kind,
                       a[1],
                       std::atoll(a[2].c_str()),
                       a.size() > 3 ? std::atoll(a[3].c_str()) : 0,
                       a.size() > 4 ? a[4] : "",
                       a.size() > 5 ? a[5] : "",
                       a.size() > 6 ? a[6] : ""};
            bans_.push_back(b);
        }
    }

    PurgeExpired();
    core.Log().Info("Loaded %zu admin entries and %zu bans", admins_.size(), bans_.size());
}

void AdminManager::SaveBans() {
    std::ofstream f(bansFile_, std::ios::trunc);

    if (!f)
        return;

    f << "// Refractor MultiMod ban list. kind identity expires(unix, 0=permanent) created reason by name\n";

    for (auto& b : bans_)
        f << KindToString(b.kind) << " " << QuoteArg(b.value) << " " << b.expires << " " << b.created << " "
          << QuoteArg(b.reason) << " " << QuoteArg(b.by) << " " << QuoteArg(b.name) << "\n";
}

void AdminManager::PurgeExpired() {
    int64_t now = plat::UnixTime();
    size_t before = bans_.size();
    bans_.erase(std::remove_if(bans_.begin(), bans_.end(),
                               [&](const BanEntry& b) { return b.expires != 0 && b.expires <= now; }),
                bans_.end());

    if (bans_.size() != before)
        SaveBans();
}

static std::string StripPort(const std::string& addr) {
    auto c = addr.rfind(':');

    return c == std::string::npos ? addr : addr.substr(0, c);
}

const AdminManager::Cached& AdminManager::Resolve(rmm_player_t p) {
    auto it = cache_.find(p);

    if (it != cache_.end())
        return it->second;

    Cached c{0, 0};
    rmm_player_info info{};
    info.struct_size = sizeof info;

    if (Core::Get().Game().GetPlayer(p, info)) {
        std::string ip = StripPort(info.address);

        for (auto& a : admins_) {
            bool match = (a.kind == RMM_ID_AUTH && info.auth_id[0] && a.value == info.auth_id) ||
                         (a.kind == RMM_ID_IP && !ip.empty() && a.value == ip) ||
                         (a.kind == RMM_ID_NAME && IEquals(a.value, info.name));

            if (match) {
                c.flags |= a.flags;
                c.immunity = std::max(c.immunity, a.immunity);
            }
        }
    }

    if (auto g = grants_.find(p); g != grants_.end()) {
        c.flags |= g->second.flags;
        c.immunity = std::max(c.immunity, g->second.immunity);
    }

    return cache_[p] = c;
}

uint32_t AdminManager::GetFlags(rmm_player_t p) {
    if (p == RMM_PLAYER_NONE)
        return kAllFlags;

    return Resolve(p).flags;
}

int32_t AdminManager::GetImmunity(rmm_player_t p) {
    if (p == RMM_PLAYER_NONE)
        return 0x7FFFFFFF;

    return Resolve(p).immunity;
}

bool AdminManager::HasFlags(rmm_player_t p, uint32_t flags) {
    uint32_t have = GetFlags(p);

    if (have & RMM_ADMFLAG_ROOT)
        return true;

    return (have & flags) == flags;
}

bool AdminManager::CanTarget(rmm_player_t admin, rmm_player_t target) {
    if (admin == RMM_PLAYER_NONE || admin == target)
        return true;

    if (GetFlags(admin) & RMM_ADMFLAG_ROOT)
        return true;

    return GetImmunity(target) <= GetImmunity(admin);
}

bool AdminManager::Grant(rmm_player_t p, uint32_t flags, int32_t immunity) {
    if (p == RMM_PLAYER_NONE)
        return false;

    auto& g = grants_[p];
    g.flags |= (flags & RMM_ADMFLAG_ROOT) ? kAllFlags : flags;
    g.immunity = std::max(g.immunity, immunity);
    cache_.erase(p);

    return true;
}

bool AdminManager::Kick(rmm_player_t p, const std::string& reason, rmm_player_t by) {
    Core& core = Core::Get();
    rmm_player_info info{};
    info.struct_size = sizeof info;

    if (!core.Game().GetPlayer(p, info))
        return false;

    core.Log().Info("Kick: %s (#%d) by %s: %s", info.name, p, core.PlayerLabel(by).c_str(), reason.c_str());

    return core.Game().KickPlayer(p, reason);
}

bool AdminManager::Ban(rmm_player_t p, int32_t minutes, const std::string& reason, rmm_player_t by) {
    Core& core = Core::Get();
    rmm_player_info info{};
    info.struct_size = sizeof info;

    if (!core.Game().GetPlayer(p, info))
        return false;

    bool ok;

    if (info.auth_id[0])
        ok = BanIdentity(RMM_ID_AUTH, info.auth_id, minutes, reason, by, info.name);
    else
        ok = BanIdentity(RMM_ID_IP, StripPort(info.address), minutes, reason, by, info.name);

    if (ok) {
        if (core.Game().HasCap(RMM_CAP_ENGINE_BAN) && core.CoreVarInt("rmm.mirrorEngineBans"))
            core.Game().BanPlayerEngine(p, minutes, reason);

        core.Game().KickPlayer(p, minutes ? Format("Banned for %d minutes: %s", minutes, reason.c_str())
                                          : "Banned: " + reason);
    }

    return ok;
}

bool AdminManager::BanIdentity(int32_t kind, const std::string& value, int32_t minutes, const std::string& reason,
                               rmm_player_t by, const std::string& displayName) {
    if (value.empty() || kind < RMM_ID_AUTH || kind > RMM_ID_NAME)
        return false;

    Unban(kind, value);  // replace any existing entry
    int64_t now = plat::UnixTime();
    bans_.push_back(BanEntry{kind, value, minutes > 0 ? now + int64_t(minutes) * 60 : 0, now, reason,
                             Core::Get().PlayerLabel(by), displayName});
    SaveBans();
    Core::Get().Log().Info("Ban added: %s %s (%s) for %s: %s", KindToString(kind), value.c_str(), displayName.c_str(),
                           minutes ? Format("%d min", minutes).c_str() : "ever", reason.c_str());

    return true;
}

bool AdminManager::Unban(int32_t kind, const std::string& value) {
    size_t before = bans_.size();
    bans_.erase(std::remove_if(bans_.begin(), bans_.end(),
                               [&](const BanEntry& b) {
                                   return b.kind == kind &&
                                          (kind == RMM_ID_NAME ? IEquals(b.value, value) : b.value == value);
                               }),
                bans_.end());

    if (bans_.size() != before) {
        SaveBans();

        return true;
    }

    return false;
}

bool AdminManager::IsBanned(int32_t kind, const std::string& value, std::string* reason) {
    if (value.empty())
        return false;

    PurgeExpired();

    for (auto& b : bans_) {
        if (b.kind != kind)
            continue;

        if (kind == RMM_ID_NAME ? IEquals(b.value, value) : b.value == value) {
            if (reason)
                *reason = b.reason;

            return true;
        }
    }

    return false;
}

bool AdminManager::CheckPlayerBanned(const rmm_player_info& info, std::string& reason) {
    return IsBanned(RMM_ID_AUTH, info.auth_id, &reason) || IsBanned(RMM_ID_IP, StripPort(info.address), &reason) ||
           IsBanned(RMM_ID_NAME, info.name, &reason);
}

}  // namespace rmm
