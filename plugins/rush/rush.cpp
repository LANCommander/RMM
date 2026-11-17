// rush - Rush for RMM.
//
// One team attacks, the other defends. The map's control points are played in
// stages: the attackers must capture every point of the current stage (usually
// two). When they do, the defenders fall back to the next stage, and the
// attackers' tickets are topped up again. The attackers win by capturing the
// last stage. The defenders win when the attackers run out of tickets (one per
// attacker death) or when a stage's time runs out.
//
//  - Only the current stage's points can be captured. Points the attackers have
//    taken are theirs for good; later stages' points and the main bases are
//    locked (RMM_CAP_CONTROL_POINT_LOCK). At round start every stage point is
//    given to the defenders.
//  - Where the stages come from, first match wins:
//      1. the map's own Rush points: control points named
//         <rush.prefix>[<stage>_]<name>, e.g. Rush_1_EastOpenBase. The team the
//         map gives them to defends;
//      2. rmm/configs/rush/maps/<map>.cfg (see configs/rush/README.cfg);
//      3. with rush.auto 1, the map's points that aren't a main base, nearest
//         to the attackers' base first, rush.pointsPerStage to a stage.
//    With none of these the map plays normally.
//  - Rotations mix modes: Rush only runs in rush.modes on rush.maps. Any other
//    round plays normally, with the map's Rush points neutral and locked
//    (rush.hidePoints), so a Rush map can also be played as conquest.
//  - The attackers' ticket counter shows their tickets. The defenders' counter
//    shows the seconds left in the stage (rush.clockTickets), else stays put.
//
// Commands (console and chat, e.g. !rush):
//   rush                       the stage, its points, tickets and time
//   rush skip | restart | reload           admin flag h (config)
//
// Needs control points (list, set, lock) and tickets; end_round to finish the round.
#include <rmm/rmm.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

const std::string kPrefix = "[Rush] ";

std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    return s;
}

std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");

    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

bool Glob(const char* p, const char* s) {
    const char *star = nullptr, *resume = nullptr;

    while (*s) {
        if (*p == '*') {
            star = p++;
            resume = s;
            continue;
        }

        if (*p == '?' || std::tolower(static_cast<unsigned char>(*p)) == std::tolower(static_cast<unsigned char>(*s))) {
            ++p;
            ++s;
            continue;
        }

        if (!star)
            return false;

        p = star + 1;
        s = ++resume;
    }

    while (*p == '*')
        ++p;

    return *p == '\0';
}

bool GlobList(const std::string& patterns, const std::string& name) {
    std::stringstream ss(patterns);
    std::string pat;

    while (std::getline(ss, pat, ',')) {
        pat = Trim(pat);

        if (!pat.empty() && Glob(pat.c_str(), name.c_str()))
            return true;
    }

    return false;
}

bool FileExists(const std::string& path) { return std::ifstream(path).good(); }

std::string Clock(double seconds) {
    int s = static_cast<int>(std::ceil(std::max(0.0, seconds)));

    return rmm::Format("%d:%02d", s / 60, s % 60);
}

/// One stage as written in a map file: point names and its own limits (-1 = default).
struct StagePlan {
    std::vector<std::string> points;
    int tickets = -1;
    int time = -1;
    int line = 0;
};

struct MapPlan {
    int attackers = 0;  // 0 = rush.attackers
    int tickets = -1;   // -1 = rush.tickets
    int time = -1;      // -1 = rush.stageTime
    std::vector<StagePlan> stages;
};

