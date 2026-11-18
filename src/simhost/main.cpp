// rmm_simhost - run the RMM core and real plugins against a simulated server.
//
//   rmm_simhost [--root <rmm dir>] [--game bf1942|bfv|bf2|bf2142] [--script <file>]
//
// Lines starting with '.' are simulated player/game actions; anything else is
// typed into the server console (RMM commands, RMM vars, then the fake engine).
// See docs/SIMHOST.md.
#include "../core/core.h"
#include "../core/platform.h"
#include "../core/util.h"
#include "../games/sim/sim_adapter.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>

using namespace rmm;

namespace {

std::mutex g_mu;
std::deque<std::string> g_lines;
std::atomic<bool> g_eof{false};

void ReaderThread() {
    std::string line;

    while (std::getline(std::cin, line)) {
        std::lock_guard<std::mutex> lk(g_mu);
        g_lines.push_back(line);
    }

    g_eof = true;
}

void PrintHelp() {
    std::printf(
        "Simulated actions:\n"
        "  .join <name> [team=1] [bot=0] [ip]   .leave <id>\n"
        "  .say <id> <text...>                   .teamsay <id> <text...>\n"
        "  .spawn <id>                           .kill <victim> [attacker] [weapon]\n"
        "  .damage <victim> <attacker> <amount>  .team <id> <team>\n"
        "  .enter <id> <vehicle> [seat=0]        .exit <id>     .switch <id> <seat>  (F1-F8)\n"
        "  .map <map> [mode] [size]              .endround [winner]\n"
        "  .capture <cp name> <team>             .players\n"
        "  .cp <name> <x y z> [team] [fixed]   (a control point of every map loaded after it)\n"
        "  .vehicle <template> [x y z] [vx vy vz]   .seat <parent handle> <template>\n"
        "  .vdamage <handle> <attacker> <amount>  .drift <seconds>\n"
        "  .static <template> [x y z]   .template <name> [server|vehicle]  (spawnable; server = not networked)\n"
        "  .aibot <name> [team=1]   (a bot the game adds itself; AI runs on *coop* maps only)\n"
        "  .botinput <bot name> [slot=value ...]   (one AI input tick; prints what the soldier gets)\n"
        "  .pickup <id> <kit handle>   (pick up a kit lying on the ground, e.g. one from rmm spawn)\n"
        "  .weapon <id> <slot>   (weapon key: hold that item)   .kititems <kit> <slot>=<weapon> ...\n"
        "  .spawner <template> <x y z> [team] [yaw]   (map spawner)   .spawners  (respawn what's gone)\n"
        "  .query [reply]   (a GameSpy query reply as the server would send it, with the plugins' rules)\n"
        "  .wait <seconds>                       .pacing on|off  (chat pacing; scripts start off)\n"
        "  .help   .quit\n"
        "Anything else is a server console line, e.g.:\n"
        "  rmm plugins list | rmm vars | rmm events | rmm caps | help | sim.say \"hi\"\n");
}

bool HandleAction(SimAdapter& sim, const std::string& line, bool& quit) {
    auto a = SplitArgs(line.substr(1));

    if (a.empty())
        return true;

    std::string cmd = ToLower(a[0]);
    auto arg = [&](size_t i, const char* def = "") { return i < a.size() ? a[i] : std::string(def); };
    auto argi = [&](size_t i, int def) { return i < a.size() ? std::atoi(a[i].c_str()) : def; };

    if (cmd == "quit")
        quit = true;
    else if (cmd == "help")
        PrintHelp();
    else if (cmd == "join")
        sim.Join(arg(1, "Player"), argi(2, 1), argi(3, 0) != 0, arg(4, "127.0.0.1"));
    else if (cmd == "leave")
        sim.Leave(argi(1, -1));
    else if (cmd == "say")
        sim.Chat(argi(1, -1), RMM_CHAT_ALL, Join(a, 2));
    else if (cmd == "teamsay")
        sim.Chat(argi(1, -1), RMM_CHAT_TEAM, Join(a, 2));
    else if (cmd == "spawn")
        sim.Spawn(argi(1, -1));
    else if (cmd == "kill")
        sim.Kill(argi(1, -1), argi(2, -1), arg(3, "sim_rifle"));
    else if (cmd == "damage")
        sim.Damage(argi(1, -1), argi(2, -1), static_cast<float>(std::atof(arg(3, "10").c_str())));
    else if (cmd == "team")
        sim.ChangeTeamAction(argi(1, -1), argi(2, 1));
    else if (cmd == "enter")
        sim.Enter(argi(1, -1), arg(2, "jeep"), argi(3, 0));
    else if (cmd == "exit")
        sim.Exit(argi(1, -1));
    else if (cmd == "switch")
        sim.SwitchSeat(argi(1, -1), argi(2, 0));
    else if (cmd == "map")
        sim.LoadMap(arg(1, "strike_at_karkand"), arg(2, "gpm_cq"), argi(3, 32));
    else if (cmd == "endround")
        sim.EndRound(argi(1, 1));
    else if (cmd == "capture")
        sim.Capture(arg(1, "cp"), argi(2, 1));
    else if (cmd == "cp") {  // name x y z [team] [fixed]
        auto f = [&](size_t i) { return static_cast<float>(std::atof(arg(i, "0").c_str())); };
        sim.AddControlPoint(arg(1, "cp"), rmm_vec3{f(2), f(3), f(4)}, argi(5, 0), ToLower(arg(6)) == "fixed");
    } else if (cmd == "vehicle") {
        auto f = [&](size_t i) { return static_cast<float>(std::atof(arg(i, "0").c_str())); };
        sim.AddVehicle(arg(1, "jeep"), rmm_vec3{f(2), f(3), f(4)}, rmm_vec3{f(5), f(6), f(7)});
    } else if (cmd == "seat") {
        sim.AddVehicle(arg(2, "seat"), rmm_vec3{0, 0, 0}, rmm_vec3{0, 0, 0},
                       static_cast<rmm_object_t>(std::strtoull(arg(1, "0").c_str(), nullptr, 0)));
    } else if (cmd == "vdamage") {
        sim.VehicleDamage(static_cast<rmm_object_t>(std::strtoull(arg(1, "0").c_str(), nullptr, 0)), argi(2, -1),
                          static_cast<float>(std::atof(arg(3, "10").c_str())));
    } else if (cmd == "static") {
        auto f = [&](size_t i) { return static_cast<float>(std::atof(arg(i, "0").c_str())); };
        sim.AddStatic(arg(1, "house"), rmm_vec3{f(2), f(3), f(4)});
    } else if (cmd == "template") {
        std::string kind = ToLower(arg(2, ""));
        sim.AddTemplate(arg(1, "kit"), kind != "server", kind == "vehicle");
    } else if (cmd == "drift")
        sim.Drift(std::atof(arg(1, "1").c_str()));
    else if (cmd == "aibot")
        sim.AddEngineBot(arg(1, "EngineBot"), argi(2, 1));
    else if (cmd == "pickup")
        sim.PickupKit(argi(1, -1), static_cast<rmm_object_t>(std::strtoull(arg(2, "0").c_str(), nullptr, 0)));
    else if (cmd == "weapon")
        sim.SwitchWeapon(argi(1, -1), argi(2, -1));
    else if (cmd == "spawner") {  // template x y z [team] [yaw]
        auto f = [&](size_t i) { return static_cast<float>(std::atof(arg(i, "0").c_str())); };
        sim.AddSpawner(arg(1, "jeep"), rmm_vec3{f(2), f(3), f(4)}, f(6), argi(5, 0));
    } else if (cmd == "spawners")
        sim.RunSpawners();
    else if (cmd == "kititems") {  // kit slot=weapon ...
        std::map<int32_t, std::string> items;

        for (size_t i = 2; i < a.size(); ++i) {
            size_t eq = a[i].find('=');

            if (eq == std::string::npos) {
                std::printf("expected slot=weapon, got %s\n", a[i].c_str());

                return true;
            }

            items[std::atoi(a[i].substr(0, eq).c_str())] = a[i].substr(eq + 1);
        }

        sim.SetKitItems(arg(1, ""), items);
    } else if (cmd == "botinput") {  // name slot=value ...: one AI input tick proposing these values
        BotControl::Frame f;

        for (size_t i = 2; i < a.size(); ++i) {
            size_t eq = a[i].find('=');
            int slot = BotInputSlotFromName(a[i].substr(0, eq));

            if (slot < 0) {
                std::printf("unknown input slot %s\n", a[i].c_str());

                return true;
            }

            f.mask |= RMM_BI_BIT(slot);
            f.value[slot] = eq == std::string::npos ? 1.0f : static_cast<float>(std::atof(a[i].c_str() + eq + 1));
        }

        sim.BotInputTick(arg(1, ""), f);
    } else if (cmd == "players") {
        for (rmm_player_t id : sim.PlayerIds()) {
            rmm_player_info p{};
            p.struct_size = sizeof p;
            sim.GetPlayer(id, p);
            std::printf("  #%d %-16s team %d score %d K/D %d/%d %s%s auth %s kit %s\n", id, p.name, p.team, p.score,
                        p.kills, p.deaths, p.is_alive ? "alive" : "dead",
                        p.vehicle ? (std::string(" in ") + p.vehicle_template).c_str() : "", p.auth_id,
                        p.kit[0] ? p.kit : "-");
        }
    } else if (cmd == "query") {  // [reply]: what the send hook makes of a GameSpy query reply
        std::string reply =
            a.size() > 1 ? Join(a, 1) : "\\gamename\\bfield1942\\hostport\\14567\\final\\\\queryid\\1.1";
        char out[2048];
        int n = Core::Get().Query().Rewrite(reply.data(), static_cast<int>(reply.size()), out, sizeof out);
        std::printf("query reply: %s%s\n", n ? std::string(out, static_cast<size_t>(n)).c_str() : reply.c_str(),
                    n ? "" : " (unchanged)");
    } else if (cmd == "wait") {
        double until = plat::MonotonicSeconds() + std::atof(arg(1, "1").c_str());

        while (plat::MonotonicSeconds() < until) {
            Core::Get().Frame();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    } else if (cmd == "pacing") {
        bool on = ToLower(arg(1, "on")) != "off";
        Core::Get().Chat().SetBypass(!on);
        std::printf("chat pacing %s\n", on ? "on" : "off");
    } else {
        std::printf("Unknown action .%s (try .help)\n", cmd.c_str());
    }

    return true;
}

void HandleLine(SimAdapter& sim, const std::string& raw, bool& quit) {
    std::string line = Trim(raw);

    if (line.empty() || line[0] == '#')
        return;

    if (line[0] == '.') {
        HandleAction(sim, line, quit);

        return;
    }

    std::string out;
    Core& core = Core::Get();

    if (!core.OnConsoleCommand(RMM_SRC_CONSOLE, RMM_PLAYER_NONE, line, &out))
        sim.Exec(line, &out);

    if (!out.empty())
        std::printf("%s\n", out.c_str());

    std::fflush(stdout);
}

}  // namespace

int main(int argc, char** argv) {
    std::string root = plat::JoinPath(plat::CurrentDir(), "rmm");
    std::string script;
    rmm_game game = RMM_GAME_BF2;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];

