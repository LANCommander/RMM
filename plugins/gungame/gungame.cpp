// gungame - Gun Game for RMM.
//
// Every player climbs the same ladder of levels. A level is either a kit
// (a weapon loadout) or a vehicle. Each kill moves the killer one step closer
// to the next level, and whoever finishes the last level wins the round.
//
//  - The ladder is a config file: rmm/configs/gungame/<gungame.ladder>.cfg,
//    or rmm/configs/gungame/maps/<map>.cfg when that file exists for the
//    current map. Levels whose template the running map doesn't know are
//    skipped (logged). See configs/gungame/default.cfg for the format.
//  - Vehicle levels spawn the vehicle and put the player in it at once: at the
//    nearest of the map's own vehicle spawn points (placed in the open by the
//    level designer; gungame.spotRange), else next to the player. By default
//    they can't get out (gungame.vehicleExit). If the owner is killed while
//    inside, the vehicle explodes (gungame.vehicleExplode).
//  - The map's own vehicles and guns don't spawn (gungame.mapVehicles, via
//    spawner events), and dead players may spawn again at once
//    (gungame.respawnTime, via spawn timing; restored when gun game stops).
//  - A knife kill costs the victim gungame.knifeDemote levels (default 1).
//  - Only kills with the level's weapon count (gungame.strictWeapons): a kit
//    level's weapons= list, and kills from the player's own vehicle on a
//    vehicle level. A kit can't be cut down to one weapon, so the pistol and
//    grenades it comes with still kill, but don't count. Knife kills count
//    everywhere (gungame.knifeCounts).
//
// Commands (console and chat, e.g. !gg):
//   gg                    your level (console: everyone's)
//   gg ladder | gg top    the ladder / the leaders
//   gg stuck              a fresh vehicle on a vehicle level (cooldown)
//   gg set <target> <lvl> | gg restart | gg reload      admin flag h (config)
//
// Needs (checked per level type at ladder load): set_kit for kit levels;
// object_spawn + put_in_vehicle for vehicle levels; object_damage
// for the explosion (without it the vehicle is removed instead).
#include <rmm/rmm.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

const std::string kPrefix = "[GG] ";

std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    return s;
}

std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");

    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

/// Case-insensitive glob (* and ?).
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

/// Any of the comma-separated globs in @p patterns matches @p name.
bool GlobList(const std::string& patterns, const char* name) {
    if (!name || !*name)
        return false;

    std::stringstream ss(patterns);
    std::string pat;

    while (std::getline(ss, pat, ',')) {
        pat = Trim(pat);

        if (!pat.empty() && Glob(pat.c_str(), name))
            return true;
    }

    return false;
}

bool FileExists(const std::string& path) { return std::ifstream(path).good(); }

enum class LevelType { Kit, Vehicle };

struct Level {
    LevelType type = LevelType::Kit;
    std::string tmpl;     // kit or vehicle template
    std::string name;     // shown to players (default: the template)
    std::string kit;      // vehicle levels: also give this kit (for on foot), "" = keep
    std::string weapons;  // weapon template globs whose kills count ("" = any; gungame.strictWeapons)
    int kills = 0;        // kills to finish this level; 0 = gungame.killsPerLevel
    float height = 1.0f;  // vehicles: spawn this high above the terrain
    int seat = 0;         // vehicles: the seat the player is put in
    int exit = -1;        // vehicles: -1 = gungame.vehicleExit, else 0/1 for this level
    int line = 0;
};

struct PlayerState {
    int level = 0;               // index into the ladder
    int kills = 0;               // kills at the current level
    rmm_object_t vehicle = 0;    // the gun game vehicle this player owns
    bool inVehicle = false;      // ...and sits in
    double lastGameExit = -100;  // last time the game took them out of it (a death)
    bool pending = false;        // the current level still has to be handed out
    double applyAt = 0;
    int attempts = 0;
    double lastNotice = -100;  // rate limit for "you can't..." messages
    double lastStuck = -1000;
    double holdAt = -1;  // when to put the level's weapon in their hands (lockWeapons), -1 = no
};

// A new kit's weapons reach the player's client a moment after the kit: a
// switch in the same tick left a BF1942 client stuck (view to the ground,
// position jumping), so the level's weapon is selected this much later.
constexpr double kHoldDelay = 1.0;

struct Cleanup {
    rmm_object_t vehicle;
    bool explode;
    rmm_player_t attacker;
};

/// Where one of the map's spawners puts its object (from RMM_EV_SPAWNER_SPAWN).
struct Spot {
    std::string tmpl;
    rmm_vec3 pos{};
    float yaw = 0;
};

float DistSq(const rmm_vec3& a, const rmm_vec3& b) {
    float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;

    return dx * dx + dy * dy + dz * dz;
}

/// Parse one ladder file. Lines: `kit <template> [opts]` or `vehicle <template> [opts]`,
/// opts are key=value (kills, name, kit, height, seat, exit). # and // start comments.
bool ParseLadder(const std::string& path, std::vector<Level>& out, std::string& err) {
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
        std::string type, tmpl, opt;
        ss >> type >> tmpl;
        Level L;
        L.line = lineNo;
        type = Lower(type);

        if (type == "kit")
            L.type = LevelType::Kit;
        else if (type == "vehicle")
            L.type = LevelType::Vehicle;
        else {
            err = rmm::Format("%s:%d: expected 'kit' or 'vehicle', got '%s'", path.c_str(), lineNo, type.c_str());

            return false;
        }

        if (tmpl.empty()) {
            err = rmm::Format("%s:%d: missing template name", path.c_str(), lineNo);

            return false;
        }

        L.tmpl = tmpl;

        while (ss >> opt) {
            size_t eq = opt.find('=');
            std::string k = Lower(opt.substr(0, eq)), v = eq == std::string::npos ? "" : opt.substr(eq + 1);

            if (eq == std::string::npos || v.empty()) {
                err = rmm::Format("%s:%d: option '%s' needs a value (key=value)", path.c_str(), lineNo, opt.c_str());

                return false;
            }

            if (k == "kills")
                L.kills = std::max(0, std::atoi(v.c_str()));
            else if (k == "name") {
                std::replace(v.begin(), v.end(), '_', ' ');
                L.name = v;
            } else if (k == "kit" && L.type == LevelType::Vehicle)
                L.kit = v;
            else if (k == "weapons")
                L.weapons = v;
            else if (k == "height" && L.type == LevelType::Vehicle)
                L.height = static_cast<float>(std::atof(v.c_str()));
            else if (k == "seat" && L.type == LevelType::Vehicle)
                L.seat = std::max(-1, std::atoi(v.c_str()));
            else if (k == "exit" && L.type == LevelType::Vehicle)
                L.exit = std::atoi(v.c_str()) != 0 ? 1 : 0;
            else {
                err = rmm::Format("%s:%d: unknown option '%s' for a %s level", path.c_str(), lineNo, k.c_str(),
                                  type.c_str());

                return false;
            }
        }

        if (L.name.empty())
            L.name = L.tmpl;

        out.push_back(L);
    }

    return true;
}

}  // namespace