/// Map file lines: `attackers <1|2>`, `tickets <n>`, `time <seconds>`,
/// `stage <point> [<point> ...] [tickets=<n>] [time=<seconds>]`. # and // start comments.
bool ParsePlan(const std::string& path, MapPlan& out, std::string& err) {
    std::ifstream f(path);

    if (!f) {
        err = "cannot open " + path;

        return false;
    }

    std::string raw;
    int lineNo = 0;

    while (std::getline(f, raw)) {
        ++lineNo;
        size_t c = std::min(raw.find('#'), raw.find("//"));
        std::string line = Trim(c == std::string::npos ? raw : raw.substr(0, c));

        if (line.empty())
            continue;

        std::stringstream ss(line);
        std::string key, word;
        ss >> key;
        key = Lower(key);
        auto number = [&](int& v, int lo) {
            if (!(ss >> word) || word.find_first_not_of("0123456789") != std::string::npos ||
                std::atoi(word.c_str()) < lo) {
                err = rmm::Format("%s:%d: '%s' needs a number", path.c_str(), lineNo, key.c_str());

                return false;
            }

            v = std::atoi(word.c_str());

            return true;
        };

        if (key == "attackers") {
            if (!number(out.attackers, 1))
                return false;

            if (out.attackers > 2) {
                err = rmm::Format("%s:%d: attackers is 1 or 2", path.c_str(), lineNo);

                return false;
            }
        } else if (key == "tickets") {
            if (!number(out.tickets, 1))
                return false;
        } else if (key == "time") {
            if (!number(out.time, 0))
                return false;
        } else if (key == "stage") {
            StagePlan s;
            s.line = lineNo;

            while (ss >> word) {
                size_t eq = word.find('=');

                if (eq == std::string::npos) {
                    s.points.push_back(word);
                    continue;
                }

                std::string k = Lower(word.substr(0, eq)), v = word.substr(eq + 1);

                if (v.empty() || v.find_first_not_of("0123456789") != std::string::npos ||
                    (k != "tickets" && k != "time")) {
                    err = rmm::Format("%s:%d: unknown option '%s' (tickets=<n>, time=<seconds>)", path.c_str(), lineNo,
                                      word.c_str());

                    return false;
                }

                (k == "tickets" ? s.tickets : s.time) = std::atoi(v.c_str());
            }

            if (s.points.empty()) {
                err = rmm::Format("%s:%d: a stage needs at least one control point", path.c_str(), lineNo);

                return false;
            }

            out.stages.push_back(s);
        } else {
            err = rmm::Format("%s:%d: expected attackers, tickets, time or stage, got '%s'", path.c_str(), lineNo,
                              key.c_str());

            return false;
        }
    }

    return true;
}

struct Point {
    rmm_object_t cp = 0;
    std::string name;  // template name
    rmm_vec3 pos{};
    bool captured = false;
    std::string shown;  // name told to players, "" = name

    const std::string& Shown() const { return shown.empty() ? name : shown; }
};

/// `<prefix>[<stage>_]<name>`, e.g. Rush_1_EastOpenBase: the map's own Rush
/// objective. @p stage is 0 when no number is given.
bool Tagged(const std::string& prefix, const std::string& tmpl, int& stage, std::string& shown) {
    if (prefix.empty() || tmpl.size() <= prefix.size() || Lower(tmpl.substr(0, prefix.size())) != Lower(prefix))
        return false;

    std::string rest = tmpl.substr(prefix.size());
    size_t digits = 0;

    while (digits < rest.size() && std::isdigit(static_cast<unsigned char>(rest[digits])))
        ++digits;

    stage = 0;

    if (digits > 0 && digits + 1 < rest.size() && rest[digits] == '_') {
        stage = std::atoi(rest.substr(0, digits).c_str());
        rest = rest.substr(digits + 1);
    }

    shown = rest;

    return true;
}

struct Stage {
    std::vector<Point> points;
    int tickets = 0;
    int time = 0;  // seconds, 0 = no limit
};

}  // namespace

