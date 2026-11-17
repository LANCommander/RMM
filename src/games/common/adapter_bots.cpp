// Bots for AdapterBase: everything goes through console templates in
// gamedata [commands] (bot_*), with tunables in [bots]. Output formats follow
// the Refractor 1 AI console (see gamedata/bf1942.ini):
//   bot_count       "32"                     bot_list        "<bot id> <name>" per line
//   bot_position    "x/y/z" (also bot_goal)  bot_team        "1"
//   bot_kit         "KTAssault"              bot_vehicle     "NoVehicle" or a template
//   bot_behaviour   index into bot_behaviours ("<index> <name>" per line), or a name
//   bot_add         prints the new bot id, -1 on failure
//   bot_strategies  "<index> <name>" per line   bot_strategy / bot_skill: one value
// Placeholders: {bot} engine bot id, {botname} console-safe name (unquoted),
// {team} {kit} {personality} {moral} {x} {y} {z} {pos} {radius} {count}
// {p1}..{p3} {behaviour} {modifiers} {strategy} {skill}.
#include "adapter_base.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace rmm {

namespace {

std::string I(int64_t v) { return std::to_string(v); }

// Words the console would split or interpret: keep names/behaviours single tokens.
std::string Token(const std::string& s) {
    std::string t;

    for (char c : Trim(s)) {
        unsigned char u = static_cast<unsigned char>(c);

        if (u < 0x20 || u == 0x7f)
            continue;

        t += (c == ' ' || c == '"' || c == ';' || c == '\\') ? '_' : c;
    }

    return t;
}

bool ParseInt(const std::string& s, long& out) {
    std::string t = Trim(s);

    if (t.empty())
        return false;

    char* end = nullptr;
    out = std::strtol(t.c_str(), &end, 10);

    return end && *end == '\0';
}

std::vector<std::string> Lines(const std::string& s) {
    std::vector<std::string> v;
    std::istringstream in(s);
    std::string line;

    while (std::getline(in, line)) {
        line = Trim(line);

        if (!line.empty())
            v.push_back(line);
    }

    return v;
}

// "<index> <name>" lines -> names in index order (lines without an index are appended).
std::vector<std::string> IndexedNames(const std::string& out) {
    std::vector<std::string> v;

    for (auto& line : Lines(out)) {
        size_t sp = line.find(' ');
        long idx;

        if (sp != std::string::npos && ParseInt(line.substr(0, sp), idx) && idx >= 0 && idx < 1024) {
            if (v.size() <= static_cast<size_t>(idx))
                v.resize(static_cast<size_t>(idx) + 1);

            v[static_cast<size_t>(idx)] = Trim(line.substr(sp + 1));
        } else {
            v.push_back(line);
        }
    }

    v.erase(std::remove(v.begin(), v.end(), std::string()), v.end());

    return v;
}

}  // namespace

bool AdapterBase::ParseVec3(const std::string& s, rmm_vec3& out) {
    std::string t = s;
    std::replace(t.begin(), t.end(), '/', ' ');
    std::replace(t.begin(), t.end(), ',', ' ');
    float x, y, z;

    if (std::sscanf(t.c_str(), "%f %f %f", &x, &y, &z) != 3)
        return false;

    out = rmm_vec3{x, y, z};

    return true;
}

std::string AdapterBase::Vec3Arg(const rmm_vec3& v) { return Format("%.2f/%.2f/%.2f", v.x, v.y, v.z); }

bool AdapterBase::BotQuery(const std::string& key, const std::map<std::string, std::string>& args, std::string& out) {
    out.clear();

    if (!HasTemplate(key))
        return false;

    std::string o;

    if (!RunTemplate(key, args, &o))
        return false;

    out = Trim(o);

    // Refractor prints "Error (0): <line>\nError : Unknown object or method!" for bad commands.
    if (StartsWith(out, "Error") || IContains(out, "Unknown object or method"))
        return false;

    return true;
}

bool AdapterBase::BotsAvailable() {
    // bot_count must be harmless when the AI isn't running (BF1942: "Unknown
    // object or method" outside co-op), unlike bot_list, which may crash then.
    std::string out;
    long n;

    return BotQuery("bot_count", {}, out) && ParseInt(out, n) && n >= 0;
}