class GunGame : public rmm::Plugin {
public:
    bool OnLoad(bool late, std::string& error) override {
        if (!rmm::HasCap(RMM_CAP_SET_KIT) && !rmm::HasCap(RMM_CAP_PUT_IN_VEHICLE)) {
            error = std::string("gun game needs set_kit or put_in_vehicle, which ") + rmm::Api().game->name() +
                    " doesn't offer (rmm caps)";

            return false;
        }

        const uint32_t num = RMM_VAR_NUMERIC;
        enabled_ = rmm::Var("gungame.enabled", "1", "Gun game on (1) or off (0)", num);
        ladderName_ = rmm::Var("gungame.ladder", "default",
                               "Ladder file: rmm/configs/gungame/<name>.cfg (a maps/<map>.cfg wins)");
        killsPerLevel_ = rmm::Var("gungame.killsPerLevel", "1", "Kills per level unless the level sets kills=", num);
        knifeWeapons_ = rmm::Var("gungame.knifeWeapons", "*knife*,*bayonet*",
                                 "Weapon templates that count as knife kills (globs, comma separated)");
        knifeDemote_ = rmm::Var("gungame.knifeDemote", "1", "Levels a knife kill costs the victim (0 = off)", num);
        knifeCounts_ = rmm::Var("gungame.knifeCounts", "1",
                                "A knife kill also counts towards the attacker's level, on any level", num);
        strictWeapons_ = rmm::Var("gungame.strictWeapons", "1",
                                  "Only kills with the level's weapon count: weapons= on the level, own vehicle on "
                                  "vehicle levels (0 = any kill)",
                                  num);
        lockWeapons_ = rmm::Var("gungame.lockWeapons", "0",
                                "Kit levels with weapons=: players can't switch to other weapons in the kit (the knife "
                                "stays if knifeCounts). BF1942: experimental",
                                num);
        suicideDemote_ = rmm::Var("gungame.suicideDemote", "0", "Levels a suicide costs (0 = off)", num);
        teamKills_ = rmm::Var("gungame.teamKills", "0", "Team kills count as kills", num);
        vehicleExit_ = rmm::Var("gungame.vehicleExit", "0", "Players may get out of their gun game vehicle", num);
        vehicleSwitch_ =
            rmm::Var("gungame.vehicleSwitch", "1", "Players may change seats (F1-F8) in their gun game vehicle", num);
        vehicleReenter_ =
            rmm::Var("gungame.vehicleReenter", "1", "Players who got out may get back into their own vehicle", num);
        otherVehicles_ = rmm::Var(
            "gungame.otherVehicles", "0",
            "Players may enter other vehicles (map vehicles, stationary guns, others' gun game vehicles)", num);
        explode_ =
            rmm::Var("gungame.vehicleExplode", "1", "A vehicle explodes when its owner is killed inside it", num);
        respawnDelay_ = rmm::Var("gungame.vehicleRespawn", "5",
                                 "Seconds until an owner who survived their vehicle gets a new one (-1 = never)", num);
        spawnDistance_ =
            rmm::Var("gungame.spawnDistance", "6", "Metres from the player where vehicles are spawned", num);
        pickups_ = rmm::Var("gungame.pickups", "0", "Players may pick up kits from the ground", num);
        endRound_ = rmm::Var("gungame.endRound", "1",
                             "End the round when someone finishes the ladder (0 = announce and start over)", num);
        levelScore_ = rmm::Var("gungame.levelScore", "1", "Show each player's level as their scoreboard score", num);
        holdTickets_ = rmm::Var("gungame.holdTickets", "1", "Keep both teams' tickets at their starting count", num);
        stuckCooldown_ = rmm::Var("gungame.stuckCooldown", "30", "Seconds between two 'gg stuck' per player", num);
        mapVehicles_ = rmm::Var("gungame.mapVehicles", "0",
                                "The map's own vehicles and stationary guns spawn (1) or not (0)", num);
        respawnTime_ = rmm::Var("gungame.respawnTime", "0",
                                "Seconds until a dead player may spawn again (-1 = the server's spawn settings)", num);
        spotRange_ = rmm::Var(
            "gungame.spotRange", "300",
            "Metres within which vehicles go to the map's vehicle spawn points (0 = always next to the player)", num);
        spawnDistance_.Bounds(3.0, 50.0);
        respawnTime_.Bounds(-1.0, 120.0);
        spotRange_.Bounds(0.0, 5000.0);

        // Off: our vehicles go (once empty) and nobody is held anywhere. On: a fresh start.
        enabled_.OnChange([this](const std::string&, const std::string&) { Restart(enabled_.Bool()); });
        respawnTime_.OnChange([this](const std::string&, const std::string&) { ApplySpawnTiming(); });
        mapVehicles_.OnChange([this](const std::string&, const std::string&) { sweepMapVehicles_ = true; });
        ladderName_.OnChange([this](const std::string&, const std::string&) { Reload(); });

        HookEvents();
        RegisterCommands();

        rmm::Every(1.0, [this] {
            Housekeeping();

            return true;
        });

        // Loaded while a round runs (late, or autoloaded after the first round started).
        (void)late;

        if (rmm::Api().game->status() == RMM_STATUS_PLAYING) {
            rmm_map_info m{};
            m.struct_size = sizeof m;

            if (rmm::Api().game->current_map(&m))
                map_ = m.map;

            Restart(true);
        }

        return true;
    }

    void OnUnload() override {
        // Leave the map clean: our empty vehicles go (occupied ones stay; removal would kill).
        for (auto& kv : owners_)
            RemoveIfEmpty(kv.first);

        RestoreSpawnTiming();
    }

private:
    // ---------------------------------------------------------------- state --