class Rush : public rmm::Plugin {
public:
    bool OnLoad(bool late, std::string& error) override {
        for (rmm_cap cap : {RMM_CAP_CONTROL_POINTS, RMM_CAP_CONTROL_POINT_SET, RMM_CAP_TICKETS}) {
            if (rmm::HasCap(cap))
                continue;

            error = std::string("Rush needs control points and tickets, which ") + rmm::Api().game->name() +
                    " doesn't offer (rmm caps)";

            return false;
        }

        if (!rmm::HasCap(RMM_CAP_CONTROL_POINT_LOCK))
            rmm::Warn("this game can't lock control points: captures outside the stage are undone instead");

        const uint32_t num = RMM_VAR_NUMERIC;
        enabled_ = rmm::Var("rush.enabled", "1", "Rush on (1) or off (0)", num);
        prefix_ = rmm::Var("rush.prefix", "Rush_",
                           "Control points named <prefix>[<stage>_]<name> are a map's Rush objectives");
        maps_ = rmm::Var("rush.maps", "*", "Maps Rush may run on (globs, comma separated); others play normally");
        auto_ = rmm::Var("rush.auto", "0",
                         "Maps with neither Rush points nor rmm/configs/rush/maps/<map>.cfg: make stages from the "
                         "map's control points (1) or play normally (0)",
                         num);
        modes_ = rmm::Var("rush.modes", "*cq*,*conquest*",
                          "Game modes Rush runs in (globs, comma separated; BF1942: GPM_CQ)");
        attackers_ = rmm::Var("rush.attackers", "0",
                              "Attacking team unless the map says: 1, 2, or 0 = the team holding fewer points", num);
        hidePoints_ = rmm::Var("rush.hidePoints", "1",
                               "Rounds that aren't Rush: the map's Rush points are made neutral and locked", num);
        tickets_ = rmm::Var("rush.tickets", "50", "Attacker tickets per stage (one per attacker death)", num);
        stageTime_ = rmm::Var("rush.stageTime", "600", "Seconds the attackers have per stage (0 = no limit)", num);
        pointsPerStage_ = rmm::Var("rush.pointsPerStage", "2", "Control points per stage (made-up stages)", num);
        clockTickets_ = rmm::Var("rush.clockTickets", "1",
                                 "The defenders' ticket counter shows the seconds left in the stage", num);
        tickets_.Bounds(1.0, 100000.0);
        stageTime_.Bounds(0.0, 86400.0);
        pointsPerStage_.Bounds(1.0, 16.0);
        attackers_.Bounds(0.0, 2.0);

        enabled_.OnChange([this](const std::string&, const std::string&) {
            if (!enabled_.Bool())
                Stop("Rush is off.");

            Start();  // off: the round goes on normally, with the Rush points hidden
        });

        HookEvents();
        RegisterCommands();
        rmm::Every(1.0, [this] {
            Tick();

            return true;
        });

        (void)late;

        if (rmm::Api().game->status() == RMM_STATUS_PLAYING)
            Start();

        return true;
    }

    void OnUnload() override { Stop(""); }

private:
    double Now() const { return rmm::Api().game->time(); }

    int Defenders() const { return 3 - attack_; }

    int StageCount() const { return static_cast<int>(stages_.size()); }

    // ---------------------------------------------------------------- setup --

    /// Gun game owns the round when it runs (both change kits, tickets and spawns).
    bool GunGameOn() const {
        rmm_handle_t gg = rmm::Api().vars->find("gungame.enabled");

        return gg != RMM_INVALID_HANDLE && rmm::Api().vars->get_bool(gg);
    }

    struct CpRef {
        rmm_object_t cp;
        rmm_control_point_info info;
    };

    std::vector<CpRef> AllPoints() {
        std::vector<CpRef> out;

        for (rmm_object_t cp : rmm::ControlPoints())
            if (auto i = rmm::ControlPointInfo(cp))
                out.push_back(CpRef{cp, *i});

        return out;
    }

    /// Stages from a map file. False (with a log line) if it doesn't fit the map.
    bool StagesFromFile(const std::string& path, const std::vector<CpRef>& all) {
        MapPlan plan;
        std::string err;

        if (!ParsePlan(path, plan, err)) {
            rmm::Error("%s - Rush is idle", err.c_str());

            return false;
        }

        std::map<std::string, const CpRef*> byName;

        for (auto& r : all)
            byName[Lower(r.info.name)] = &r;

        std::map<rmm_object_t, int> used;

        for (auto& sp : plan.stages) {
            Stage s;
            s.tickets = sp.tickets > 0 ? sp.tickets : plan.tickets > 0 ? plan.tickets : tickets_.Int();
            s.time = sp.time >= 0 ? sp.time : plan.time >= 0 ? plan.time : stageTime_.Int();

            for (auto& name : sp.points) {
                auto it = byName.find(Lower(name));

                if (it == byName.end()) {
                    rmm::Error("%s:%d: no control point '%s' on this map (rmm cps lists them) - Rush is idle",
                               path.c_str(), sp.line, name.c_str());

                    return false;
                }

                if (used.count(it->second->cp)) {
                    rmm::Error("%s:%d: '%s' is already in stage %d - Rush is idle", path.c_str(), sp.line, name.c_str(),
                               used[it->second->cp] + 1);

                    return false;
                }

                used[it->second->cp] = StageCount();
                s.points.push_back(
                    Point{it->second->cp, it->second->info.name, it->second->info.position, false, std::string()});
            }

            stages_.push_back(s);
        }

        attack_ = plan.attackers ? plan.attackers : attackers_.Int();

        return !stages_.empty();
    }

