#include "sim_adapter.h"

#include "../../core/core.h"

#include <cstdio>
#include <cstdlib>
#include <iterator>

namespace rmm {

static const char* EmulatedName(rmm_game g) {
    switch (g) {
        case RMM_GAME_BF1942:
            return "Simulator (as Battlefield 1942)";
        case RMM_GAME_BFV:
            return "Simulator (as Battlefield Vietnam)";
        case RMM_GAME_BF2142:
            return "Simulator (as Battlefield 2142)";
        default:
            return "Simulator (as Battlefield 2)";
    }
}

SimAdapter::SimAdapter(rmm_game emulate) : emulate_(emulate), name_(EmulatedName(emulate)) {
    shortName_ = "sim";
    mod_ = "sim";
}

bool SimAdapter::Attach(Core& core, std::string& /*err*/) {
    core_ = &core;
    std::string gdErr;

    if (!LoadGameData(core.BuildPath(RMM_PATH_GAMEDATA, ""), gdErr)) {
        core.Log().Warn("%s - using built-in simulator gamedata", gdErr.c_str());
        static const char* cmds[][2] = {
            {"kick", "sim.kick {id} {reason}"},
            {"ban", "sim.ban {id} {minutes} {reason}"},
            {"kill", "sim.kill {id}"},
            {"set_team", "sim.setTeam {id} {team}"},
            {"pm", "sim.pm {id} {msg}"},
            {"set_score", "sim.setScore {id} {score}"},
            {"teleport", "sim.teleport {id} {x} {y} {z}"},
            {"change_map", "sim.changeMap {map} {mode} {size}"},
            {"restart_map", "sim.restartMap"},
            {"run_next_map", "sim.runNextMap"},
            {"maplist_list", "sim.maplist"},
            {"maplist_append", "sim.maplistAppend {map} {mode} {size}"},
            {"maplist_clear", "sim.maplistClear"},
            {"set_next_map", "sim.setNextMap {index}"},
            {"say_all", "sim.say {msg}"},
            {"say_team", "sim.sayTeam {team} {msg}"},
            {"center", "sim.center {msg}"},
            {"get_tickets", "sim.tickets {team}"},
            {"set_tickets", "sim.setTickets {team} {tickets}"},
        };

        for (auto& c : cmds)
            data_.Set("commands", c[0], c[1]);

        for (int i = 0; i < RMM_SETTING_COUNT; ++i)
            data_.Set("settings", SettingKey(i), std::string("sim.") + SettingKey(i));
    }

    // Seed engine variables for every mapped setting.
    for (int i = 0; i < RMM_SETTING_COUNT; ++i) {
        std::string v = SettingVar(i);

        if (!v.empty())
            vars_[ToLower(v)] = data_.Get("sim_defaults", SettingKey(i), "0");
    }

    maplist_ = {
        {sizeof(rmm_map_info), "strike_at_karkand", "gpm_cq", 32},
        {sizeof(rmm_map_info), "gulf_of_oman", "gpm_cq", 64},
        {sizeof(rmm_map_info), "sharqi_peninsula", "gpm_cq", 32},
    };

    // A real adapter would call OnGameReady() once the engine finished
    // booting; the simulator is ready immediately.
    core.OnGameReady();
    LoadMap("strike_at_karkand", "gpm_cq", 32);

    return true;
}

uint32_t SimAdapter::EventFlags(int32_t ev) const {
    switch (ev) {
        case RMM_EV_PLAYER_CONNECT:
            return RMM_EVF_SUPPORTED | RMM_EVF_BLOCKABLE;
        case RMM_EV_CHAT:
            return RMM_EVF_SUPPORTED | RMM_EVF_BLOCKABLE | RMM_EVF_MUTABLE;
        case RMM_EV_PLAYER_DAMAGE:
            return RMM_EVF_SUPPORTED | RMM_EVF_BLOCKABLE | RMM_EVF_MUTABLE;
        case RMM_EV_VEHICLE_DAMAGE:
            return RMM_EVF_SUPPORTED | RMM_EVF_BLOCKABLE | RMM_EVF_MUTABLE;
        case RMM_EV_RCON_COMMAND:
            return RMM_EVF_SUPPORTED | RMM_EVF_BLOCKABLE;
        case RMM_EV_VEHICLE_ENTER:
            return RMM_EVF_SUPPORTED | RMM_EVF_BLOCKABLE;  // like BF1942
        case RMM_EV_VEHICLE_EXIT:
            return RMM_EVF_SUPPORTED | RMM_EVF_BLOCKABLE;
        case RMM_EV_PLAYER_KIT_PICKUP:
            return RMM_EVF_SUPPORTED | RMM_EVF_BLOCKABLE;
        case RMM_EV_PLAYER_WEAPON_SWITCH:
            return RMM_EVF_SUPPORTED | RMM_EVF_BLOCKABLE;
        case RMM_EV_SPAWNER_SPAWN:
            return RMM_EVF_SUPPORTED | RMM_EVF_BLOCKABLE;
        default:
            return RMM_EVF_SUPPORTED;
    }
}

int SimAdapter::CapOverride(int32_t cap) const {
    switch (cap) {
        case RMM_CAP_CONSOLE_EXEC:
        case RMM_CAP_CONSOLE_OUTPUT:
        case RMM_CAP_CONSOLE_HOOK:
        case RMM_CAP_PLAYER_POSITION:
        case RMM_CAP_AUTH_ID:
        case RMM_CAP_DAMAGE_PLAYER:
        case RMM_CAP_PLAYER_HEALTH:
        case RMM_CAP_WORLD_OBJECTS:
        case RMM_CAP_OBJECT_MOVE:
        case RMM_CAP_OBJECT_ROTATION:
        case RMM_CAP_OBJECT_VELOCITY:
        case RMM_CAP_PLAYER_PUSH:
        case RMM_CAP_OBJECT_QUERY:
        case RMM_CAP_OBJECT_SPAWN:
        case RMM_CAP_OBJECT_REMOVE:
        case RMM_CAP_GROUND_HEIGHT:
        case RMM_CAP_BOT_INPUT:
        case RMM_CAP_SET_KIT:
        case RMM_CAP_END_ROUND:
        case RMM_CAP_PUT_IN_VEHICLE:
        case RMM_CAP_OBJECT_DAMAGE:
        case RMM_CAP_ITEMS:
        case RMM_CAP_SPAWN_TIMING:
        case RMM_CAP_CONTROL_POINTS:
        case RMM_CAP_CONTROL_POINT_SET:
        case RMM_CAP_CONTROL_POINT_LOCK:
        case RMM_CAP_QUERY_RULES:
            return 1;
        default:
            return -1;
    }
}

void SimAdapter::ConsolePrint(const std::string& line) {
    std::printf("%s\n", line.c_str());
    std::fflush(stdout);
}

std::vector<rmm_player_t> SimAdapter::PlayerIds() {
    std::vector<rmm_player_t> v;

    for (auto& kv : players_)
        v.push_back(kv.first);

    return v;
}

bool SimAdapter::GetPlayer(rmm_player_t id, rmm_player_info& out) {
    auto it = players_.find(id);

    if (it == players_.end())
        return false;

    out = it->second;
    // Sim soldiers face +Z while alive.
    out.forward = out.is_alive ? rmm_vec3{0, 0, 1} : rmm_vec3{0, 0, 0};

    return true;
}

// --------------------------------------------------------------------------------
// Fake engine console
// --------------------------------------------------------------------------------

bool SimAdapter::Exec(const std::string& command, std::string* output) {
    auto t = SplitArgs(command);

    if (t.empty())
        return false;

    std::string cmd = ToLower(t[0]);
    auto arg = [&](size_t i) { return i < t.size() ? t[i] : std::string(); };
    auto argi = [&](size_t i) { return std::atoi(arg(i).c_str()); };
    auto out = [&](const std::string& s) {
        if (output) {
            if (!output->empty())
                *output += "\n";

            *output += s;
        }
    };

    auto say = [&](const std::string& s) {
        std::printf("[game] %s\n", s.c_str());
        std::fflush(stdout);
    };

    if (cmd == "sim.kick") {
        Remove(argi(1), "kicked: " + arg(2));

        return true;
    }

    if (cmd == "sim.ban") {
        Remove(argi(1), "engine-banned: " + arg(3));

        return true;
    }

    if (cmd == "sim.kill") {
        auto it = players_.find(argi(1));

        if (it == players_.end())
            return false;

        Kill(argi(1), RMM_PLAYER_NONE, "admin_slay");

        return true;
    }

    if (cmd == "sim.setteam") {
        ChangeTeamAction(argi(1), argi(2));

        return true;
    }

    if (cmd == "sim.pm") {
        say("(to #" + arg(1) + ") " + arg(2));

        return true;
    }

    if (cmd == "sim.say") {
        say("SERVER: " + arg(1));

        return true;
    }

    if (cmd == "sim.sayteam") {
        say("SERVER (team " + arg(1) + "): " + arg(2));

        return true;
    }

    if (cmd == "sim.center") {
        say("[center] " + arg(1));

        return true;
    }

    if (cmd == "sim.setscore") {
        if (players_.count(argi(1)))
            players_[argi(1)].score = argi(2);

        return true;
    }

    if (cmd == "sim.teleport") {
        if (!players_.count(argi(1)))
            return false;

        players_[argi(1)].position = rmm_vec3{(float)std::atof(arg(2).c_str()), (float)std::atof(arg(3).c_str()),
                                              (float)std::atof(arg(4).c_str())};

        return true;
    }

    if (cmd == "sim.changemap") {
        LoadMap(arg(1), arg(2).empty() ? "gpm_cq" : arg(2), argi(3));

        return true;
    }

    if (cmd == "sim.restartmap") {
        LoadMap(currentMap_.map, currentMap_.mode, currentMap_.size);

        return true;
    }

    if (cmd == "sim.runnextmap") {
        if (maplist_.empty())
            return false;

        auto& m = maplist_[static_cast<size_t>(nextMapIndex_) % maplist_.size()];
        nextMapIndex_ = (nextMapIndex_ + 1) % static_cast<int32_t>(maplist_.size());
        LoadMap(m.map, m.mode, m.size);

        return true;
    }

    if (cmd == "sim.maplist") {
        for (size_t i = 0; i < maplist_.size(); ++i)
            out(Format("%zu: \"%s\" %s %d", i, maplist_[i].map, maplist_[i].mode, maplist_[i].size));

        return true;
    }

    if (cmd == "sim.maplistappend") {
        rmm_map_info m{};
        m.struct_size = sizeof m;
        CopyOut(arg(1), m.map, sizeof m.map);
        CopyOut(arg(2), m.mode, sizeof m.mode);
        m.size = argi(3);
        maplist_.push_back(m);

        return true;
    }

    if (cmd == "sim.maplistclear") {
        maplist_.clear();

        return true;
    }

    if (cmd == "sim.setnextmap") {
        nextMapIndex_ = argi(1);

        return true;
    }

    if (cmd == "sim.tickets") {
        int tm = argi(1);
        out(std::to_string(tm >= 1 && tm <= 2 ? tickets_[tm] : 0));

        return true;
    }

    if (cmd == "sim.settickets") {
        int tm = argi(1);

        if (tm >= 1 && tm <= 2)
            tickets_[tm] = argi(2);

        return true;
    }

    if (StartsWith(cmd, "simai.")) {
        bool ok = true;

        if (AiExec(cmd, t, output, ok))
            return ok;
    }

    // object.property get/set
    if (cmd.find('.') != std::string::npos) {
        if (t.size() == 1) {
            auto it = vars_.find(cmd);

            if (it == vars_.end()) {
                out("Unknown object or method: " + t[0]);

                return false;
            }

            out(it->second);

            return true;
        }

        vars_[cmd] = rmm::Join(t, 1);  // qualified: member Join() hides it

        return true;
    }

    out("Unknown command: " + t[0]);

    return false;
}

// --------------------------------------------------------------------------------
// Simulated client/game actions
// --------------------------------------------------------------------------------

void SimAdapter::SetStatus(int32_t s) {
    if (s == status_)
        return;

    rmm_ev_game_status ev{status_, s};
    status_ = s;
    core_->Dispatch(RMM_EV_GAME_STATUS, &ev);
}

rmm_player_t SimAdapter::Join(const std::string& name, int32_t team, bool bot, const std::string& ip) {
    rmm_player_t id = 0;

    while (players_.count(id))
        ++id;

    if (id >= maxPlayers_) {
        std::printf("[sim] server full\n");

        return RMM_PLAYER_NONE;
    }

    rmm_player_info p{};
    p.struct_size = sizeof p;
    p.id = id;
    CopyOut(name, p.name, sizeof p.name);
    CopyOut(ip + ":" + std::to_string(29900 + id), p.address, sizeof p.address);
    p.team = team;
    p.ping = bot ? 0 : 40 + id;
    p.is_bot = bot;
    p.seat = -1;
    players_[id] = p;  // visible to plugins during the connect event

    std::string reject;

    if (!core_->OnPlayerConnect(id, name, p.address, reject)) {
        players_.erase(id);
        std::printf("[sim] %s rejected: %s\n", name.c_str(), reject.c_str());

        return RMM_PLAYER_NONE;
    }

    if (!players_.count(id))
        return RMM_PLAYER_NONE;  // kicked during connect

    std::printf("[sim] %s joined as #%d (team %d)\n", name.c_str(), id, team);
    // Auth arrives a moment later on real servers (GameSpy/CD-key check).
    CopyOut(std::to_string(10000000 + id * 7919), players_[id].auth_id, sizeof p.auth_id);
    core_->OnPlayerAuth(id);

    if (players_.count(id)) {
        rmm_ev_player_change tev{id, RMM_TEAM_NONE, team};
        core_->Dispatch(RMM_EV_PLAYER_TEAM, &tev);
    }

    return id;
}

void SimAdapter::Remove(rmm_player_t id, const std::string& why) {
    auto it = players_.find(id);

    if (it == players_.end())
        return;

    std::printf("[sim] %s left (%s)\n", it->second.name, why.c_str());
    core_->OnPlayerDisconnect(id);  // plugins can still query the player
    players_.erase(id);

    for (auto b = bots_.begin(); b != bots_.end();)  // a kicked bot takes its AI with it
        b = b->player == id ? bots_.erase(b) : std::next(b);
}

void SimAdapter::Leave(rmm_player_t id) { Remove(id, "disconnected"); }

void SimAdapter::Chat(rmm_player_t id, int32_t channel, const std::string& text) {
    auto it = players_.find(id);

    if (it == players_.end()) {
        std::printf("[sim] no player #%d\n", id);

        return;
    }

    std::string name = it->second.name;
    std::string msg = text;

    if (core_->OnChat(id, channel, msg)) {
        std::printf("[chat] (blocked) %s: %s\n", name.c_str(), text.c_str());

        return;
    }

    std::printf("[chat]%s %s: %s\n", channel == RMM_CHAT_TEAM ? " (team)" : "", name.c_str(), msg.c_str());
}

void SimAdapter::Spawn(rmm_player_t id) {
    auto it = players_.find(id);

    if (it == players_.end())
        return;

    it->second.is_alive = true;
    health_[id] = 100.f;
    it->second.position = rmm_vec3{static_cast<float>(id * 10), 50.f, static_cast<float>(id * -5)};
    CopyOut("Assault", it->second.kit, sizeof it->second.kit);
    GiveKitItems(id, "Assault");
    // Like BF1942's spawnPlayer: the spawn-menu kit is handed over (a pickup
    // that can't be blocked) before the spawn is reported.
    rmm_ev_player_kit kev{id, "Assault", 0, RMM_KIT_REASON_SPAWN};

    if (core_->Events().DispatchPre(RMM_EV_PLAYER_KIT_PICKUP, &kev, false) < RMM_HANDLED)
        core_->Events().DispatchPost(RMM_EV_PLAYER_KIT_PICKUP, &kev);

    if (!players_.count(id))
        return;  // kicked by a handler

    rmm_ev_player_spawn ev{id, players_[id].position, "Assault"};
    core_->Dispatch(RMM_EV_PLAYER_SPAWN, &ev);
}

void SimAdapter::SwapKit(rmm_player_t id, const std::string& kit, rmm_object_t kitObject, int32_t reason) {
    std::string old = players_[id].kit;

    if (!old.empty()) {
        rmm_ev_player_kit dev{id, old.c_str(), 0, reason};
        core_->Dispatch(RMM_EV_PLAYER_KIT_DROP, &dev);

        if (!players_.count(id))
            return;
    }

    CopyOut(kit, players_[id].kit, sizeof players_[id].kit);
    GiveKitItems(id, kit);
    std::printf("[sim] #%d now carries %s\n", id, kit.c_str());
}

// ---- items -------------------------------------------------------------------------

void SimAdapter::SetKitItems(const std::string& kit, const std::map<int32_t, std::string>& items) {
    kitItems_[ToLower(kit)] = items;
}

// A kit's items as set with .kititems, else BF1942's layout: knife, pistol,
// "<kit>_Gun", grenade. The main weapon (slot 3, else the lowest) is held.
void SimAdapter::GiveKitItems(rmm_player_t id, const std::string& kit) {
    auto k = kitItems_.find(ToLower(kit));
    items_[id] =
        k != kitItems_.end()
            ? k->second
            : std::map<int32_t, std::string>{{1, "KnifeAllies"}, {2, "Colt"}, {3, kit + "_Gun"}, {4, "GrenadeAllies"}};
    activeItem_[id] = items_[id].count(3) ? 3 : (items_[id].empty() ? -1 : items_[id].begin()->first);
}

bool SimAdapter::GetItems(rmm_player_t id, std::vector<rmm_item_info>& out) {
    auto it = players_.find(id);

    if (it == players_.end() || !health_.count(id) || it->second.vehicle)
        return false;

    for (auto& kv : items_[id]) {
        rmm_item_info info{};
        info.slot = kv.first;
        CopyOut(kv.second, info.weapon, sizeof info.weapon);
        info.active = kv.first == activeItem_[id];
        out.push_back(info);
    }

    return true;
}

bool SimAdapter::DoSwitch(rmm_player_t id, int32_t slot, int32_t reason) {
    auto it = players_.find(id);

    if (it == players_.end() || !health_.count(id) || it->second.vehicle || !items_[id].count(slot))
        return false;

    int32_t from = activeItem_[id];

    if (from == slot)
        return true;

    std::string fromName = items_[id].count(from) ? items_[id][from] : "", toName = items_[id][slot];
    rmm_ev_weapon_switch ev{id, from, slot, fromName.c_str(), toName.c_str(), reason};
    bool canBlock = reason == RMM_WEAPON_REASON_PLAYER || reason == RMM_WEAPON_REASON_NETWORK;

    if (core_->Events().DispatchPre(RMM_EV_PLAYER_WEAPON_SWITCH, &ev, canBlock) >= RMM_HANDLED) {
        std::printf("[sim] #%d's switch to %d:%s blocked\n", id, slot, toName.c_str());

        return false;
    }

    if (!players_.count(id) || !items_[id].count(slot))
        return false;

    activeItem_[id] = slot;
    std::printf("[sim] #%d holds %d:%s\n", id, slot, toName.c_str());
    core_->Events().DispatchPost(RMM_EV_PLAYER_WEAPON_SWITCH, &ev);

    return true;
}

void SimAdapter::SwitchWeapon(rmm_player_t id, int32_t slot) { DoSwitch(id, slot, RMM_WEAPON_REASON_PLAYER); }

int32_t SimAdapter::HeldItem(rmm_player_t id) const {
    auto it = activeItem_.find(id);

    return it == activeItem_.end() ? -1 : it->second;
}

bool SimAdapter::SelectItem(rmm_player_t id, int32_t slot) { return DoSwitch(id, slot, RMM_WEAPON_REASON_PLUGIN); }

bool SimAdapter::RemoveItem(rmm_player_t id, int32_t slot) {
    auto it = players_.find(id);

    if (it == players_.end() || !health_.count(id) || it->second.vehicle || !items_[id].count(slot))
        return false;

    if (activeItem_[id] == slot) {  // switch away first, like the BF1942 backend
        for (int32_t s : {3, 2, 1, 4, 5, 6, 7, 8})
            if (s != slot && items_[id].count(s) && DoSwitch(id, s, RMM_WEAPON_REASON_PLUGIN))
                break;

        if (activeItem_[id] == slot)
            activeItem_[id] = -1;
    }

    std::printf("[sim] #%d loses %d:%s\n", id, slot, items_[id][slot].c_str());
    items_[id].erase(slot);

    return true;
}

// Mirrors the BF1942 rules: living, on foot, and a template the game knows.
bool SimAdapter::SetKit(rmm_player_t id, const std::string& kit) {
    auto it = players_.find(id);

    if (it == players_.end() || !health_.count(id) || it->second.vehicle || !TemplateExists(kit))
        return false;

    rmm_ev_player_kit ev{id, kit.c_str(), 0, RMM_KIT_REASON_PLUGIN};

    if (core_->Events().DispatchPre(RMM_EV_PLAYER_KIT_PICKUP, &ev, false) >= RMM_HANDLED)
        return false;

    SwapKit(id, kit, 0, RMM_KIT_REASON_PLUGIN);

    if (players_.count(id))
        core_->Events().DispatchPost(RMM_EV_PLAYER_KIT_PICKUP, &ev);

    return true;
}

void SimAdapter::PickupKit(rmm_player_t id, rmm_object_t kit) {
    auto it = players_.find(id);
    SimObject* o = Obj(kit);

    if (it == players_.end() || !health_.count(id) || !o || o->vehicle) {
        std::printf("[sim] #%d can't pick up 0x%llx\n", id, (unsigned long long)kit);

        return;
    }

    std::string tmpl = o->tmpl;
    rmm_ev_player_kit ev{id, tmpl.c_str(), kit, RMM_KIT_REASON_PICKUP};

    if (core_->Events().DispatchPre(RMM_EV_PLAYER_KIT_PICKUP, &ev) >= RMM_HANDLED) {
        std::printf("[sim] #%d's pickup of %s blocked\n", id, tmpl.c_str());

        return;
    }

    if (!players_.count(id))
        return;

    std::string old = players_[id].kit;
    rmm_vec3 at = players_[id].position;
    objects_.erase(kit);  // in the player's hands now
    SwapKit(id, tmpl, kit, RMM_KIT_REASON_PICKUP);

    if (!old.empty()) {  // the old kit lands on the ground
        rmm_object_t h = nextObject_++;
        SimObject d;
        d.tmpl = old;
        d.root = h;
        d.pos = at;
        d.vehicle = false;
        objects_[h] = d;
        std::printf("[sim] %s dropped as 0x%llx\n", old.c_str(), (unsigned long long)h);
    }

    if (players_.count(id))
        core_->Events().DispatchPost(RMM_EV_PLAYER_KIT_PICKUP, &ev);
}

bool SimAdapter::EndRoundNow(int32_t winner) {
    if (status_ != RMM_STATUS_PLAYING)
        return false;

    EndRound(winner);

    return true;
}

void SimAdapter::Kill(rmm_player_t victim, rmm_player_t attacker, const std::string& weapon) {
    auto v = players_.find(victim);

    if (v == players_.end())
        return;

    if (v->second.vehicle) {
        DoExit(victim, RMM_VEHICLE_REASON_GAME);
        v = players_.find(victim);  // handlers may have removed the player

        if (v == players_.end())
            return;
    }

    v->second.is_alive = false;
    health_.erase(victim);
    v->second.deaths++;
    int victimTeam = v->second.team == 2 ? 2 : 1;

    if (tickets_[victimTeam] > 0)
        tickets_[victimTeam]--;

    bool suicide = attacker == victim;
    bool tk = false;
    int delta = 0, total = 0;
    std::string vehicle;
    auto a = players_.find(attacker);
    bool hasAttacker = a != players_.end();

    if (hasAttacker && !suicide) {
        tk = a->second.team == v->second.team;
        a->second.kills += tk ? 0 : 1;
        delta = tk ? -4 : 2;
        total = (a->second.score += delta);
        vehicle = a->second.vehicle_template;
    }

    // No iterators are used past this point: handlers may kick players.
    rmm_ev_player_killed ev{victim,
                            hasAttacker ? attacker : RMM_PLAYER_NONE,
                            weapon.c_str(),
                            vehicle.c_str(),
                            tk ? RMM_TRUE : RMM_FALSE,
                            suicide ? RMM_TRUE : RMM_FALSE};
    core_->Dispatch(RMM_EV_PLAYER_KILLED, &ev);

    if (delta != 0 && players_.count(attacker)) {
        rmm_ev_player_score sev{attacker, delta, total};
        core_->Dispatch(RMM_EV_PLAYER_SCORE, &sev);
    }
}

void SimAdapter::Damage(rmm_player_t victim, rmm_player_t attacker, float amount) {
    if (!players_.count(victim) || !health_.count(victim)) {
        std::printf("[sim] #%d is not alive\n", victim);

        return;
    }

    float dmg = amount;
    rmm_ev_player_damage ev{victim, attacker, &dmg, "sim_rifle"};
    int32_t res = core_->Dispatch(RMM_EV_PLAYER_DAMAGE, &ev);

    if (res >= RMM_HANDLED) {
        std::printf("[sim] damage to #%d blocked\n", victim);

        return;
    }

    auto h = health_.find(victim);  // handlers may have killed or kicked the victim

    if (h == health_.end())
        return;

    h->second -= dmg;
    std::printf("[sim] #%d took %.1f damage (health %.1f)\n", victim, dmg, h->second);

    if (h->second <= 0.f)
        Kill(victim, attacker, "sim_rifle");
}

// Direct damage from the API: no PLAYER_DAMAGE event (mirrors the real backends).
bool SimAdapter::DamagePlayer(rmm_player_t id, float amount, rmm_player_t attacker) {
    auto h = health_.find(id);

    if (h == health_.end())
        return false;

    h->second -= amount;
    std::printf("[sim] #%d took %.1f damage from the API (health %.1f)\n", id, amount, h->second);

    if (h->second <= 0.f)
        Kill(id, attacker, "rmm_damage");

    return true;
}

bool SimAdapter::PlayerHealth(rmm_player_t id, float& health, float& maxHealth) {
    auto h = health_.find(id);

    if (h == health_.end())
        return false;

    health = h->second;
    maxHealth = 100.f;

    return true;
}

// No physics in the sim: just report the impulse. Living players only; in a
// vehicle the vehicle's root is pushed, like the real backends.
bool SimAdapter::PushPlayer(rmm_player_t id, const rmm_vec3& v) {
    auto it = players_.find(id);

    if (it == players_.end() || !health_.count(id))
        return false;

    if (it->second.vehicle) {
        rmm_object_t root = RootOf(it->second.vehicle);
        std::printf("[sim] vehicle 0x%llx (#%d) pushed by %.1f,%.1f,%.1f\n",
                    (unsigned long long)(root ? root : it->second.vehicle), id, v.x, v.y, v.z);

        return true;
    }

    std::printf("[sim] #%d pushed by %.1f,%.1f,%.1f\n", id, v.x, v.y, v.z);

    return true;
}

// ---- world --------------------------------------------------------------------

SimAdapter::SimObject* SimAdapter::Obj(rmm_object_t h) {
    auto it = objects_.find(h);

    return it == objects_.end() ? nullptr : &it->second;
}

rmm_object_t SimAdapter::RootOf(rmm_object_t obj) const {
    auto it = objects_.find(obj);

    return it == objects_.end() ? 0 : it->second.root;
}

rmm_object_t SimAdapter::AddVehicle(const std::string& tmpl, const rmm_vec3& pos, const rmm_vec3& vel,
                                    rmm_object_t parent) {
    if (parent && !objects_.count(parent)) {
        std::printf("[sim] no object 0x%llx\n", (unsigned long long)parent);

        return 0;
    }

    rmm_object_t h = nextObject_++;
    SimObject o;
    o.tmpl = tmpl;
    o.root = parent ? RootOf(parent) : h;
    o.pos = parent ? objects_[parent].pos : pos;  // seats sit on their parent
    o.vel = parent ? rmm_vec3{0, 0, 0} : vel;
    o.seat = 0;

    for (auto& kv : objects_)
        if (kv.second.root == o.root)
            ++o.seat;  // root 0, then seats in the order added

    objects_[h] = o;
    templates_.emplace(ToLower(tmpl), true);

    if (!parent)
        vehicleTemplates_[ToLower(tmpl)] = true;

    std::printf("[sim] %s 0x%llx %s at %.1f,%.1f,%.1f\n", parent ? "seat" : "vehicle", (unsigned long long)h,
                tmpl.c_str(), o.pos.x, o.pos.y, o.pos.z);

    return h;
}

// ---- map spawners and spawn timing ------------------------------------------------

void SimAdapter::AddSpawner(const std::string& tmpl, const rmm_vec3& pos, float yaw, int32_t team) {
    Spawner s;
    s.handle = 0x10000 + static_cast<rmm_object_t>(spawners_.size());  // apart from object handles
    s.tmpl = tmpl;
    s.pos = pos;
    s.yaw = yaw;
    s.team = team;
    spawners_.push_back(s);
    std::printf("[sim] spawner 0x%llx for %s at %.1f,%.1f,%.1f\n", (unsigned long long)s.handle, tmpl.c_str(), pos.x,
                pos.y, pos.z);
    RunSpawners();
}

void SimAdapter::RunSpawners() {
    for (auto& s : spawners_) {
        if (s.object && objects_.count(s.object))
            continue;  // its object is still there

        s.object = 0;
        rmm_ev_spawner ev{s.handle, s.tmpl.c_str(), s.pos, rmm_vec3{s.yaw, 0, 0}, s.team, 0};

        if (core_->Events().DispatchPre(RMM_EV_SPAWNER_SPAWN, &ev, true) >= RMM_HANDLED) {
            std::printf("[sim] spawner 0x%llx: %s blocked\n", (unsigned long long)s.handle, s.tmpl.c_str());
            continue;
        }

        s.object = AddVehicle(s.tmpl, s.pos, rmm_vec3{0, 0, 0});
        objects_[s.object].rot = rmm_vec3{s.yaw, 0, 0};
        ev.object = s.object;
        core_->Events().DispatchPost(RMM_EV_SPAWNER_SPAWN, &ev);
    }
}

bool SimAdapter::GetSpawnTiming(rmm_spawn_timing& out) {
    out.delay = spawnTiming_.delay;
    out.waves = spawnTiming_.waves;
    out.wave_interval = spawnTiming_.wave_interval;

    return true;
}

bool SimAdapter::SetSpawnTiming(const rmm_spawn_timing& t) {
    spawnTiming_.delay = t.delay;
    spawnTiming_.waves = t.waves ? RMM_TRUE : RMM_FALSE;
    spawnTiming_.wave_interval = t.wave_interval;
    std::printf("[sim] spawn timing: delay %.1f, waves %s, interval %.1f\n", t.delay, t.waves ? "on" : "off",
                t.wave_interval);

    return true;
}

rmm_object_t SimAdapter::AddStatic(const std::string& tmpl, const rmm_vec3& pos) {
    rmm_object_t h = nextObject_++;
    SimObject o;
    o.tmpl = tmpl;
    o.root = h;
    o.pos = pos;
    o.vehicle = false;
    o.networked = false;  // like most BF1942 statics: loaded by each client from the map
    objects_[h] = o;
    templates_.emplace(ToLower(tmpl), false);
    std::printf("[sim] static 0x%llx %s at %.1f,%.1f,%.1f\n", (unsigned long long)h, tmpl.c_str(), pos.x, pos.y, pos.z);

    return h;
}

void SimAdapter::AddTemplate(const std::string& tmpl, bool networked, bool vehicle) {
    templates_[ToLower(tmpl)] = networked;

    if (vehicle)
        vehicleTemplates_[ToLower(tmpl)] = true;
}

std::vector<IGameAdapter::VehicleRef> SimAdapter::Vehicles() {
    std::vector<VehicleRef> v;

    for (auto& kv : objects_)
        if (kv.second.vehicle)
            v.push_back(VehicleRef{kv.first, kv.second.tmpl, kv.second.root == kv.first});

    return v;
}

std::vector<IGameAdapter::ObjectRef> SimAdapter::WorldObjects() {
    std::vector<ObjectRef> v;

    for (auto& kv : objects_)
        if (kv.second.root == kv.first)
            v.push_back(ObjectRef{kv.first, kv.second.tmpl, kv.second.pos, kv.second.vehicle, false});

    return v;
}

bool SimAdapter::TemplateExists(const std::string& tmpl) { return templates_.count(ToLower(tmpl)) != 0; }

rmm_object_t SimAdapter::SpawnObject(const std::string& tmpl, const rmm_vec3& pos, const rmm_vec3& ypr) {
    auto t = templates_.find(ToLower(tmpl));

    if (t == templates_.end())
        return 0;

    rmm_object_t h = nextObject_++;
    SimObject o;
    o.tmpl = tmpl;
    o.root = h;
    o.pos = pos;
    o.rot = ypr;
    o.vehicle = vehicleTemplates_.count(ToLower(tmpl)) != 0;
    o.seat = o.vehicle ? 0 : -1;
    o.networked = t->second;
    objects_[h] = o;
    std::printf("[sim] spawned 0x%llx %s at %.1f,%.1f,%.1f yaw %.0f%s\n", (unsigned long long)h, tmpl.c_str(), pos.x,
                pos.y, pos.z, ypr.x, o.networked ? "" : " (server-only)");

    return h;
}

bool SimAdapter::RemoveObject(rmm_object_t obj) {
    rmm_object_t root = RootOf(obj);

    if (!root)
        return false;

    for (auto& kv : players_)  // the engine kills whoever sits in a removed vehicle
        if (kv.second.vehicle && RootOf(kv.second.vehicle) == root)
            Kill(kv.first, RMM_PLAYER_NONE, "");

    for (auto it = objects_.begin(); it != objects_.end();)
        it = it->second.root == root ? objects_.erase(it) : std::next(it);

    std::printf("[sim] removed 0x%llx\n", (unsigned long long)root);

    return true;
}

// Like a weapon hit on the root, without the damage event. A
// destroyed vehicle kills its occupants, then VEHICLE_DESTROYED is reported;
// the wreck stays (health <= 0) until something removes it.
bool SimAdapter::DamageObject(rmm_object_t obj, float amount, rmm_player_t attacker) {
    rmm_object_t root = RootOf(obj);
    SimObject* r = Obj(root);

    if (!r || r->health <= 0.f)
        return false;

    r->health -= amount;
    std::string tmpl = r->tmpl;
    bool vehicle = r->vehicle;
    std::printf("[sim] %s 0x%llx took %.1f damage from the API (health %.1f)\n", tmpl.c_str(), (unsigned long long)root,
                amount, r->health);

    if (r->health > 0.f)
        return true;

    std::printf("[sim] %s 0x%llx explodes\n", tmpl.c_str(), (unsigned long long)root);

    for (rmm_player_t p : ObjectOccupants(root))
        if (health_.count(p))
            Kill(p, attacker, "explosion");

    if (vehicle) {
        rmm_ev_vehicle ev{attacker, root, tmpl.c_str(), -1, RMM_FALSE, root, tmpl.c_str(), RMM_VEHICLE_REASON_GAME};
        core_->Dispatch(RMM_EV_VEHICLE_DESTROYED, &ev);
    } else {
        rmm_ev_object_destroyed ev{root, tmpl.c_str(), attacker, ""};
        core_->Dispatch(RMM_EV_OBJECT_DESTROYED, &ev);
    }

    return true;
}

bool SimAdapter::GroundHeight(float x, float, float& height) {
    height = 10.0f + x / 100.0f;

    return true;
}

std::vector<rmm_player_t> SimAdapter::ObjectOccupants(rmm_object_t obj) {
    std::vector<rmm_player_t> v;
    rmm_object_t root = RootOf(obj);

    if (!root)
        return v;

    for (auto& kv : players_)
        if (kv.second.vehicle && RootOf(kv.second.vehicle) == root)
            v.push_back(kv.first);

    return v;
}

bool SimAdapter::GetObjectInfo(rmm_object_t obj, rmm_object_info& out) {
    SimObject* o = Obj(obj);

    if (!o)
        return false;

    SimObject* r = Obj(o->root);
    out.object = obj;
    out.root = o->root;
    CopyOut(o->tmpl, out.template_name, sizeof out.template_name);
    CopyOut(r ? r->tmpl : o->tmpl, out.root_template, sizeof out.root_template);
    out.position = o->pos;
    out.rotation = r ? r->rot : o->rot;
    out.velocity = r ? r->vel : o->vel;
    out.angular_velocity = r ? r->ang : o->ang;
    out.health = r ? r->health : o->health;
    out.max_health = 1000.f;
    out.occupant_count = static_cast<int32_t>(ObjectOccupants(obj).size());
    out.is_vehicle = o->vehicle ? RMM_TRUE : RMM_FALSE;
    out.is_root = o->root == obj;
    out.is_destroyed = out.health <= 0.f;
    out.is_networked = (r ? r->networked : o->networked) ? RMM_TRUE : RMM_FALSE;
    out.seat = o->vehicle ? o->seat : -1;
    out.seat_occupant = SeatTakenBy(obj);

    return true;
}

bool SimAdapter::SetObjectPosition(rmm_object_t obj, const rmm_vec3& pos) {
    SimObject* r = Obj(RootOf(obj));

    if (!r)
        return false;

    rmm_vec3 d{pos.x - r->pos.x, pos.y - r->pos.y, pos.z - r->pos.z};
    rmm_object_t root = RootOf(obj);

    for (auto& kv : objects_)  // children move with the root
        if (kv.second.root == root) {
            kv.second.pos.x += d.x;
            kv.second.pos.y += d.y;
            kv.second.pos.z += d.z;
        }

    return true;
}

bool SimAdapter::SetObjectRotation(rmm_object_t obj, const rmm_vec3& ypr) {
    SimObject* r = Obj(RootOf(obj));

    if (!r)
        return false;

    r->rot = ypr;

    return true;
}

bool SimAdapter::SetObjectVelocity(rmm_object_t obj, const rmm_vec3& linear, const rmm_vec3& angular) {
    SimObject* r = Obj(RootOf(obj));

    if (!r)
        return false;

    r->vel = linear;
    r->ang = angular;
    std::printf("[sim] 0x%llx velocity set to %.1f,%.1f,%.1f\n", (unsigned long long)RootOf(obj), linear.x, linear.y,
                linear.z);

    return true;
}

void SimAdapter::Drift(double seconds) {
    for (auto& kv : objects_) {
        if (kv.second.root != kv.first)
            continue;

        rmm_vec3 v = kv.second.vel;

        if (v.x == 0 && v.y == 0 && v.z == 0)
            continue;

        rmm_vec3 p = kv.second.pos;
        SetObjectPosition(kv.first,
                          rmm_vec3{p.x + float(v.x * seconds), p.y + float(v.y * seconds), p.z + float(v.z * seconds)});
        std::printf("[sim] 0x%llx %s drifted to %.1f,%.1f,%.1f\n", (unsigned long long)kv.first, kv.second.tmpl.c_str(),
                    kv.second.pos.x, kv.second.pos.y, kv.second.pos.z);
    }
}

void SimAdapter::VehicleDamage(rmm_object_t obj, rmm_player_t attacker, float amount) {
    rmm_object_t root = RootOf(obj);
    SimObject* r = Obj(root);

    if (!r) {
        std::printf("[sim] no object 0x%llx\n", (unsigned long long)obj);

        return;
    }

    std::string tmpl = r->tmpl;
    auto occ = ObjectOccupants(root);
    float dmg = amount;
    rmm_ev_vehicle_damage ev{root,
                             tmpl.c_str(),
                             attacker,
                             &dmg,
                             "sim_rocket",
                             static_cast<int32_t>(occ.size()),
                             occ.empty() ? nullptr : occ.data()};

    if (core_->Dispatch(RMM_EV_VEHICLE_DAMAGE, &ev) >= RMM_HANDLED) {
        std::printf("[sim] damage to %s blocked\n", tmpl.c_str());

        return;
    }

    if ((r = Obj(root)) == nullptr)
        return;

    r->health -= dmg;
    std::printf("[sim] %s took %.1f damage (health %.1f)\n", tmpl.c_str(), dmg, r->health);
}

rmm_object_t SimAdapter::SeatObject(rmm_object_t root, int32_t seat) const {
    for (auto& kv : objects_)
        if (kv.second.vehicle && kv.second.root == root && kv.second.seat == seat)
            return kv.first;

    return 0;
}

rmm_player_t SimAdapter::SeatTakenBy(rmm_object_t obj) const {
    for (auto& kv : players_)
        if (kv.second.vehicle == obj)
            return kv.first;

    return RMM_PLAYER_NONE;
}

// One enter, BF1942 style: PRE before the player is in (blockable unless the
// reason is SWITCH or PLUGIN), POST once they are.
bool SimAdapter::DoEnter(rmm_player_t id, rmm_object_t h, int32_t reason) {
    auto it = players_.find(id);

    if (it == players_.end() || !objects_.count(h) || SeatTakenBy(h) != RMM_PLAYER_NONE)
        return false;

    rmm_object_t root = RootOf(h);
    std::string tmpl = objects_[h].tmpl, rootTmpl = objects_[root].tmpl;
    int32_t seat = objects_[h].seat;
    rmm_ev_vehicle ev{id, h, tmpl.c_str(), seat, seat == 0 ? RMM_TRUE : RMM_FALSE, root, rootTmpl.c_str(), reason};
    bool canBlock = reason != RMM_VEHICLE_REASON_SWITCH && reason != RMM_VEHICLE_REASON_PLUGIN;

    if (core_->Events().DispatchPre(RMM_EV_VEHICLE_ENTER, &ev, canBlock) >= RMM_HANDLED) {
        std::printf("[sim] #%d kept out of %s\n", id, tmpl.c_str());

        return false;
    }

    it = players_.find(id);

    if (it == players_.end() || !objects_.count(h))
        return false;  // kicked, or the vehicle was removed

    it->second.vehicle = h;
    it->second.seat = seat;
    CopyOut(tmpl, it->second.vehicle_template, sizeof it->second.vehicle_template);
    std::printf("[sim] #%d in %s seat %d\n", id, rootTmpl.c_str(), seat);
    core_->Events().DispatchPost(RMM_EV_VEHICLE_ENTER, &ev);

    return true;
}

// One exit: blockable only when the player chose it (PLAYER, SWITCH).
bool SimAdapter::DoExit(rmm_player_t id, int32_t reason) {
    auto it = players_.find(id);

    if (it == players_.end() || !it->second.vehicle)
        return false;

    std::string tmpl = it->second.vehicle_template;
    rmm_object_t h = it->second.vehicle, root = RootOf(h);
    std::string rootTmpl = root ? objects_[root].tmpl : tmpl;
    rmm_ev_vehicle ev{id,
                      h,
                      tmpl.c_str(),
                      it->second.seat,
                      it->second.seat == 0 ? RMM_TRUE : RMM_FALSE,
                      root ? root : h,
                      rootTmpl.c_str(),
                      reason};
    bool canBlock = reason == RMM_VEHICLE_REASON_PLAYER || reason == RMM_VEHICLE_REASON_SWITCH;

    if (core_->Events().DispatchPre(RMM_EV_VEHICLE_EXIT, &ev, canBlock) >= RMM_HANDLED) {
        std::printf("[sim] #%d kept in %s\n", id, rootTmpl.c_str());

        return false;
    }

    it = players_.find(id);

    if (it == players_.end())
        return false;

    it->second.vehicle = 0;
    it->second.seat = -1;
    it->second.vehicle_template[0] = '\0';
    std::printf("[sim] #%d out of %s\n", id, rootTmpl.c_str());
    core_->Events().DispatchPost(RMM_EV_VEHICLE_EXIT, &ev);

    return true;
}

void SimAdapter::Enter(rmm_player_t id, const std::string& vehicle, int32_t seat) {
    auto it = players_.find(id);

    if (it == players_.end())
        return;

    if (it->second.vehicle) {
        std::printf("[sim] #%d is already in a vehicle\n", id);

        return;
    }

    // Reuse a placed object with this template (roots first), else place one here.
    rmm_object_t h = 0;

    for (int pass = 0; pass < 2 && !h; ++pass)
        for (auto& kv : objects_)
            if (IEquals(kv.second.tmpl, vehicle) && (pass == 1 || kv.second.root == kv.first)) {
                h = kv.first;
                break;
            }

    if (!h)
        h = AddVehicle(vehicle, it->second.position, rmm_vec3{0, 0, 0});

    if (rmm_object_t s = SeatObject(RootOf(h), seat))
        h = s;

    DoEnter(id, h, RMM_VEHICLE_REASON_PLAYER);
}

void SimAdapter::Exit(rmm_player_t id) { DoExit(id, RMM_VEHICLE_REASON_PLAYER); }

// F1-F8, as BF1942's checkPlayerTriggers does it: exit the old seat, enter the
// new one. A blocked exit cancels the switch.
void SimAdapter::SwitchSeat(rmm_player_t id, int32_t seat) {
    auto it = players_.find(id);

    if (it == players_.end() || !it->second.vehicle)
        return;

    rmm_object_t target = SeatObject(RootOf(it->second.vehicle), seat);

    if (!target || target == it->second.vehicle || SeatTakenBy(target) != RMM_PLAYER_NONE) {
        std::printf("[sim] #%d can't switch to seat %d\n", id, seat);

        return;
    }

    if (DoExit(id, RMM_VEHICLE_REASON_SWITCH))
        DoEnter(id, target, RMM_VEHICLE_REASON_SWITCH);
}

bool SimAdapter::PutInVehicle(rmm_player_t id, rmm_object_t vehicle, int32_t seat) {
    auto it = players_.find(id);
    rmm_object_t root = RootOf(vehicle);

    if (it == players_.end() || !health_.count(id) || !root || !objects_[root].vehicle)
        return false;

    rmm_object_t target = 0;

    if (seat >= 0) {
        target = SeatObject(root, seat);
    } else {  // first free seat, driver first
        for (int32_t s = 0; s < 16 && !target; ++s)
            if (rmm_object_t o = SeatObject(root, s); o && SeatTakenBy(o) == RMM_PLAYER_NONE)
                target = o;
    }

    if (!target || SeatTakenBy(target) != RMM_PLAYER_NONE)
        return false;

    if (it->second.vehicle)
        DoExit(id, RMM_VEHICLE_REASON_PLUGIN);

    return DoEnter(id, target, RMM_VEHICLE_REASON_PLUGIN);
}

bool SimAdapter::TakeOutOfVehicle(rmm_player_t id) {
    auto it = players_.find(id);

    return it != players_.end() && health_.count(id) && it->second.vehicle && DoExit(id, RMM_VEHICLE_REASON_PLUGIN);
}

// --------------------------------------------------------------------------------
// Fake AI console. Mirrors what BF1942's ai.* commands print (docs/re/bf1942-linux.md),
// including the hazard RMM must avoid: listing bots outside co-op "crashes".
// --------------------------------------------------------------------------------

static const char* kSimBehaviours[] = {"Avoid", "MoveTo", "Idle", "Fire", "Scout", "TakeCover", "Change", "Special"};
static const char* kSimStrategies[] = {"west", "east", "south", "north", "wide", "finalPush", "breakOut"};

bool SimAdapter::AiRunning() const { return IContains(currentMap_.mode, "coop"); }

SimAdapter::SimBot* SimAdapter::BotById(int32_t id) {
    for (auto& b : bots_)
        if (b.id == id)
            return &b;

    return nullptr;
}

static std::string SimVec(const rmm_vec3& v) { return Format("%g/%g/%g", v.x, v.y, v.z); }

static bool SimParseVec(const std::string& s, rmm_vec3& v) {
    return std::sscanf(s.c_str(), "%f/%f/%f", &v.x, &v.y, &v.z) == 3;
}

rmm_player_t SimAdapter::AddEngineBot(const std::string& name, int32_t team) {
    if (!AiRunning()) {
        std::printf("[sim] the AI only runs on co-op maps\n");

        return RMM_PLAYER_NONE;
    }

    rmm_player_t id = Join(name, team, true, "bot");

    if (id == RMM_PLAYER_NONE)
        return id;

    Spawn(id);
    bots_.push_back(SimBot{nextBotId_++, id, name, "KTAssault"});
    std::printf("[sim] AI bot %d is %s (#%d)\n", bots_.back().id, name.c_str(), id);

    return id;
}

// One AI input tick, like BF1942's BotMain::updatePlayerAction -> AIPlayer::addInput:
// the "engine AI" proposes @p engine, RMM's BotControl may replace it, and the
// result is what the soldier would do. Printed so scripts can check it.
void SimAdapter::BotInputTick(const std::string& name, const BotControl::Frame& engine) {
    for (auto& b : bots_) {
        if (b.name != name)
            continue;

        BotControl::Frame f = engine;
        bool changed = core_->BotInput().Filter(b.player, f, core_->Time());
        std::string s;

        for (int i = 0; i < RMM_BOT_INPUT_SLOTS; ++i)
            if (((f.mask >> i) & 1u) && f.value[i] != 0.0f)
                s += Format(" %s=%g", BotInputSlotName(i).c_str(), f.value[i]);

        std::printf("[sim] %s input%s:%s\n", name.c_str(), changed ? " (rmm)" : "", s.empty() ? " idle" : s.c_str());

        return;
    }

    std::printf("[sim] no AI bot named %s\n", name.c_str());
}

bool SimAdapter::AiExec(const std::string& cmd, const std::vector<std::string>& t, std::string* output, bool& ok) {
    auto arg = [&](size_t i) { return i < t.size() ? t[i] : std::string(); };
    auto argi = [&](size_t i) { return std::atoi(arg(i).c_str()); };
    auto out = [&](const std::string& s) {
        if (output) {
            if (!output->empty())
                *output += "\n";

            *output += s;
        }
    };

    std::string m = cmd.substr(6);  // after "simai."

    // Like BF1942 outside co-op: most methods are unknown...
    if (!AiRunning()) {
        if (m == "getallbots") {  // ...but this one takes the server down. RMM must never send it here.
            std::printf("SIMULATED CRASH: AI bot list requested outside co-op\n");
            ok = false;

            return true;
        }

        if (m != "getbotskill" && m != "setbotskill") {
            out("Error  (0): " + rmm::Join(t, 0));
            out("Error : Unknown object or method!");
            ok = true;  // the engine "ran" the line; only the output tells

            return true;
        }
    }

    std::string ms = m;

    if (ms == "getbotcount") {
        out(std::to_string(bots_.size()));

        return true;
    }

    if (ms == "getallbots") {
        for (auto& b : bots_)
            out(std::to_string(b.id) + " " + b.name);

        return true;
    }

    if (ms == "getbotskill") {
        out(Format("%g", botSkill_));

        return true;
    }

    if (ms == "setbotskill") {
        botSkill_ = static_cast<float>(std::atof(arg(1).c_str()));
        std::printf("[sim] bot skill %g\n", botSkill_);

        return true;
    }

    if (ms == "getactivestrategy") {
        int tm = argi(1);
        out(tm == 1 || tm == 2 ? strategy_[tm] : "");

        return true;
    }

    if (ms == "listsaistrategies") {
        for (size_t i = 0; i < sizeof kSimStrategies / sizeof *kSimStrategies; ++i)
            out(std::to_string(i) + " " + kSimStrategies[i]);

        return true;
    }

    if (ms == "forcestrategy") {
        int tm = argi(2);

        for (const char* s : kSimStrategies)
            if (arg(1) == s && (tm == 1 || tm == 2)) {
                strategy_[tm] = s;
                std::printf("[sim] team %d strategy %s\n", tm, s);
            }

        return true;
    }

    if (ms == "createaiplayer") {  // name team kit [x/y/z]: the player only; the AI attaches with addBot
        rmm_player_t id = Join(arg(1), argi(2), true, "bot");

        if (id == RMM_PLAYER_NONE)
            return true;

        Spawn(id);
        rmm_vec3 p;

        if (SimParseVec(arg(4), p))
            players_[id].position = p;

        CopyOut(arg(3), players_[id].kit, sizeof players_[id].kit);
        addAttempts_[arg(1)] = 0;

        return true;
    }

    if (ms == "addbot") {  // name team moral personality vehicle -> bot id or -1
        std::string name = arg(1);
        auto a = addAttempts_.find(name);

        // The engine finds the new player only from the next frame on.
        if (a == addAttempts_.end() || a->second++ == 0) {
            out("-1");

            return true;
        }

        if (arg(3) != "default" || (arg(4) != "StandardWeights" && arg(4) != "UnitWeights")) {
            out("-1");

            return true;
        }

        if (StartsWith(name, "Reject")) {
            out("-1");

            return true;
        }  // test hook: the AI is "full" for these

        for (auto& kv : players_)
            if (kv.second.is_bot && name == kv.second.name) {
                addAttempts_.erase(a);
                bots_.push_back(SimBot{nextBotId_++, kv.first, name, kv.second.kit});
                std::printf("[sim] AI bot %d is %s (#%d)\n", bots_.back().id, name.c_str(), kv.first);
                out(std::to_string(bots_.back().id));

                return true;
            }

        out("-1");

        return true;
    }

    if (ms == "removeallbots") {
        auto copy = bots_;
        bots_.clear();

        for (auto& b : copy)
            Remove(b.player, "bot removed");

        return true;
    }

    // ---- per-bot commands: bot id is the first argument, except botMoveTo (last) ----
    int32_t id = ms == "botmoveto" ? argi(3) : argi(1);
    SimBot* b = BotById(id);

    if (ms == "removebot") {
        if (!b) {
            out("Failed to find bot with id: " + std::to_string(id) + ".");

            return true;
        }

        std::string name = b->name;
        rmm_player_t player = b->player;
        bots_.erase(bots_.begin() + (b - bots_.data()));
        Remove(player, "bot removed");
        out("Bot " + name + " id: " + std::to_string(id) + " has been removed.");

        return true;
    }

    if (ms == "getbotposition" || ms == "getbotgoalposition") {
        rmm_vec3 v{};

        if (b)
            v = ms == "getbotposition" ? players_[b->player].position : b->goal;

        out(SimVec(v));

        return true;
    }

    if (!b) {
        out("0");

        return true;
    }  // the engine answers with defaults for unknown ids

    auto& p = players_[b->player];

    if (ms == "getbotside") {
        out(std::to_string(p.team));

        return true;
    }

    if (ms == "getbotkit") {
        out(b->kit);

        return true;
    }

    if (ms == "getbotvehicle") {
        out(p.vehicle ? p.vehicle_template : "NoVehicle");

        return true;
    }

    if (ms == "getbotbehaviourlist") {
        for (size_t i = 0; i < sizeof kSimBehaviours / sizeof *kSimBehaviours; ++i)
            out(std::to_string(i) + " " + kSimBehaviours[i]);

        return true;
    }

    if (ms == "getactivebehaviour") {
        for (size_t i = 0; i < sizeof kSimBehaviours / sizeof *kSimBehaviours; ++i)
            if (b->behaviour == kSimBehaviours[i])
                out(std::to_string(i));

        return true;
    }

    if (ms == "botmoveto") {  // radius x/y/z id
        if (SimParseVec(arg(2), b->goal))
            b->behaviour = "MoveTo";

        std::printf("[sim] bot %d (%s) moving to %s within %s m\n", id, b->name.c_str(), SimVec(b->goal).c_str(),
                    arg(1).c_str());

        return true;
    }

    if (ms == "botsetwaypoints_patrol") {  // id radius count p1 p2
        if (SimParseVec(arg(4), b->goal))
            b->behaviour = "MoveTo";

        std::printf("[sim] bot %d (%s) patrolling %d point(s) %s %s\n", id, b->name.c_str(), argi(3), arg(4).c_str(),
                    argi(3) > 1 ? arg(5).c_str() : "");

        return true;
    }

    if (ms == "botgiveorder") {  // id behaviour modifiers - unknown names are silently ignored
        for (const char* n : kSimBehaviours)
            if (arg(2) == n && (arg(3) == "StandardWeights" || arg(3) == "MoveToMods"))
                b->behaviour = n;

        std::printf("[sim] bot %d (%s) ordered %s (%s)\n", id, b->name.c_str(), arg(2).c_str(), arg(3).c_str());

        return true;
    }

    return false;  // unknown simai method: fall through to the generic handler
}

void SimAdapter::LoadMap(const std::string& map, const std::string& mode, int32_t size) {
    // The engine drops its AI bots with the level.
    if (!bots_.empty()) {
        auto copy = bots_;
        bots_.clear();

        for (auto& b : copy)
            Remove(b.player, "level unloaded");
    }

    addAttempts_.clear();
    SetStatus(RMM_STATUS_LOADING);
    NoteMap(map, mode, size);
    tickets_[1] = tickets_[2] = 200;

    for (auto& cp : cps_) {
        cp.team = cp.startTeam;
        cp.locked = false;
    }

    objects_.clear();
    spawners_.clear();  // each level has its own
    health_.clear();

    for (auto& kv : players_) {
        kv.second.is_alive = false;
        kv.second.vehicle = 0;
        kv.second.seat = -1;
        kv.second.vehicle_template[0] = '\0';
    }

    core_->OnMapLoad(map, mode, mod_, size);
    SetStatus(RMM_STATUS_PLAYING);
    rmm_ev_map ev{currentMap_.map, currentMap_.mode, mod_.c_str(), size};
    core_->Dispatch(RMM_EV_ROUND_START, &ev);
}

void SimAdapter::EndRound(int32_t winner) {
    rmm_ev_round_end ev{winner, 0};
    core_->Dispatch(RMM_EV_ROUND_END, &ev);
    SetStatus(RMM_STATUS_ENDGAME);
}

void SimAdapter::Capture(const std::string& cp, int32_t team) {
    // No positions in the sim: every living player of the capturing team took it.
    std::vector<rmm_player_t> who;

    for (auto& kv : players_)
        if (kv.second.is_alive && team != RMM_TEAM_NONE && kv.second.team == team)
            who.push_back(kv.first);

    for (auto& c : cps_) {
        if (ToLower(c.name) != ToLower(cp))
            continue;

        if (c.locked || c.fixed) {
            std::printf("[sim] capture of %s by team %d blocked (%s)\n", c.name.c_str(), team,
                        c.locked ? "locked" : "fixed");

            return;
        }

        int32_t old = c.team;
        c.team = team;

        if (old != team)
            CpChanged(c, old, who);

        return;
    }

    int32_t old = cpOwner_.count(cp) ? cpOwner_[cp] : RMM_TEAM_NONE;
    cpOwner_[cp] = team;
    rmm_ev_control_point ev{
        0, cp.c_str(), old, team, static_cast<int32_t>(who.size()), who.empty() ? nullptr : who.data()};
    core_->Dispatch(RMM_EV_CONTROL_POINT, &ev);
}

void SimAdapter::CpChanged(SimCp& cp, int32_t old, const std::vector<rmm_player_t>& who) {
    std::string name = cp.name;  // handlers may add control points
    rmm_ev_control_point ev{
        cp.handle, name.c_str(), old, cp.team, static_cast<int32_t>(who.size()), who.empty() ? nullptr : who.data()};
    core_->Dispatch(RMM_EV_CONTROL_POINT, &ev);
}

void SimAdapter::AddControlPoint(const std::string& name, const rmm_vec3& pos, int32_t team, bool fixed) {
    SimCp c;
    c.handle = 0x20000 + static_cast<rmm_object_t>(cps_.size());
    c.name = name;
    c.pos = pos;
    c.team = c.startTeam = team;
    c.fixed = fixed;
    cps_.push_back(c);
    std::printf("[sim] control point 0x%llx %s team %d%s\n", (unsigned long long)c.handle, name.c_str(), team,
                fixed ? " (fixed)" : "");
}

SimAdapter::SimCp* SimAdapter::Cp(rmm_object_t h) {
    for (auto& c : cps_)
        if (c.handle == h)
            return &c;

    return nullptr;
}

std::vector<rmm_object_t> SimAdapter::ControlPoints() {
    std::vector<rmm_object_t> v;

    for (auto& c : cps_)
        v.push_back(c.handle);

    return v;
}

bool SimAdapter::GetControlPoint(rmm_object_t h, rmm_control_point_info& out) {
    SimCp* c = Cp(h);

    if (!c)
        return false;

    std::snprintf(out.name, sizeof out.name, "%s", c->name.c_str());
    out.position = c->pos;
    out.radius = 10.0f;
    out.team = c->team;
    out.state = RMM_CP_STATE_HELD;
    out.locked = c->locked ? RMM_TRUE : RMM_FALSE;
    out.fixed = c->fixed ? RMM_TRUE : RMM_FALSE;

    return true;
}

bool SimAdapter::SetControlPoint(rmm_object_t h, int32_t team) {
    SimCp* c = Cp(h);

    if (!c)
        return false;

    int32_t old = c->team;
    c->team = team;
    std::printf("[sim] %s set to team %d\n", c->name.c_str(), team);

    if (old != team)
        CpChanged(*c, old, {});

    return true;
}

bool SimAdapter::LockControlPoint(rmm_object_t h, bool locked) {
    SimCp* c = Cp(h);

    if (!c)
        return false;

    if (c->locked != locked)
        std::printf("[sim] %s %s\n", c->name.c_str(), locked ? "locked" : "unlocked");

    c->locked = locked;

    return true;
}

void SimAdapter::ChangeTeamAction(rmm_player_t id, int32_t team) {
    auto it = players_.find(id);

    if (it == players_.end() || it->second.team == team)
        return;

    int32_t old = it->second.team;
    it->second.team = team;
    rmm_ev_player_change ev{id, old, team};
    core_->Dispatch(RMM_EV_PLAYER_TEAM, &ev);
}

}  // namespace rmm