    bool Active() const { return enabled_.Bool() && !ladder_.empty() && !roundOver_; }

    double Now() const { return rmm::Api().game->time(); }

    int Count() const { return static_cast<int>(ladder_.size()); }

    PlayerState& State(rmm_player_t p) { return players_[p]; }

    int KillsNeeded(int level) const {
        int k = ladder_[static_cast<size_t>(level)].kills;

        return k > 0 ? k : std::max(1, killsPerLevel_.Int());
    }

    std::string LevelText(int level) const {
        return rmm::Format("Level %d/%d: %s", level + 1, Count(), ladder_[static_cast<size_t>(level)].name.c_str());
    }

    void Tell(rmm_player_t p, const std::string& msg) { rmm::Message(p, kPrefix + msg); }

    /// Rate-limited Tell, for messages a held key would repeat many times a second.
    void Notice(rmm_player_t p, const std::string& msg) {
        PlayerState& st = State(p);

        if (Now() - st.lastNotice < 3.0)
            return;

        st.lastNotice = Now();
        Tell(p, msg);
    }

    std::optional<rmm_object_info> Info(rmm_object_t o) { return o ? rmm::ObjectInfo(o) : std::nullopt; }

    rmm_object_t RootOf(rmm_object_t o) {
        auto i = Info(o);

        return i ? i->root : 0;
    }

    bool Usable(rmm_object_t v) {
        auto i = Info(v);

        return i && !i->is_destroyed;
    }

    // --------------------------------------------------------------- ladder --

    std::string LadderPath() const {
        if (!map_.empty()) {
            std::string perMap = rmm::Path(RMM_PATH_CONFIGS, "gungame/maps/" + Lower(map_) + ".cfg");

            if (FileExists(perMap))
                return perMap;
        }

        return rmm::Path(RMM_PATH_CONFIGS, "gungame/" + ladderName_.Str() + ".cfg");
    }

    /// Load the ladder for the current map, dropping levels this game/map can't hand out.
    void LoadLadder() {
        std::vector<Level> all;
        std::string err, path = LadderPath();
        ladder_.clear();

        if (!FileExists(path)) {
            rmm::Warn("no ladder file %s: gun game is idle", path.c_str());

            return;
        }

        if (!ParseLadder(path, all, err)) {
            rmm::Error("%s - gun game is idle", err.c_str());

            return;
        }

        const bool canKit = rmm::HasCap(RMM_CAP_SET_KIT);
        const bool canVehicle = rmm::HasCap(RMM_CAP_OBJECT_SPAWN) && rmm::HasCap(RMM_CAP_PUT_IN_VEHICLE);
        const bool canCheck = rmm::HasCap(RMM_CAP_OBJECT_QUERY);

        for (auto& L : all) {
            const char* why = nullptr;

            if (L.type == LevelType::Kit && !canKit)
                why = "kits can't be handed out on this game";
            else if (L.type == LevelType::Vehicle && !canVehicle)
                why = "vehicles can't be spawned/entered on this game";
            else if (canCheck && !rmm::TemplateExists(L.tmpl))
                why = "template not loaded on this map";
            else if (canCheck && !L.kit.empty() && !rmm::TemplateExists(L.kit))
                why = "its kit= template is not loaded on this map";

            if (why) {
                rmm::Warn("ladder line %d (%s %s) skipped: %s", L.line, L.type == LevelType::Kit ? "kit" : "vehicle",
                          L.tmpl.c_str(), why);
                continue;
            }

            ladder_.push_back(L);
        }

        rmm::Info("ladder %s: %d level(s)%s", path.c_str(), Count(), ladder_.empty() ? " - gun game is idle" : "");

        if (!explode_.Bool() || rmm::HasCap(RMM_CAP_OBJECT_DAMAGE))
            return;

        rmm::Warn(
            "this game can't damage objects (object_damage): vehicles of killed owners are removed, not exploded");
    }

    /// Re-read the ladder mid-round; levels are kept (clamped to the new length).
    void Reload() {
        LoadLadder();

        for (auto& kv : players_) {
            kv.second.level = std::min(kv.second.level, std::max(0, Count() - 1));
            kv.second.kills = 0;
            Schedule(kv.first, kv.second, 0.0);
        }
    }

    /// Everyone back to level 1; our vehicles are removed at the next frame.
    void Restart(bool reloadLadder) {
        if (reloadLadder)
            LoadLadder();

        roundOver_ = false;

        for (auto& kv : owners_)
            cleanup_.push_back(Cleanup{kv.first, false, RMM_PLAYER_NONE});

        owners_.clear();
        players_.clear();

        for (rmm_player_t p : rmm::Players()) {
            PlayerState& st = State(p);
            Schedule(p, st, 0.0);
            UpdateScore(p);
        }

        ApplySpawnTiming();
        sweepMapVehicles_ = true;
    }

    // ------------------------------------------------- map vehicles, respawns --

    /// Spawners are held back while gun game runs, and on a level that has a
    /// ladder from its load on (spawners fill the map before the round starts).
    bool BlockMapVehicles() const {
        if (!enabled_.Bool() || mapVehicles_.Bool())
            return false;

        return Active() || FileExists(LadderPath());
    }

    /// Map vehicles that spawned before gun game started (or while mapVehicles was 1)
    /// go once empty; their spawners then try again, and are blocked.
    void SweepMapVehicles() {
        sweepMapVehicles_ = false;

        if (!Active() || mapVehicles_.Bool() || !rmm::HasCap(RMM_CAP_OBJECT_REMOVE))
            return;

        int n = 0;

        for (rmm_object_t v : rmm::FindVehicles("", RMM_FIND_ROOTS_ONLY | RMM_FIND_EMPTY_ONLY))
            if (!owners_.count(v) && rmm::RemoveObject(v))
                ++n;

        if (n)
            rmm::Info("removed %d map vehicle(s)", n);
    }