    bool IsTagged(const CpRef& r) const {
        int stage;
        std::string shown;

        return Tagged(prefix_.Str(), r.info.name, stage, shown);
    }

    /// The attackers' home points among @p pool: their bases, else whatever they hold.
    std::vector<const CpRef*> Home(const std::vector<const CpRef*>& pool) const {
        std::vector<const CpRef*> home;

        for (auto* r : pool)
            if (r->info.team == attack_ && (r->info.fixed || r->info.only_team == attack_))
                home.push_back(r);

        if (home.empty())  // no fixed base: whatever the attackers start with is home
            for (auto* r : pool)
                if (r->info.team == attack_)
                    home.push_back(r);

        return home;
    }

    /// Append stages of rush.pointsPerStage points each, nearest to @p home first
    /// (map order when there's no home).
    void AppendByDistance(std::vector<const CpRef*> cand, const std::vector<const CpRef*>& home,
                          const std::map<rmm_object_t, std::string>& shown = {}) {
        rmm_vec3 c{0, 0, 0};

        for (auto* h : home) {
            c.x += h->info.position.x / static_cast<float>(home.size());
            c.z += h->info.position.z / static_cast<float>(home.size());
        }

        auto dist = [&](const CpRef* r) {
            float dx = r->info.position.x - c.x, dz = r->info.position.z - c.z;

            return dx * dx + dz * dz;
        };

        if (!home.empty())
            std::stable_sort(cand.begin(), cand.end(),
                             [&](const CpRef* a, const CpRef* b) { return dist(a) < dist(b); });

        size_t per = static_cast<size_t>(std::max(1, pointsPerStage_.Int()));

        for (size_t i = 0; i < cand.size(); i += per) {
            Stage s;
            s.tickets = tickets_.Int();
            s.time = stageTime_.Int();

            for (size_t k = i; k < cand.size() && k < i + per; ++k) {
                auto it = shown.find(cand[k]->cp);
                s.points.push_back(Point{cand[k]->cp, cand[k]->info.name, cand[k]->info.position, false,
                                         it == shown.end() ? std::string() : it->second});
            }

            stages_.push_back(s);
        }
    }

    /// Stages from the map's own Rush points (rush.prefix). The team the map gives
    /// them to defends. Numbered points form the stages in number order; any
    /// without a number follow, grouped nearest to the attackers first.
    bool StagesFromTags(const std::vector<CpRef>& all) {
        std::map<int, Stage> numbered;
        std::vector<const CpRef*> loose, rest;
        std::map<rmm_object_t, std::string> shown;
        int owner = 0;
        bool mixed = false;

        for (auto& r : all) {
            int stage;
            std::string name;

            if (!Tagged(prefix_.Str(), r.info.name, stage, name)) {
                rest.push_back(&r);
                continue;
            }

            if (r.info.team == 1 || r.info.team == 2) {
                mixed = mixed || (owner && owner != r.info.team);
                owner = r.info.team;
            }

            shown[r.cp] = name;

            if (stage <= 0) {
                loose.push_back(&r);
                continue;
            }

            Stage& s = numbered[stage];
            s.tickets = tickets_.Int();
            s.time = stageTime_.Int();
            s.points.push_back(Point{r.cp, r.info.name, r.info.position, false, name});
        }

        if (owner && !mixed)
            attack_ = 3 - owner;
        else if (attack_ != 1 && attack_ != 2)
            attack_ = AutoAttackers(all);

        for (auto& kv : numbered)
            stages_.push_back(kv.second);

        if (!loose.empty())
            AppendByDistance(loose, Home(rest), shown);

        if (Home(rest).empty())
            rmm::Warn("the attackers (team %d) hold no point outside the Rush points: they may have nowhere to spawn",
                      attack_);

        return !stages_.empty();
    }

