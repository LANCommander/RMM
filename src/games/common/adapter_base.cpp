#include "adapter_base.h"

#include "../../core/core.h"
#include "../../core/platform.h"

#include <cstdlib>
#include <sstream>

namespace rmm {

static const char* kQuotedKeys[] = {"msg", "reason", "map", "mode", "name"};

bool AdapterBase::LoadGameData(const std::string& gamedataDir, std::string& err) {
    std::string path = plat::JoinPath(gamedataDir, shortName_ + ".ini");

    if (!data_.Load(path)) {
        err = "gamedata file not found: " + path;

        return false;
    }

    if (mod_.empty())
        mod_ = data_.Get("game", "default_mod", shortName_);

    return true;
}

std::string AdapterBase::BuildCommand(const std::string& key, const std::map<std::string, std::string>& args) const {
    std::string tmpl = data_.Get("commands", key);

    if (tmpl.empty())
        return {};

    std::map<std::string, std::string> vars = args;

    for (const char* q : kQuotedKeys) {
        auto it = vars.find(q);

        if (it != vars.end())
            it->second = QuoteArg(it->second);
    }

    return Substitute(tmpl, vars);
}

bool AdapterBase::RunTemplate(const std::string& key, const std::map<std::string, std::string>& args,
                              std::string* out) {
    std::string cmd = BuildCommand(key, args);

    if (cmd.empty())
        return false;

    // Templates may hold several commands separated by " ; " (e.g. append + save).
    bool ok = true;
    size_t start = 0;

    while (start <= cmd.size()) {
        size_t sep = cmd.find(" ; ", start);
        std::string part = Trim(cmd.substr(start, sep == std::string::npos ? std::string::npos : sep - start));

        if (!part.empty())
            ok = Exec(part, out) && ok;

        if (sep == std::string::npos)
            break;

        start = sep + 3;
    }

    return ok;
}

// Capabilities default to "is there a gamedata template for it?"
bool AdapterBase::HasCap(int32_t cap) const {
    int o = CapOverride(cap);

    if (o >= 0)
        return o != 0;

    // Explicit [capabilities] entries win (e.g. private_message = 0).
    if (const char* k = CapKey(cap); k && data_.Has("capabilities", k))
        return data_.Get("capabilities", k) != "0";

    switch (cap) {
        case RMM_CAP_KICK:
            return HasTemplate("kick");
        case RMM_CAP_ENGINE_BAN:
            return HasTemplate("ban");
        case RMM_CAP_KILL_PLAYER:
            return HasTemplate("kill");
        case RMM_CAP_SET_TEAM:
            return HasTemplate("set_team");
        case RMM_CAP_PRIVATE_MESSAGE:
            return HasTemplate("pm");
        case RMM_CAP_SET_SCORE:
            return HasTemplate("set_score");
        case RMM_CAP_TELEPORT:
            return HasTemplate("teleport");
        case RMM_CAP_CHANGE_MAP:
            return HasTemplate("change_map");
        case RMM_CAP_MAPLIST:
            return HasTemplate("maplist_list");
        case RMM_CAP_TICKETS:
            return HasTemplate("get_tickets");
        case RMM_CAP_CENTER_MESSAGE:
            return HasTemplate("center");
        case RMM_CAP_BOTS:
            return !SettingVar(RMM_SETTING_BOT_COUNT).empty() || !SettingVar(RMM_SETTING_BOT_RATIO).empty();
        // Bots: [capabilities] bot_add = 0 etc. switch single ones off (see above).
        case RMM_CAP_BOT_QUERY:
            return HasTemplate("bot_count") && HasTemplate("bot_list");
        case RMM_CAP_BOT_ADD:
            return HasTemplate("bot_count") && HasTemplate("bot_list") &&
                   (HasTemplate("bot_add") || HasTemplate("bot_add_player"));
        case RMM_CAP_BOT_REMOVE:
            return HasTemplate("bot_count") && HasTemplate("bot_list") && HasTemplate("bot_remove");
        case RMM_CAP_BOT_ORDERS:
            return HasTemplate("bot_count") && HasTemplate("bot_list") &&
                   (HasTemplate("bot_move") || HasTemplate("bot_patrol") || HasTemplate("bot_order"));
        case RMM_CAP_BOT_STRATEGY:
            return HasTemplate("bot_count") && HasTemplate("bot_strategy") && HasTemplate("bot_strategies") &&
                   HasTemplate("bot_set_strategy");
        case RMM_CAP_BOT_SKILL:
            return HasTemplate("bot_count") && HasTemplate("bot_skill") && HasTemplate("bot_set_skill");
        case RMM_CAP_SET_KIT:
            return HasTemplate("set_kit");
        case RMM_CAP_END_ROUND:
            return HasTemplate("end_round");
        default:
            return false;
    }
}

std::string AdapterBase::SettingVar(int32_t setting) const {
    const char* key = SettingKey(setting);

    return key ? data_.Get("settings", key) : std::string();
}

bool AdapterBase::GetEngineVar(const std::string& name, std::string& value) {
    if (name.empty() || name.find_first_of(" \t;\"") != std::string::npos)
        return false;

    std::string out;

    if (!Exec(name, &out))
        return false;

    value = Trim(out);

    if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
        value = value.substr(1, value.size() - 2);

    return true;
}

bool AdapterBase::SetEngineVar(const std::string& name, const std::string& value) {
    if (name.empty() || name.find_first_of(" \t;\"") != std::string::npos)
        return false;

    bool needsQuotes = value.empty() || value.find_first_of(" \t;") != std::string::npos;

    return Exec(name + " " + (needsQuotes ? QuoteArg(value) : value), nullptr);
}

static std::string I(int64_t v) { return std::to_string(v); }

bool AdapterBase::KickPlayer(rmm_player_t id, const std::string& reason) {
    return RunTemplate("kick", {{"id", I(id)}, {"reason", reason}});
}

bool AdapterBase::BanPlayerEngine(rmm_player_t id, int32_t minutes, const std::string& reason) {
    return RunTemplate("ban", {{"id", I(id)}, {"minutes", I(minutes)}, {"reason", reason}});
}

bool AdapterBase::KillPlayer(rmm_player_t id) { return RunTemplate("kill", {{"id", I(id)}}); }

bool AdapterBase::SetTeam(rmm_player_t id, int32_t team) {
    return RunTemplate("set_team", {{"id", I(id)}, {"team", I(team)}});
}

bool AdapterBase::PrivateMessage(rmm_player_t id, const std::string& msg) {
    return RunTemplate("pm", {{"id", I(id)}, {"msg", msg}});
}

bool AdapterBase::SetScore(rmm_player_t id, int32_t score) {
    return RunTemplate("set_score", {{"id", I(id)}, {"score", I(score)}});
}

bool AdapterBase::Teleport(rmm_player_t id, const rmm_vec3& p) {
    return RunTemplate(
        "teleport",
        {{"id", I(id)}, {"x", std::to_string(p.x)}, {"y", std::to_string(p.y)}, {"z", std::to_string(p.z)}});
}

void AdapterBase::NoteMap(const std::string& map, const std::string& mode, int32_t size) {
    CopyOut(map, currentMap_.map, sizeof currentMap_.map);
    CopyOut(mode, currentMap_.mode, sizeof currentMap_.mode);
    currentMap_.size = size;
}

bool AdapterBase::CurrentMap(rmm_map_info& out) {
    if (!currentMap_.map[0])
        return false;

    out = currentMap_;

    return true;
}

bool AdapterBase::ChangeMap(const std::string& map, const std::string& mode, int32_t size) {
    std::string m = mode.empty() ? std::string(currentMap_.mode) : mode;
    int32_t s = size;

    if (s <= 0)
        s = currentMap_.size;

    if (s <= 0)
        s = std::atoi(data_.Get("game", "default_size", "0").c_str());

    return RunTemplate("change_map", {{"map", map}, {"mode", m}, {"size", I(s)}, {"mod", ModName()}});
}

bool AdapterBase::RestartMap() { return RunTemplate("restart_map", {}); }

bool AdapterBase::RunNextMap() { return RunTemplate("run_next_map", {}); }

std::vector<rmm_map_info> AdapterBase::ParseMaplist(const std::string& output) const {
    // Accepts lines like:  3: "strike_at_karkand" gpm_cq 32   or   el_alamein conquest
    std::vector<rmm_map_info> v;
    std::istringstream in(output);
    std::string line;

    while (std::getline(in, line)) {
        auto t = SplitArgs(line);

        if (t.empty())
            continue;

        size_t i = 0;

        if (!t[0].empty() && t[0].back() == ':')
            ++i;

        if (i >= t.size())
            continue;

        rmm_map_info m{};
        m.struct_size = sizeof m;
        CopyOut(t[i], m.map, sizeof m.map);

        if (i + 1 < t.size())
            CopyOut(t[i + 1], m.mode, sizeof m.mode);

        if (i + 2 < t.size())
            m.size = std::atoi(t[i + 2].c_str());

        v.push_back(m);
    }

    return v;
}

int32_t AdapterBase::MaplistCount() {
    std::string out;

    if (!RunTemplate("maplist_list", {}, &out))
        return 0;

    return static_cast<int32_t>(ParseMaplist(out).size());
}

bool AdapterBase::MaplistGet(int32_t index, rmm_map_info& out) {
    std::string o;

    if (!RunTemplate("maplist_list", {}, &o))
        return false;

    auto v = ParseMaplist(o);

    if (index < 0 || static_cast<size_t>(index) >= v.size())
        return false;

    out = v[static_cast<size_t>(index)];

    return true;
}

bool AdapterBase::MaplistAppend(const std::string& map, const std::string& mode, int32_t size) {
    return RunTemplate("maplist_append", {{"map", map}, {"mode", mode}, {"size", I(size)}, {"mod", ModName()}});
}

bool AdapterBase::MaplistClear() { return RunTemplate("maplist_clear", {}); }

bool AdapterBase::SetNextMap(int32_t index) { return RunTemplate("set_next_map", {{"index", I(index)}}); }

bool AdapterBase::SayAll(const std::string& msg) { return RunTemplate("say_all", {{"msg", msg}}); }

bool AdapterBase::SayTeam(int32_t team, const std::string& msg) {
    return RunTemplate("say_team", {{"team", I(team)}, {"msg", msg}});
}

bool AdapterBase::CenterMessage(const std::string& msg) { return RunTemplate("center", {{"msg", msg}}); }

bool AdapterBase::ObjectTemplate(rmm_object_t obj, std::string& out) {
    rmm_object_info info{};
    info.struct_size = sizeof info;

    if (!obj || !GetObjectInfo(obj, info))
        return false;

    out = info.template_name;

    return true;
}

// Generic fallback: players whose reported vehicle is @p obj.
std::vector<rmm_player_t> AdapterBase::ObjectOccupants(rmm_object_t obj) {
    std::vector<rmm_player_t> v;

    if (!obj)
        return v;

    for (rmm_player_t id : PlayerIds()) {
        rmm_player_info p{};
        p.struct_size = sizeof p;

        if (GetPlayer(id, p) && p.vehicle == obj)
            v.push_back(id);
    }

    return v;
}

int32_t AdapterBase::GetTickets(int32_t team) {
    std::string out;

    if (!RunTemplate("get_tickets", {{"team", I(team)}}, &out))
        return -1;

    return std::atoi(Trim(out).c_str());
}

bool AdapterBase::SetTickets(int32_t team, int32_t tickets) {
    return RunTemplate("set_tickets", {{"team", I(team)}, {"tickets", I(tickets)}});
}

bool AdapterBase::SetKit(rmm_player_t id, const std::string& kitTemplate) {
    return RunTemplate("set_kit", {{"id", I(id)}, {"kit", kitTemplate}});
}

bool AdapterBase::EndRoundNow(int32_t winner) { return RunTemplate("end_round", {{"team", I(winner)}}); }

}  // namespace rmm