    /// gungame.respawnTime while gun game runs; the server's own timing otherwise.
    void ApplySpawnTiming() {
        if (!rmm::HasCap(RMM_CAP_SPAWN_TIMING))
            return;

        double want = respawnTime_.Float();

        if (!Active() || want < 0) {
            RestoreSpawnTiming();

            return;
        }

        auto now = rmm::GetSpawnTiming();

        if (!now)
            return;

        if (!savedTiming_)
            savedTiming_ = now;

        rmm_spawn_timing t = *now;
        t.delay = static_cast<float>(want);
        t.waves = RMM_FALSE;

        if (now->waves || std::fabs(now->delay - t.delay) > 0.01f) {
            if (rmm::SetSpawnTiming(t))
                rmm::Info("respawn time %.1f s (spawn waves off)", t.delay);
        }
    }

    void RestoreSpawnTiming() {
        if (!savedTiming_)
            return;

        if (rmm::SetSpawnTiming(*savedTiming_))
            rmm::Info("server spawn timing restored (delay %.1f s, waves %s)", savedTiming_->delay,
                      savedTiming_->waves ? "on" : "off");

        savedTiming_.reset();
    }

    /// Free map vehicle spawn points (of any ladder vehicle) within gungame.spotRange, nearest first.
    std::vector<Spot> SpotsNear(const rmm_vec3& at) {
        std::vector<Spot> out;
        double range = spotRange_.Float();

        if (range <= 0 || spots_.empty())
            return out;

        std::set<std::string> ladderVehicles;

        for (auto& L : ladder_)
            if (L.type == LevelType::Vehicle)
                ladderVehicles.insert(Lower(L.tmpl));

        std::vector<rmm_vec3> parked;  // any vehicle on a spot makes it taken

        for (rmm_object_t v : rmm::FindVehicles("", RMM_FIND_ROOTS_ONLY))
            if (auto i = Info(v))
                parked.push_back(i->position);

        const bool ground = rmm::HasCap(RMM_CAP_GROUND_HEIGHT);

        for (auto& kv : spots_) {
            const Spot& s = kv.second;

            if (!ladderVehicles.count(Lower(s.tmpl)))
                continue;  // a boat, plane or gun spot

            if (DistSq(s.pos, at) > range * range)
                continue;

            if (ground) {  // on the terrain, not on a deck or in the water
                auto h = rmm::GroundHeight(s.pos.x, s.pos.z);

                if (h && std::fabs(s.pos.y - *h) > 3.0f)
                    continue;
            }

            bool taken = false;

            for (auto& o : parked)
                taken = taken || DistSq(o, s.pos) < 10.0f * 10.0f;

            if (!taken)
                out.push_back(s);
        }

        std::sort(out.begin(), out.end(),
                  [&](const Spot& x, const Spot& y) { return DistSq(x.pos, at) < DistSq(y.pos, at); });

        return out;
    }

    // --------------------------------------------------------------- levels --

    void Schedule(rmm_player_t, PlayerState& st, double delay) {
        if (!Active()) {
            st.pending = false;

            return;
        }

        st.pending = true;
        st.applyAt = Now() + delay;
        st.attempts = 0;
    }

    void UpdateScore(rmm_player_t p) {
        if (!levelScore_.Bool() || !rmm::HasCap(RMM_CAP_SET_SCORE) || ladder_.empty())
            return;

        auto info = rmm::GetPlayer(p);
        int want = State(p).level + 1;

        if (info && info->score != want)
            rmm::Api().players->set_score(p, want);
    }

    void SetLevel(rmm_player_t p, int level) {
        PlayerState& st = State(p);
        st.level = std::max(0, std::min(level, Count() - 1));
        st.kills = 0;
        UpdateScore(p);
        auto info = rmm::GetPlayer(p);

        if (info && info->is_alive)
            Schedule(p, st, 0.0);
        else
            st.pending = false;  // handed out at the next spawn
    }

    void AddKill(rmm_player_t p) {
        PlayerState& st = State(p);

        if (++st.kills < KillsNeeded(st.level)) {
            Tell(p, rmm::Format("%d/%d kills for the next level", st.kills, KillsNeeded(st.level)));

            return;
        }

        if (st.level + 1 >= Count()) {
            Win(p);

            return;
        }

        SetLevel(p, st.level + 1);
        Tell(p, LevelText(st.level));

        if (st.level + 1 == Count())
            rmm::SayAll(kPrefix + rmm::PlayerName(p) + " reached the last level!");
    }

    /// Does this kill count towards the attacker's level? (gungame.strictWeapons)
    /// Vehicle levels: only kills made from their own gun game vehicle (and, with
    /// weapons=, with those weapons). Kit levels with weapons=: only those weapons.
    /// A kill the game reports no weapon for counts: there is nothing to check.
    bool CountsForLevel(const rmm_ev_player_killed& e, bool knife, std::string& why) {
        if (!strictWeapons_.Bool())
            return true;

        if (knife && knifeCounts_.Bool())
            return true;

        PlayerState& st = State(e.attacker);
        const Level& L = ladder_[static_cast<size_t>(st.level)];
        const char* weapon = e.weapon ? e.weapon : "";

        if (L.type == LevelType::Vehicle) {
            bool fromOwn = st.vehicle && (st.inVehicle || (e.vehicle && Lower(e.vehicle) == Lower(L.tmpl)));

            if (!fromOwn) {
                why = "Only kills from your " + L.name + " count.";

                return false;
            }
        }

        if (L.weapons.empty() || !*weapon || GlobList(L.weapons, weapon))
            return true;

        rmm::Debug("kill by %s with %s doesn't match level weapons %s", rmm::PlayerName(e.attacker).c_str(), weapon,
                   L.weapons.c_str());
        why = L.type == LevelType::Vehicle ? "Only kills with your " + L.name + "'s main weapon count."
                                           : "Only kills with the " + L.name + " count.";

        return false;
    }

    void Demote(rmm_player_t p, int levels, const std::string& why) {
        PlayerState& st = State(p);
        int old = st.level;
        SetLevel(p, st.level - levels);

        if (st.level != old)
            Tell(p, why + " - down to " + LevelText(st.level));
    }

    void Win(rmm_player_t p) {
        auto info = rmm::GetPlayer(p);
        std::string name = rmm::PlayerName(p);
        rmm::SayAll(kPrefix + name + " wins Gun Game!", rmm::PaceImmediate());
        rmm::Info("%s won (team %d)", name.c_str(), info ? info->team : 0);

        if (endRound_.Bool() && rmm::HasCap(RMM_CAP_END_ROUND)) {
            int team = info && (info->team == 1 || info->team == 2) ? info->team : RMM_TEAM_NONE;

            if (rmm::EndRound(team)) {
                roundOver_ = true;

                return;
            }

            rmm::Warn("end_round refused; starting the ladder over instead");
        }

        rmm::SayAll(kPrefix + "New game: everyone back to level 1.");
        Restart(false);
    }