    /// Stages made from the map: every point that isn't a base, nearest to the attackers first.
    bool StagesFromMap(const std::vector<CpRef>& all) {
        std::vector<const CpRef*> pool;

        for (auto& r : all)
            pool.push_back(&r);

        std::vector<const CpRef*> home = Home(pool);

        if (home.empty()) {
            rmm::Warn("the attackers (team %d) hold no control point to spawn at - write "
                      "configs/rush/maps/<map>.cfg; Rush is idle",
                      attack_);

            return false;
        }

        std::vector<const CpRef*> cand;

        for (auto* r : pool) {
            bool isHome = std::find(home.begin(), home.end(), r) != home.end();

            if (!isHome && !r->info.fixed && r->info.only_team == 0)
                cand.push_back(r);
        }

        if (cand.empty()) {
            rmm::Warn("no capturable control points on this map - Rush is idle");

            return false;
        }

        AppendByDistance(cand, home);

        return true;
    }

    /// The attacking team when nobody chose one: whoever holds fewer points (ties: team 2).
    int AutoAttackers(const std::vector<CpRef>& all) {
        int held[3] = {0, 0, 0};

        for (auto& r : all)
            if (r.info.team == 1 || r.info.team == 2)
                ++held[r.info.team];

        return held[1] < held[2] ? 1 : 2;
    }

    /// A fresh round of Rush on the current map, if it applies here.
    void Start() {
        Unlock();
        stages_.clear();
        others_.clear();
        active_ = false;
        over_ = false;
        stage_ = 0;
        rmm_map_info m{};
        m.struct_size = sizeof m;
        rmm::Api().game->current_map(&m);
        map_ = m.map;
        std::vector<CpRef> all = AllPoints();
        // Not a Rush round (off, another mode, a map left out): play normally, with
        // the map's Rush points out of the way.
        const char* why = !enabled_.Bool()                  ? "rush.enabled is 0"
                          : GunGameOn()                     ? "gun game is on (gungame.enabled 1)"
                          : !GlobList(maps_.Str(), map_)    ? "the map isn't in rush.maps"
                          : !GlobList(modes_.Str(), m.mode) ? "the mode isn't in rush.modes"
                          : all.empty()                     ? "the map has no control points"
                                                            : nullptr;

        if (why) {
            rmm::Debug("%s %s: not Rush (%s)", map_.c_str(), m.mode, why);
            Hide(all);

            return;
        }

        std::string path = rmm::Path(RMM_PATH_CONFIGS, "rush/maps/" + Lower(map_) + ".cfg");
        bool tagged = std::any_of(all.begin(), all.end(), [this](const CpRef& r) { return IsTagged(r); });
        bool haveFile = !map_.empty() && FileExists(path);
        std::string source;
        bool ok = false;
        attack_ = attackers_.Int();

        if (tagged) {  // the map's own layout comes first
            source = "the map's " + prefix_.Str() + " points";
            ok = StagesFromTags(all);
        } else if (haveFile) {
            source = path;
            ok = StagesFromFile(path, all);  // the file may name the attackers

            if (attack_ != 1 && attack_ != 2)
                attack_ = AutoAttackers(all);
        } else if (auto_.Bool()) {
            source = "made from the map";

            if (attack_ != 1 && attack_ != 2)
                attack_ = AutoAttackers(all);

            ok = StagesFromMap(all);
        } else {
            rmm::Debug("%s: no %s points, no %s and rush.auto 0: not Rush", map_.c_str(), prefix_.Str().c_str(),
                       path.c_str());
        }

        if (!ok) {
            stages_.clear();
            Hide(all);

            return;
        }

        // Every stage point belongs to the defenders; everything else stays as it is, locked.
        applying_ = true;

        for (auto& r : all) {
            bool inStage = false;

            for (auto& s : stages_)
                for (auto& p : s.points)
                    inStage = inStage || p.cp == r.cp;

            if (inStage) {
                if (r.info.team != Defenders())
                    rmm::SetControlPoint(r.cp, Defenders());
            } else {
                others_.push_back(r.cp);
                rmm::LockControlPoint(r.cp, true);
            }
        }

        applying_ = false;
        startTickets_ = std::max(1, rmm::Api().game->get_tickets(Defenders()));
        active_ = true;
        stage_ = -1;
        rmm::Info("%s: team %d attacks, %d stage(s) (%s)", map_.c_str(), attack_, StageCount(), source.c_str());
        rmm::SayAll(kPrefix + rmm::Format("Rush! Team %d attacks, team %d defends: %d stage(s).", attack_, Defenders(),
                                          StageCount()));
        NextStage();
    }