std::vector<IGameAdapter::BotRef> AdapterBase::Bots() {
    std::vector<BotRef> v;
    std::string out;

    if (!BotsAvailable() || !BotQuery("bot_list", {}, out))
        return v;

    // Engine names -> player slots (bots are players too).
    std::map<std::string, rmm_player_t> byName;

    for (rmm_player_t id : PlayerIds()) {
        rmm_player_info p{};
        p.struct_size = sizeof p;

        if (GetPlayer(id, p) && (p.is_bot || !byName.count(p.name)))
            byName[p.name] = id;
    }

    for (auto& line : Lines(out)) {
        size_t sp = line.find(' ');
        long id;

        if (sp == std::string::npos || !ParseInt(line.substr(0, sp), id))
            continue;

        std::string name = Trim(line.substr(sp + 1));
        auto it = byName.find(name);

        if (it == byName.end())
            continue;  // a removed bot whose slot is already gone

        v.push_back(BotRef{it->second, static_cast<int32_t>(id), name});
    }

    return v;
}

std::vector<std::string> AdapterBase::BotBehaviours(const BotRef& bot) {
    std::string out;

    if (!BotQuery("bot_behaviours", {{"bot", I(bot.botId)}}, out))
        return {};

    return IndexedNames(out);
}

bool AdapterBase::GetBotInfo(const BotRef& bot, rmm_bot_info& out) {
    std::map<std::string, std::string> a{{"bot", I(bot.botId)}};
    rmm_player_info p{};
    p.struct_size = sizeof p;

    if (GetPlayer(bot.player, p)) {  // fallbacks for anything the AI console doesn't report
        out.team = p.team;
        out.position = p.position;
        CopyOut(p.kit, out.kit, sizeof out.kit);
        CopyOut(p.vehicle_template, out.vehicle, sizeof out.vehicle);
    }

    std::string s;
    rmm_vec3 v;
    long n;

    if (BotQuery("bot_position", a, s) && ParseVec3(s, v))
        out.position = v;

    if (BotQuery("bot_goal", a, s) && ParseVec3(s, v))
        out.goal = v;

    if (BotQuery("bot_team", a, s) && ParseInt(s, n))
        out.team = static_cast<int32_t>(n);

    if (BotQuery("bot_kit", a, s))
        CopyOut(s, out.kit, sizeof out.kit);

    if (BotQuery("bot_vehicle", a, s))
        CopyOut(IEquals(s, data_.Get("bots", "no_vehicle", "NoVehicle")) ? std::string() : s, out.vehicle,
                sizeof out.vehicle);

    if (BotQuery("bot_behaviour", a, s)) {
        if (ParseInt(s, n)) {
            auto names = BotBehaviours(bot);

            if (n >= 0 && static_cast<size_t>(n) < names.size())
                CopyOut(names[static_cast<size_t>(n)], out.behaviour, sizeof out.behaviour);
        } else {
            CopyOut(s, out.behaviour, sizeof out.behaviour);
        }
    }

    return true;
}

bool AdapterBase::BotAddBegin(const BotSpawn& s, std::string& err) {
    std::string kit = Token(s.kit.empty() ? data_.Get("bots", "default_kit", "KTAssault") : s.kit);
    std::map<std::string, std::string> a{{"botname", s.name},
                                         {"team", I(s.team)},
                                         {"kit", kit},
                                         {"x", Format("%.2f", s.pos.x)},
                                         {"y", Format("%.2f", s.pos.y)},
                                         {"z", Format("%.2f", s.pos.z)},
                                         {"pos", Vec3Arg(s.pos)}};
    std::string key = s.at && HasTemplate("bot_add_player_at") ? "bot_add_player_at" : "bot_add_player";

    if (!HasTemplate(key))
        return HasTemplate("bot_add");  // single-step games: all in BotAddPoll

    std::string out;

    if (!RunTemplate(key, a, &out) || StartsWith(Trim(out), "Error")) {
        err = Trim(out).empty() ? "the game refused the bot" : Trim(out);

        return false;
    }

    return true;
}

IGameAdapter::BotAddState AdapterBase::BotAddPoll(const BotSpawn& s, int attempt, std::string& err) {
    if (!HasTemplate("bot_add"))
        return BotAddState::Done;  // the player template did everything

    std::string p =
        Token(s.personality.empty() ? data_.Get("bots", "default_personality", "StandardWeights") : s.personality);
    std::map<std::string, std::string> a{
        {"botname", s.name}, {"team", I(s.team)}, {"personality", p}, {"moral", data_.Get("bots", "moral", "default")}};
    std::string out;
    long id;

    // BF1942: ai.addBot finds the player createAIPlayer made only from the next
    // frame on, and prints -1 until then (or for good, if the AI is full).
    if (BotQuery("bot_add", a, out) && ParseInt(out, id) && id >= 0)
        return BotAddState::Done;

    int attempts = std::max(1, std::atoi(data_.Get("bots", "add_attempts", "8").c_str()));

    if (attempt < attempts)
        return BotAddState::Pending;

    err = "the AI refused the bot (AI full, or unknown kit/personality)";

    // createAIPlayer may have made a player with no AI behind it: don't leave it standing around.
    for (rmm_player_t id2 : PlayerIds()) {
        rmm_player_info pi{};
        pi.struct_size = sizeof pi;

        if (GetPlayer(id2, pi) && pi.is_bot && s.name == pi.name)
            KickPlayer(id2, "bot could not be added");
    }

    return BotAddState::Failed;
}