    // ------------------------------------------------------------- vehicles --

    /// Let go of a player's vehicle: it is removed once empty, or blown up.
    void Release(PlayerState& st, bool explode, rmm_player_t attacker) {
        if (!st.vehicle)
            return;

        owners_.erase(st.vehicle);
        cleanup_.push_back(Cleanup{st.vehicle, explode, attacker});
        st.vehicle = 0;
        st.inVehicle = false;
    }

    void RemoveIfEmpty(rmm_object_t v) {
        auto i = Info(v);

        if (i && i->occupant_count == 0)
            rmm::RemoveObject(v);
    }

    void RunCleanup() {
        if (cleanup_.empty())
            return;

        auto batch = std::move(cleanup_);
        cleanup_.clear();

        for (auto& c : batch) {
            auto i = Info(c.vehicle);

            if (!i)
                continue;  // gone already

            if (c.explode && !i->is_destroyed) {
                // Beyond its health either way; the engine does the explosion and the wreck.
                float amount = (i->health > 0 ? i->health : 0.0f) + 100000.0f;

                if (rmm::DamageObject(c.vehicle, amount, c.attacker))
                    continue;

                rmm::RemoveObject(c.vehicle);  // no object damage on this game: make it vanish
                continue;
            }

            if (!i->is_destroyed && i->occupant_count == 0)
                rmm::RemoveObject(c.vehicle);

            // occupied (someone else got in): leave it be, it's a normal vehicle now
        }
    }

    /// Spawn @p L's vehicle for a player at @p at: at the nearest free map vehicle
    /// spawn point (clear of buildings by design), else at one of 8 spots around
    /// the player, the one farthest from other objects first (vehicles die if
    /// spawned inside something, and those spots can't see building walls).
    rmm_object_t SpawnNear(const rmm_vec3& at, const Level& L) {
        for (const Spot& s : SpotsNear(at)) {
            rmm_vec3 pos = s.pos;
            pos.y += std::max(0.0f, L.height - 1.0f);

            if (rmm_object_t v = rmm::SpawnObject(L.tmpl, pos, rmm_vec3{s.yaw, 0, 0}, 0))
                return v;
        }

        const float d = static_cast<float>(spawnDistance_.Float());
        std::vector<rmm_vec3> around;

        if (rmm::HasCap(RMM_CAP_OBJECT_QUERY))
            for (rmm_object_t o : rmm::FindObjectsNear(at, d + 8.0f, "", RMM_FIND_NO_SOLDIERS))
                if (auto i = Info(o))
                    around.push_back(i->position);

        struct Spot {
            rmm_vec3 pos;
            float yaw;
            float clearance;
        };

        std::vector<Spot> spots;

        for (int k = 0; k < 8; ++k) {
            float a = static_cast<float>(k) * 0.785398f;  // 45 degrees
            rmm_vec3 p{at.x + std::cos(a) * d, at.y, at.z + std::sin(a) * d};
            float best = 1e9f;

            for (auto& o : around) {
                float dx = o.x - p.x, dz = o.z - p.z;
                best = std::min(best, dx * dx + dz * dz);
            }

            spots.push_back(Spot{p, a * 57.29578f, best});
        }

        std::stable_sort(spots.begin(), spots.end(),
                         [](const Spot& x, const Spot& y) { return x.clearance > y.clearance; });
        const bool ground = rmm::HasCap(RMM_CAP_GROUND_HEIGHT);

        for (auto& s : spots) {
            rmm_vec3 pos = s.pos;
            pos.y = ground ? L.height : at.y + L.height;
            rmm_object_t v = rmm::SpawnObject(L.tmpl, pos, rmm_vec3{s.yaw, 0, 0}, ground ? RMM_SPAWN_ON_GROUND : 0);

            if (v)
                return v;
        }

        return 0;
    }

    /// One step towards handing out the player's level; true once it's done.
    /// Runs from the frame event, never from inside an engine callback.
    bool Step(rmm_player_t p, PlayerState& st) {
        auto info = rmm::GetPlayer(p);

        if (!info || !info->is_alive)
            return true;  // gone, or dead: the next spawn re-arms it

        const Level& L = ladder_[static_cast<size_t>(st.level)];
        rmm_object_t sittingIn = info->vehicle ? RootOf(info->vehicle) : 0;

        if (L.type == LevelType::Kit) {
            if (sittingIn) {
                rmm::ExitVehicle(p);

                return false;
            }

            Release(st, false, RMM_PLAYER_NONE);

            if (!rmm::SetKit(p, L.tmpl))
                return false;

            st.holdAt = Now() + kHoldDelay;

            return true;
        }

        if (st.vehicle && Usable(st.vehicle) && Lower(Info(st.vehicle)->root_template) == Lower(L.tmpl)) {
            if (sittingIn == st.vehicle)
                return true;  // seated: done

            if (sittingIn) {
                rmm::ExitVehicle(p);

                return false;
            }

            if (!rmm::EnterVehicle(p, st.vehicle, L.seat))
                rmm::EnterVehicle(p, st.vehicle, -1);

            return false;  // confirmed next step
        }

        if (sittingIn) {
            rmm::ExitVehicle(p);

            return false;
        }

        Release(st, false, RMM_PLAYER_NONE);

        if (!L.kit.empty())
            rmm::SetKit(p, L.kit);

        rmm_object_t v = SpawnNear(info->position, L);

        if (!v)
            return false;

        st.vehicle = v;
        owners_[v] = p;
        rmm::EnterVehicle(p, v, L.seat);
        st.applyAt = Now() + 0.2;  // give the new vehicle a moment before checking

        return false;
    }