    /// Stop running Rush this round: unlock what we locked (owners stay as they are).
    void Stop(const std::string& why) {
        bool was = active_;
        active_ = false;
        Unlock();

        if (was && !why.empty())
            rmm::SayAll(kPrefix + why);
    }

    void Unlock() {
        if (!rmm::HasCap(RMM_CAP_CONTROL_POINT_LOCK))
            return;

        for (auto& s : stages_)
            for (auto& p : s.points)
                rmm::LockControlPoint(p.cp, false);

        for (rmm_object_t cp : others_)
            rmm::LockControlPoint(cp, false);

        for (rmm_object_t cp : hidden_)
            rmm::LockControlPoint(cp, false);

        hidden_.clear();
    }

    /// A round that isn't Rush: the map's Rush points go neutral and locked, so
    /// they give no spawns and nobody can take them (rush.hidePoints). They still
    /// show as neutral flags.
    void Hide(const std::vector<CpRef>& all) {
        if (!hidePoints_.Bool())
            return;

        applying_ = true;

        for (auto& r : all) {
            if (!IsTagged(r))
                continue;

            if (r.info.team != RMM_TEAM_NONE)
                rmm::SetControlPoint(r.cp, RMM_TEAM_NONE);

            if (rmm::LockControlPoint(r.cp, true))
                hidden_.push_back(r.cp);
        }

        applying_ = false;

        if (!hidden_.empty())
            rmm::Info("%s: not a Rush round; %zu Rush point(s) neutral and locked", map_.c_str(), hidden_.size());
    }

    // --------------------------------------------------------------- stages --

    std::string PointList(const Stage& s) const {
        std::string out;

        for (size_t i = 0; i < s.points.size(); ++i) {
            if (i)
                out += i + 1 == s.points.size() ? " and " : ", ";

            out += s.points[i].Shown();
        }

        return out;
    }

    void NextStage() {
        ++stage_;

        if (stage_ >= StageCount()) {
            Finish(attack_, "The attackers took every objective!");

            return;
        }

        Stage& s = stages_[static_cast<size_t>(stage_)];
        applying_ = true;

        for (auto& p : s.points) {
            auto i = rmm::ControlPointInfo(p.cp);

            if (i && i->team != Defenders())
                rmm::SetControlPoint(p.cp, Defenders());

            rmm::LockControlPoint(p.cp, false);
        }

        for (size_t k = static_cast<size_t>(stage_) + 1; k < stages_.size(); ++k)
            for (auto& p : stages_[k].points)
                rmm::LockControlPoint(p.cp, true);

        applying_ = false;
        ticketsLeft_ = s.tickets;
        stageEnds_ = s.time > 0 ? Now() + s.time : 0;
        warned_ = 1 << 30;  // the last "seconds left" mark announced
        std::string limits = rmm::Format("%d tickets", ticketsLeft_);

        if (s.time > 0)
            limits += ", " + Clock(s.time);

        if (stage_ > 0)
            rmm::SayAll(kPrefix + "Stage cleared! The defenders fall back.", rmm::PaceImmediate());

        rmm::SayAll(kPrefix + rmm::Format("Stage %d/%d: capture %s (%s).", stage_ + 1, StageCount(),
                                          PointList(s).c_str(), limits.c_str()));
        WriteTickets();
    }

    void Finish(int winner, const std::string& why) {
        if (over_)
            return;

        over_ = true;
        active_ = false;
        rmm::SayAll(kPrefix + why + rmm::Format(" Team %d wins.", winner), rmm::PaceImmediate());
        rmm::Info("round over: team %d wins (%s)", winner, why.c_str());

        if (rmm::HasCap(RMM_CAP_END_ROUND) && rmm::EndRound(winner))
            return;

        rmm::Warn("end_round unavailable or refused: the round goes on without Rush");
        Unlock();
    }