bool AdapterBase::BotRemove(const BotRef& bot) {
    std::string out;

    if (!HasTemplate("bot_remove") || !RunTemplate("bot_remove", {{"bot", I(bot.botId)}, {"botname", bot.name}}, &out))
        return false;

    // Refractor-style replies: "Bot <name> id: <n> has been ..." or "Failed to ...".
    return !IContains(out, "fail") && !StartsWith(Trim(out), "Error") && !IContains(out, "Unknown object or method");
}

bool AdapterBase::BotRemoveAll() {
    std::string out;

    return BotQuery("bot_remove_all", {}, out);
}

bool AdapterBase::BotMoveTo(const BotRef& bot, const rmm_vec3& pos, float radius) {
    if (radius <= 0)
        radius = static_cast<float>(std::atof(data_.Get("bots", "move_radius", "5").c_str()));

    std::string out;

    return BotQuery("bot_move",
                    {{"bot", I(bot.botId)},
                     {"radius", Format("%.2f", radius)},
                     {"pos", Vec3Arg(pos)},
                     {"x", Format("%.2f", pos.x)},
                     {"y", Format("%.2f", pos.y)},
                     {"z", Format("%.2f", pos.z)}},
                    out);
}

int32_t AdapterBase::BotMaxPatrolPoints() const {
    return std::clamp(std::atoi(data_.Get("bots", "patrol_points", "2").c_str()), 1, 3);
}

bool AdapterBase::BotPatrol(const BotRef& bot, const std::vector<rmm_vec3>& points, float radius) {
    if (points.empty() || static_cast<int32_t>(points.size()) > BotMaxPatrolPoints())
        return false;

    if (radius <= 0)
        radius = static_cast<float>(std::atof(data_.Get("bots", "move_radius", "5").c_str()));

    std::map<std::string, std::string> a{
        {"bot", I(bot.botId)}, {"radius", Format("%.2f", radius)}, {"count", I(static_cast<int64_t>(points.size()))}};

    for (size_t i = 0; i < 3; ++i)  // the console wants every slot: repeat the last point
        a["p" + std::to_string(i + 1)] = Vec3Arg(points[std::min(i, points.size() - 1)]);

    std::string out;

    return BotQuery("bot_patrol", a, out);
}

bool AdapterBase::BotOrder(const BotRef& bot, const std::string& behaviour, const std::string& modifiers) {
    std::string b = Token(behaviour);

    if (b.empty())
        return false;

    std::string m = Token(modifiers.empty() ? data_.Get("bots", "default_modifiers", "StandardWeights") : modifiers);
    std::string out;

    return BotQuery("bot_order", {{"bot", I(bot.botId)}, {"behaviour", b}, {"modifiers", m}}, out);
}

bool AdapterBase::BotGetStrategy(int32_t team, std::string& out) {
    return BotQuery("bot_strategy", {{"team", I(team)}}, out) && !out.empty();
}

std::vector<std::string> AdapterBase::BotStrategies(int32_t team) {
    std::string out;

    if (!BotQuery("bot_strategies", {{"team", I(team)}}, out))
        return {};

    return IndexedNames(out);
}

bool AdapterBase::BotSetStrategy(int32_t team, const std::string& strategy) {
    // Only names the team's strategic AI knows: an unknown one is never sent.
    auto known = BotStrategies(team);
    auto it = std::find_if(known.begin(), known.end(), [&](const std::string& k) { return IEquals(k, strategy); });

    if (it == known.end())
        return false;

    std::string out;

    return BotQuery("bot_set_strategy", {{"team", I(team)}, {"strategy", *it}}, out);
}

bool AdapterBase::BotGetSkill(float& skill) {
    std::string out;
    double d;

    if (!BotQuery("bot_skill", {}, out) || !ParseDouble(out, d))
        return false;

    skill = static_cast<float>(d);

    return true;
}

bool AdapterBase::BotSetSkill(float skill) {
    std::string out;

    return BotQuery("bot_set_skill", {{"skill", Format("%.3f", std::clamp(skill, 0.0f, 1.0f))}}, out);
}

}  // namespace rmm