    void ProcessPending() {
        double now = Now();

        for (auto& kv : players_) {
            PlayerState& st = kv.second;

            if (st.holdAt >= 0 && now >= st.holdAt) {
                st.holdAt = -1;
                auto info = rmm::GetPlayer(kv.first);

                if (info && info->is_alive && !info->vehicle)
                    HoldLevelWeapon(kv.first);
            }

            if (!st.pending || now < st.applyAt)
                continue;

            double before = st.applyAt;

            if (Step(kv.first, st)) {
                st.pending = false;
                continue;
            }

            if (st.applyAt == before)
                st.applyAt = now + 0.1;

            if (++st.attempts >= 40) {
                st.pending = false;
                rmm::Warn("could not hand %s to %s", ladder_[static_cast<size_t>(st.level)].tmpl.c_str(),
                          rmm::PlayerName(kv.first).c_str());
                Tell(kv.first,
                     "Could not give you " + ladder_[static_cast<size_t>(st.level)].name + " (gg stuck to retry)");
            }
        }
    }

    void Housekeeping() {
        ApplySpawnTiming();  // a level load puts the server's own timing back

        if (!Active())
            return;

        for (auto& kv : players_)
            UpdateScore(kv.first);

        if (holdTickets_.Bool() && rmm::HasCap(RMM_CAP_TICKETS) && rmm::Api().game->status() == RMM_STATUS_PLAYING)
            for (int t = 1; t <= 2; ++t)
                if (startTickets_[t] > 0 && rmm::Api().game->get_tickets(t) < startTickets_[t])
                    rmm::Api().game->set_tickets(t, startTickets_[t]);
    }

    bool ExitAllowed(const PlayerState& st) const {
        int lvl = ladder_.empty() ? -1 : st.level;

        if (lvl >= 0 && ladder_[static_cast<size_t>(lvl)].exit >= 0)
            return ladder_[static_cast<size_t>(lvl)].exit != 0;

        return vehicleExit_.Bool();
    }

    // --------------------------------------------------------------- events --

    void HookEvents() {
        rmm::On<rmm_ev_frame>(RMM_EV_FRAME, [this](rmm_ev_frame&) {
            RunCleanup();

            if (sweepMapVehicles_)
                SweepMapVehicles();

            if (Active())
                ProcessPending();

            return RMM_CONTINUE;
        });

        rmm::On<rmm_ev_map>(RMM_EV_MAP_LOAD, [this](rmm_ev_map& e) {
            map_ = e.map ? e.map : "";
            spots_.clear();

            return RMM_CONTINUE;
        });

        // Remember where the map puts its vehicles; keep them from spawning.
        rmm::On<rmm_ev_spawner>(
            RMM_EV_SPAWNER_SPAWN,
            [this](rmm_ev_spawner& e) -> rmm_result {
                spots_[e.spawner] = Spot{e.object_template ? e.object_template : "", e.position, e.rotation.x};

                if (!BlockMapVehicles())
                    return RMM_CONTINUE;

                rmm::Debug("blocked map spawner: %s at %.0f,%.0f,%.0f", e.object_template, e.position.x, e.position.y,
                           e.position.z);

                return RMM_HANDLED;
            },
            RMM_PHASE_PRE);
        rmm::On<rmm_ev_map>(RMM_EV_ROUND_START, [this](rmm_ev_map& e) {
            if (e.map && *e.map)
                map_ = e.map;

            startTickets_[1] = rmm::Api().game->get_tickets(1);
            startTickets_[2] = rmm::Api().game->get_tickets(2);
            Restart(true);

            if (Active())
                rmm::SayAll(kPrefix + rmm::Format("Gun Game: %d levels. Type !gg for yours.", Count()));

            return RMM_CONTINUE;
        });
        rmm::On<rmm_ev_round_end>(RMM_EV_ROUND_END, [this](rmm_ev_round_end&) {
            roundOver_ = true;

            for (auto& kv : players_)
                kv.second.pending = false;

            return RMM_CONTINUE;
        });

        rmm::On<rmm_ev_player>(RMM_EV_PLAYER_DISCONNECT, [this](rmm_ev_player& e) {
            auto it = players_.find(e.player);

            if (it == players_.end())
                return RMM_CONTINUE;

            Release(it->second, false, RMM_PLAYER_NONE);
            players_.erase(it);

            return RMM_CONTINUE;
        });

        rmm::On<rmm_ev_player_spawn>(RMM_EV_PLAYER_SPAWN, [this](rmm_ev_player_spawn& e) {
            if (!Active())
                return RMM_CONTINUE;

            PlayerState& st = State(e.player);
            Release(st, false, RMM_PLAYER_NONE);  // a leftover from the last life
            Schedule(e.player, st, 0.0);

            // Kits right away (the documented spawn pattern); vehicles from the frame event.
            if (ladder_[static_cast<size_t>(st.level)].type == LevelType::Kit &&
                rmm::SetKit(e.player, ladder_[static_cast<size_t>(st.level)].tmpl)) {
                st.pending = false;
                st.holdAt = Now() + kHoldDelay;
            }

            UpdateScore(e.player);

            return RMM_CONTINUE;
        });

        rmm::On<rmm_ev_player_killed>(RMM_EV_PLAYER_KILLED, [this](rmm_ev_player_killed& e) {
            if (!Active())
                return RMM_CONTINUE;

            PlayerState& vs = State(e.victim);
            vs.pending = false;
            // Killed in their own vehicle (or taken out of it by the death a moment ago)?
            bool inside = vs.vehicle && (vs.inVehicle || Now() - vs.lastGameExit < 1.0);
            Release(vs, inside && explode_.Bool(), e.attacker);

            bool suicide = e.suicide || e.attacker == e.victim;
            bool kill = e.attacker != RMM_PLAYER_NONE && !suicide && (!e.teamkill || teamKills_.Bool());

            if (suicide && suicideDemote_.Int() > 0)
                Demote(e.victim, suicideDemote_.Int(), "Suicide");

            bool knife = kill && GlobList(knifeWeapons_.Str(), e.weapon);

            if (knife && knifeDemote_.Int() > 0) {
                std::string killer = rmm::PlayerName(e.attacker);
                Demote(e.victim, knifeDemote_.Int(), "Knifed by " + killer);
                rmm::SayAll(kPrefix + killer + " knifed " + rmm::PlayerName(e.victim) + "!");
            }

            if (!kill)
                return RMM_CONTINUE;

            std::string why;

            if (CountsForLevel(e, knife, why))
                AddKill(e.attacker);
            else
                Tell(e.attacker, why);

            return RMM_CONTINUE;
        });

        // Vehicles: who may get in where.
        rmm::On<rmm_ev_vehicle>(
            RMM_EV_VEHICLE_ENTER,
            [this](rmm_ev_vehicle& e) -> rmm_result {
                if (!Active() || e.reason != RMM_VEHICLE_REASON_PLAYER)
                    return RMM_CONTINUE;

                auto own = owners_.find(e.root);

                if (own != owners_.end() && own->second == e.player) {
                    if (vehicleReenter_.Bool())
                        return RMM_CONTINUE;

                    Notice(e.player, "You can't get back in.");

                    return RMM_HANDLED;
                }

                if (otherVehicles_.Bool())
                    return RMM_CONTINUE;

                Notice(e.player, own != owners_.end() ? "That's someone else's gun game vehicle."
                                                      : "Gun Game: no other vehicles.");

                return RMM_HANDLED;
            },
            RMM_PHASE_PRE);
        rmm::On<rmm_ev_vehicle>(RMM_EV_VEHICLE_ENTER, [this](rmm_ev_vehicle& e) {
            auto own = owners_.find(e.root);

            if (own != owners_.end() && own->second == e.player)
                State(e.player).inVehicle = true;

            return RMM_CONTINUE;
        });

        // ...and who may get out.
        rmm::On<rmm_ev_vehicle>(
            RMM_EV_VEHICLE_EXIT,
            [this](rmm_ev_vehicle& e) -> rmm_result {
                if (!Active())
                    return RMM_CONTINUE;

                auto own = owners_.find(e.root);

                if (own == owners_.end() || own->second != e.player)
                    return RMM_CONTINUE;

                PlayerState& st = State(e.player);

                if (e.reason == RMM_VEHICLE_REASON_PLAYER && !ExitAllowed(st)) {
                    Notice(e.player, "You can't leave your vehicle in Gun Game.");

                    return RMM_HANDLED;
                }

                if (e.reason == RMM_VEHICLE_REASON_SWITCH && !vehicleSwitch_.Bool())
                    return RMM_HANDLED;

                return RMM_CONTINUE;
            },
            RMM_PHASE_PRE);
        rmm::On<rmm_ev_vehicle>(RMM_EV_VEHICLE_EXIT, [this](rmm_ev_vehicle& e) {
            auto own = owners_.find(e.root);

            if (own == owners_.end() || own->second != e.player || e.reason == RMM_VEHICLE_REASON_SWITCH)
                return RMM_CONTINUE;

            PlayerState& st = State(e.player);
            st.inVehicle = false;

            if (e.reason == RMM_VEHICLE_REASON_GAME)
                st.lastGameExit = Now();

            return RMM_CONTINUE;
        });

        rmm::On<rmm_ev_vehicle>(RMM_EV_VEHICLE_DESTROYED, [this](rmm_ev_vehicle& e) {
            rmm_object_t root = e.root ? e.root : e.vehicle;
            auto own = owners_.find(root);

            if (own == owners_.end())
                return RMM_CONTINUE;

            rmm_player_t owner = own->second;
            owners_.erase(own);
            PlayerState& st = State(owner);
            st.vehicle = 0;
            st.inVehicle = false;
            // An owner who wasn't inside gets a new one after a while (if they're
            // still on a vehicle level then and alive; a dead one gets it at spawn).
            double delay = respawnDelay_.Float();

            if (Active() && delay >= 0) {
                auto info = rmm::GetPlayer(owner);

                if (info && info->is_alive && ladder_[static_cast<size_t>(st.level)].type == LevelType::Vehicle) {
                    Tell(owner, rmm::Format("Your vehicle was destroyed; a new one in %.0f s.", delay));
                    Schedule(owner, st, delay);
                }
            }

            return RMM_CONTINUE;
        });

        rmm::On<rmm_ev_player_kit>(
            RMM_EV_PLAYER_KIT_PICKUP,
            [this](rmm_ev_player_kit& e) -> rmm_result {
                if (!Active() || pickups_.Bool() || e.reason != RMM_KIT_REASON_PICKUP)
                    return RMM_CONTINUE;

                Notice(e.player, "No picking up kits in Gun Game.");

                return RMM_HANDLED;
            },
            RMM_PHASE_PRE);

        // Keep the level's weapon in hand (gungame.lockWeapons).
        rmm::On<rmm_ev_weapon_switch>(
            RMM_EV_PLAYER_WEAPON_SWITCH,
            [this](rmm_ev_weapon_switch& e) -> rmm_result {
                if (e.reason != RMM_WEAPON_REASON_PLAYER && e.reason != RMM_WEAPON_REASON_NETWORK)
                    return RMM_CONTINUE;

                const Level* L = LockedLevel(e.player);

                if (!L || WeaponAllowed(*L, e.to_weapon))
                    return RMM_CONTINUE;

                Notice(e.player, "This level: the " + L->name + " only.");

                return RMM_HANDLED;
            },
            RMM_PHASE_PRE);
    }