    /// The attackers' counter shows their tickets; the defenders' the clock (or stays put).
    void WriteTickets() {
        if (!active_)
            return;

        auto& g = *rmm::Api().game;

        if (g.get_tickets(attack_) != ticketsLeft_)
            g.set_tickets(attack_, std::max(ticketsLeft_, 1));

        int def = startTickets_;

        if (clockTickets_.Bool() && stageEnds_ > 0)
            // Never near 0: the engine ends the round when a team's tickets reach 0, and
            // its ticket bleed takes one now and then between two of our writes.
            def = std::max(5, static_cast<int>(std::ceil(stageEnds_ - Now())));

        if (g.get_tickets(Defenders()) != def)
            g.set_tickets(Defenders(), def);
    }

    void Tick() {
        if (!active_)
            return;

        if (stageEnds_ > 0) {
            double left = stageEnds_ - Now();

            if (left <= 0) {
                Finish(Defenders(), "Time is up!");

                return;
            }

            for (int mark : {60, 30, 10}) {
                if (left <= mark && mark < warned_ && left > mark - 1.5) {
                    warned_ = mark;
                    rmm::SayAll(kPrefix + rmm::Format("%d seconds left to take %s!", mark,
                                                      PointList(stages_[static_cast<size_t>(stage_)]).c_str()));
                }
            }
        }

        WriteTickets();
    }

    /// Where a point is in the plan: stage index (or -1) and the point.
    Point* Find(rmm_object_t cp, const char* name, int& stage) {
        for (size_t k = 0; k < stages_.size(); ++k)
            for (auto& p : stages_[k].points)
                if ((cp && p.cp == cp) || (!cp && name && Lower(p.name) == Lower(name))) {
                    stage = static_cast<int>(k);

                    return &p;
                }

        stage = -1;

        return nullptr;
    }

    void OnPointChanged(const rmm_ev_control_point& e) {
        int stage = -1;
        Point* p = Find(e.control_point, e.name, stage);
        bool other = !p && std::find(others_.begin(), others_.end(), e.control_point) != others_.end();

        if (!p && !other)
            return;

        if (!p || stage != stage_ || p->captured) {
            // Not this stage's: it must not change (only happens without locks).
            int keep = !p ? e.old_team : stage < stage_ || p->captured ? attack_ : Defenders();

            if (e.new_team != keep)
                reverts_.push_back(std::make_pair(e.control_point, keep));

            return;
        }

        if (e.new_team == attack_) {
            p->captured = true;
            rmm::LockControlPoint(p->cp, true);
            Stage& s = stages_[static_cast<size_t>(stage_)];
            int left = 0;

            for (auto& q : s.points)
                left += q.captured ? 0 : 1;

            rmm::SayAll(kPrefix + rmm::Format("The attackers captured %s%s", p->Shown().c_str(),
                                              left ? rmm::Format(" - %d to go.", left).c_str() : "!"));

            if (!left)
                advance_ = true;  // from the frame event, not inside the engine's capture
        } else if (e.new_team == RMM_TEAM_NONE && e.old_team == Defenders()) {
            rmm::SayTeam(Defenders(), kPrefix + p->Shown() + " is falling! Get back there!");
        } else if (e.new_team == Defenders()) {
            rmm::SayAll(kPrefix + "The defenders hold " + p->Shown() + ".");
        }
    }

    // --------------------------------------------------------------- events --

    void HookEvents() {
        rmm::On<rmm_ev_map>(RMM_EV_ROUND_START, [this](rmm_ev_map&) {
            Start();

            return RMM_CONTINUE;
        });
        rmm::On<rmm_ev_round_end>(RMM_EV_ROUND_END, [this](rmm_ev_round_end&) {
            over_ = true;
            active_ = false;

            return RMM_CONTINUE;
        });
        rmm::On<rmm_ev_frame>(RMM_EV_FRAME, [this](rmm_ev_frame&) {
            if (!active_)
                return RMM_CONTINUE;

            if (!reverts_.empty()) {
                auto batch = std::move(reverts_);
                reverts_.clear();
                applying_ = true;

                for (auto& r : batch)
                    rmm::SetControlPoint(r.first, r.second);

                applying_ = false;
            }

            if (advance_) {
                advance_ = false;
                NextStage();
            }

            WriteTickets();

            return RMM_CONTINUE;
        });
        rmm::On<rmm_ev_control_point>(RMM_EV_CONTROL_POINT, [this](rmm_ev_control_point& e) {
            if (active_ && !applying_)
                OnPointChanged(e);

            return RMM_CONTINUE;
        });
        rmm::On<rmm_ev_player_killed>(RMM_EV_PLAYER_KILLED, [this](rmm_ev_player_killed& e) {
            if (!active_)
                return RMM_CONTINUE;

            auto victim = rmm::GetPlayer(e.victim);

            if (!victim || victim->team != attack_)
                return RMM_CONTINUE;

            if (--ticketsLeft_ <= 0) {
                ticketsLeft_ = 0;
                Finish(Defenders(), "The attackers ran out of tickets!");

                return RMM_CONTINUE;
            }

            if (ticketsLeft_ == 10 || ticketsLeft_ == 5)
                rmm::SayAll(kPrefix + rmm::Format("The attackers have %d tickets left.", ticketsLeft_));

            WriteTickets();

            return RMM_CONTINUE;
        });
    }