        if (a == "--root" && i + 1 < argc)
            root = argv[++i];
        else if (a == "--script" && i + 1 < argc)
            script = argv[++i];
        else if (a == "--game" && i + 1 < argc) {
            std::string g = ToLower(argv[++i]);
            game = g == "bf1942"   ? RMM_GAME_BF1942
                   : g == "bfv"    ? RMM_GAME_BFV
                   : g == "bf2142" ? RMM_GAME_BF2142
                                   : RMM_GAME_BF2;
        } else {
            std::printf("usage: rmm_simhost [--root <dir>] [--game bf1942|bfv|bf2|bf2142] [--script <file>]\n");

            return 2;
        }
    }

    auto adapter = std::make_unique<SimAdapter>(game);
    SimAdapter* sim = adapter.get();
    std::string err;

    if (!Core::Get().Start(std::move(adapter), root, err)) {
        std::fprintf(stderr, "RMM failed to start: %s\n", err.c_str());

        return 1;
    }

    bool quit = false;

    if (!script.empty()) {
        std::ifstream f(script);

        if (!f) {
            std::fprintf(stderr, "cannot open %s\n", script.c_str());

            return 1;
        }

        // Scripts run without real time passing: deliver chat at once so output
        // stays in order. ".pacing on" + ".wait" tests the queue itself.
        Core::Get().Chat().SetBypass(true);
        std::string line;

        while (!quit && std::getline(f, line)) {
            std::printf("> %s\n", line.c_str());
            HandleLine(*sim, line, quit);
            Core::Get().Frame();
        }

        Core::Get().Shutdown();

        return 0;
    }

    std::printf("Refractor MultiMod simulator. Type .help for actions, .quit to exit.\n");
    std::thread reader(ReaderThread);
    reader.detach();

    while (!quit) {
        std::deque<std::string> batch;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            batch.swap(g_lines);
        }

        for (auto& l : batch) {
            HandleLine(*sim, l, quit);

            if (quit)
                break;
        }

        Core::Get().Frame();

        if (g_eof && batch.empty())
            break;

        std::this_thread::sleep_for(std::chrono::milliseconds(50));  // ~20 Hz, like a server tick
    }

    Core::Get().Shutdown();

    return 0;
}