    /// The player's level if its weapons are locked (kit level with weapons=), else null.
    const Level* LockedLevel(rmm_player_t p) {
        if (!Active() || !lockWeapons_.Bool())
            return nullptr;

        auto it = players_.find(p);

        if (it == players_.end() || it->second.level < 0 || it->second.level >= static_cast<int>(ladder_.size()))
            return nullptr;

        const Level& L = ladder_[static_cast<size_t>(it->second.level)];

        return L.type == LevelType::Kit && !L.weapons.empty() ? &L : nullptr;
    }

    bool WeaponAllowed(const Level& L, const char* weapon) {
        return GlobList(L.weapons, weapon) || (knifeCounts_.Bool() && GlobList(knifeWeapons_.Str(), weapon));
    }

    /// After a kit is handed out: hold the level's weapon (the game picks the
    /// "best" one, which on a pistol or knife level is the wrong one).
    void HoldLevelWeapon(rmm_player_t p) {
        const Level* L = LockedLevel(p);

        if (!L)
            return;

        auto items = rmm::Items(p);

        for (auto& i : items)
            if (i.active && GlobList(L->weapons, i.weapon))
                return;

        for (auto& i : items)
            if (GlobList(L->weapons, i.weapon) && rmm::SelectItem(p, i.slot))
                return;
    }

    // ------------------------------------------------------------- commands --