    // ------------------------------------------------------------- commands --

    void RegisterCommands() {
        rmm::Command("rush", RMM_CMD_ALL, 0, "Rush: the stage, objectives, tickets and time",
                     "[skip | restart | reload]", [this](rmm::CmdArgs& a) {
                         std::string sub = Lower(a.Arg(1));

                         if (sub == "skip" || sub == "restart" || sub == "reload") {
                             if (!rmm::HasFlags(a.Caller(), RMM_ADMFLAG_CONFIG)) {
                                 a.Reply("You do not have access to this command.");

                                 return;
                             }

                             if (sub == "skip") {
                                 if (!active_) {
                                     a.Reply("Rush is not running.");

                                     return;
                                 }

                                 applying_ = true;

                                 for (auto& p : stages_[static_cast<size_t>(stage_)].points) {
                                     p.captured = true;
                                     rmm::SetControlPoint(p.cp, attack_);
                                     rmm::LockControlPoint(p.cp, true);
                                 }

                                 applying_ = false;
                                 NextStage();

                                 return;
                             }

                             Start();  // restart and reload: the map file is read again
                             a.Reply(active_ ? "Rush restarted." : "Rush is idle on this map (see the log).");

                             return;
                         }

                         if (!active_) {
                             a.Reply(over_ ? "Rush: the round is over." : "Rush is not running on this map.");

                             return;
                         }

                         const Stage& s = stages_[static_cast<size_t>(stage_)];
                         std::string line =
                             rmm::Format("Stage %d/%d, team %d attacks: %d ticket%s", stage_ + 1, StageCount(), attack_,
                                         ticketsLeft_, ticketsLeft_ == 1 ? "" : "s");

                         if (stageEnds_ > 0)
                             line += ", " + Clock(stageEnds_ - Now()) + " left";

                         a.Reply(line + ".");

                         for (auto& p : s.points) {
                             auto i = rmm::ControlPointInfo(p.cp);
                             const char* state = p.captured                        ? "captured"
                                                 : !i                              ? "?"
                                                 : i->team == Defenders()          ? "held by the defenders"
                                                 : i->state == RMM_CP_STATE_TAKING ? "being taken"
                                                                                   : "neutral";
                             a.Replyf("  %s: %s", p.Shown().c_str(), state);
                         }
                     });
    }

    // ----------------------------------------------------------------- data --

    rmm::Var enabled_, prefix_, maps_, auto_, modes_, attackers_, hidePoints_, tickets_, stageTime_, pointsPerStage_,
        clockTickets_;
    std::vector<rmm_object_t> hidden_;  // Rush points kept out of a round that isn't Rush
    std::string map_;
    std::vector<Stage> stages_;
    std::vector<rmm_object_t> others_;  // points outside the stages (bases): locked
    std::vector<std::pair<rmm_object_t, int>> reverts_;
    int attack_ = 2;
    int stage_ = 0;
    int ticketsLeft_ = 0;
    int startTickets_ = 1;
    double stageEnds_ = 0;  // 0 = no time limit
    int warned_ = 1 << 30;
    bool active_ = false, over_ = false, applying_ = false, advance_ = false;
};

RMM_DECLARE_PLUGIN(Rush, "Rush", "1.0.0", "RMM Team",
                   "Attackers take the map's control points stage by stage; defenders hold them off", "", "rush",
                   RMM_GAME_ANY, 0)