    void RegisterCommands() {
        rmm::Command(
            "gg", RMM_CMD_ALL, 0, "Gun game: your level, the ladder, the leaders",
            "[ladder | top | stuck | set <target> <level> | restart | reload]", [this](rmm::CmdArgs& a) {
                std::string sub = Lower(a.Arg(1));

                if (sub == "set" || sub == "restart" || sub == "reload") {
                    if (!rmm::HasFlags(a.Caller(), RMM_ADMFLAG_CONFIG)) {
                        a.Reply("You do not have access to this command.");

                        return;
                    }

                    if (sub == "reload") {
                        Reload();
                        a.Replyf("Ladder reloaded: %d level(s).", Count());

                        return;
                    }

                    if (sub == "restart") {
                        Restart(true);
                        rmm::SayAll(kPrefix + "Restarted: everyone back to level 1.");

                        return;
                    }

                    if (a.Count() < 3) {
                        a.Reply("Usage: gg set <target> <level>");

                        return;
                    }

                    if (ladder_.empty()) {
                        a.Reply("No ladder loaded.");

                        return;
                    }

                    int level = std::atoi(a.Arg(3).c_str());

                    if (level < 1 || level > Count()) {
                        a.Replyf("Level must be 1-%d.", Count());

                        return;
                    }

                    for (rmm_player_t p : rmm::Target(a, a.Arg(2), RMM_TARGET_ALLOW_MULTI)) {
                        SetLevel(p, level - 1);
                        Tell(p, LevelText(level - 1));
                        a.Replyf("%s is on level %d.", rmm::PlayerName(p).c_str(), level);
                    }

                    return;
                }

                if (ladder_.empty()) {
                    a.Reply("Gun game has no ladder for this map.");

                    return;
                }

                if (sub == "ladder") {
                    a.Pace(rmm::PaceEvery(1.5f));

                    for (int i = 0; i < Count(); ++i) {
                        const Level& L = ladder_[static_cast<size_t>(i)];
                        bool strict = strictWeapons_.Bool() && !L.weapons.empty();
                        a.Replyf("%d. %s%s (%d kill%s%s%s)", i + 1, L.name.c_str(),
                                 L.type == LevelType::Vehicle ? " [vehicle]" : "", KillsNeeded(i),
                                 KillsNeeded(i) == 1 ? "" : "s", strict ? ", " : "", strict ? L.weapons.c_str() : "");
                    }

                    return;
                }

                if (sub == "top") {
                    std::vector<std::pair<rmm_player_t, PlayerState*>> rows;

                    for (auto& kv : players_)
                        rows.push_back({kv.first, &kv.second});

                    std::stable_sort(rows.begin(), rows.end(), [](auto& x, auto& y) {
                        return x.second->level != y.second->level ? x.second->level > y.second->level
                                                                  : x.second->kills > y.second->kills;
                    });

                    if (rows.empty()) {
                        a.Reply("Nobody is playing.");

                        return;
                    }

                    for (size_t i = 0; i < rows.size() && i < 5; ++i)
                        a.Replyf("%zu. %s - level %d/%d", i + 1, rmm::PlayerName(rows[i].first).c_str(),
                                 rows[i].second->level + 1, Count());

                    return;
                }

                if (sub == "stuck") {
                    rmm_player_t p = a.Caller();

                    if (p == RMM_PLAYER_NONE) {
                        a.Reply("Players only.");

                        return;
                    }

                    PlayerState& st = State(p);

                    if (!Active() || ladder_[static_cast<size_t>(st.level)].type != LevelType::Vehicle) {
                        a.Reply("Only on a vehicle level.");

                        return;
                    }

                    double wait = stuckCooldown_.Float() - (Now() - st.lastStuck);

                    if (wait > 0) {
                        a.Replyf("Wait %.0f s.", wait);

                        return;
                    }

                    auto info = rmm::GetPlayer(p);

                    if (!info || !info->is_alive) {
                        a.Reply("You are dead.");

                        return;
                    }

                    st.lastStuck = Now();

                    if (info->vehicle)
                        rmm::ExitVehicle(p);

                    Release(st, false, RMM_PLAYER_NONE);
                    Schedule(p, st, 0.3);
                    a.Reply("A new vehicle is on its way.");

                    return;
                }

                if (a.Caller() == RMM_PLAYER_NONE) {  // console: everyone
                    for (auto& kv : players_)
                        a.Replyf("#%d %s: level %d/%d (%s), %d kill(s)%s", kv.first, rmm::PlayerName(kv.first).c_str(),
                                 kv.second.level + 1, Count(),
                                 ladder_[static_cast<size_t>(kv.second.level)].name.c_str(), kv.second.kills,
                                 kv.second.vehicle ? rmm::Format(", vehicle 0x%llx",
                                                                 static_cast<unsigned long long>(kv.second.vehicle))
                                                         .c_str()
                                                   : "");

                    if (players_.empty())
                        a.Reply("Nobody is playing.");

                    return;
                }

                PlayerState& st = State(a.Caller());
                a.Replyf("%s, %d/%d kills", LevelText(st.level).c_str(), st.kills, KillsNeeded(st.level));
            });
    }

    // ----------------------------------------------------------------- data --

    rmm::Var enabled_, ladderName_, killsPerLevel_, knifeWeapons_, knifeDemote_, knifeCounts_, strictWeapons_,
        lockWeapons_;
    rmm::Var suicideDemote_, teamKills_;
    rmm::Var vehicleExit_, vehicleSwitch_, vehicleReenter_, otherVehicles_, explode_, respawnDelay_, spawnDistance_;
    rmm::Var pickups_, endRound_, levelScore_, holdTickets_, stuckCooldown_;
    rmm::Var mapVehicles_, respawnTime_, spotRange_;
    std::map<rmm_object_t, Spot> spots_;           // map spawner -> where it puts its object
    std::optional<rmm_spawn_timing> savedTiming_;  // the server's, while gungame.respawnTime applies
    bool sweepMapVehicles_ = false;

    std::vector<Level> ladder_;
    std::map<rmm_player_t, PlayerState> players_;
    std::map<rmm_object_t, rmm_player_t> owners_;  // gun game vehicle -> owner
    std::vector<Cleanup> cleanup_;                 // vehicles to remove/explode at the next frame
    std::string map_;
    bool roundOver_ = false;
    int32_t startTickets_[3] = {0, 0, 0};
};

RMM_DECLARE_PLUGIN(GunGame, "Gun Game", "1.0.0", "RMM Team", "Climb a ladder of kits and vehicles, one kill at a time",
                   "", "gungame", RMM_GAME_ANY, 0)
