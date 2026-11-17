// apitest - test fixture for the game-mode calls (not shipped: built
// into <build>/testplugins and loaded by tests/gamemode.rmmsim only).
//
//   apitest.blockPickup   kit template (or *) whose pickups from the ground are blocked
//   apitest.blockVehicles vehicle template (or *) players are kept out of
//   apitest.spawnKit      kit given with set_kit to everyone who spawns (the gun game pattern)
//   apitest.blockExit     vehicle template (or *) players can't leave (use key or seat switch)
// "none" switches each one off. (Values can survive between runs: a plugin's
// changed vars are kept as pending values when it unloads, and saved.)
//
// Every kit / round event is logged as "apitest: ..." so scripts can check it.
#include <rmm/rmm.hpp>

#include <algorithm>
#include <cctype>

namespace {

const char* Reason(int32_t r) {
    switch (r) {
        case RMM_KIT_REASON_PICKUP:
            return "pickup";
        case RMM_KIT_REASON_SPAWN:
            return "spawn";
        case RMM_KIT_REASON_CHANGE:
            return "change";
        case RMM_KIT_REASON_PLUGIN:
            return "plugin";
        default:
            return "other";
    }
}

const char* VehReason(int32_t r) {
    switch (r) {
        case RMM_VEHICLE_REASON_PLAYER:
            return "player";
        case RMM_VEHICLE_REASON_SWITCH:
            return "switch";
        case RMM_VEHICLE_REASON_SPAWN:
            return "spawn";
        case RMM_VEHICLE_REASON_GAME:
            return "game";
        default:
            return "plugin";
    }
}

bool Matches(const std::string& rule, const char* name) {
    if (rule.empty() || !name)
        return false;

    if (rule == "*")
        return true;

    std::string a = rule, b = name;
    auto lower = [](std::string& s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    };

    lower(a);
    lower(b);

    return a == b;
}

}  // namespace

class ApiTest : public rmm::Plugin {
public:
    bool OnLoad(bool, std::string&) override {
        blockPickup_ = rmm::Var("apitest.blockPickup", "", "Block ground pickups of this kit (* = all)");
        blockVehicles_ = rmm::Var("apitest.blockVehicles", "", "Keep players out of this vehicle (* = all)");
        spawnKit_ = rmm::Var("apitest.spawnKit", "", "set_kit this kit on every spawn");
        blockExit_ = rmm::Var("apitest.blockExit", "",
                              "Keep players in this vehicle: no use-key exits or seat switches (* = all)");

        rmm::On<rmm_ev_player_kit>(
            RMM_EV_PLAYER_KIT_PICKUP,
            [this](rmm_ev_player_kit& e) -> rmm_result {
                if (e.reason == RMM_KIT_REASON_PICKUP && Matches(blockPickup_.Str(), e.kit)) {
                    rmm::Info("apitest: blocking pickup of %s by #%d", e.kit, e.player);

                    return RMM_HANDLED;
                }

                return RMM_CONTINUE;
            },
            RMM_PHASE_PRE);
        rmm::On<rmm_ev_player_kit>(RMM_EV_PLAYER_KIT_PICKUP, [](rmm_ev_player_kit& e) {
            rmm::Info("apitest: kit_pickup #%d %s (%s)", e.player, e.kit, Reason(e.reason));

            return RMM_CONTINUE;
        });
        rmm::On<rmm_ev_player_kit>(RMM_EV_PLAYER_KIT_DROP, [](rmm_ev_player_kit& e) {
            rmm::Info("apitest: kit_drop #%d %s (%s)", e.player, e.kit, Reason(e.reason));

            return RMM_CONTINUE;
        });
        rmm::On<rmm_ev_vehicle>(
            RMM_EV_VEHICLE_ENTER,
            [this](rmm_ev_vehicle& e) -> rmm_result {
                if (Matches(blockVehicles_.Str(), e.root_template)) {
                    rmm::Info("apitest: keeping #%d out of %s", e.player, e.root_template);

                    return RMM_HANDLED;
                }

                return RMM_CONTINUE;
            },
            RMM_PHASE_PRE);
        rmm::On<rmm_ev_vehicle>(RMM_EV_VEHICLE_ENTER, [](rmm_ev_vehicle& e) {
            auto p = rmm::GetPlayer(e.player);
            rmm::Info("apitest: entered #%d %s seat %d %s (player now in %s seat %d)", e.player, e.vehicle_template,
                      e.seat, VehReason(e.reason), p && p->vehicle ? p->vehicle_template : "nothing", p ? p->seat : -2);

            return RMM_CONTINUE;
        });
        rmm::On<rmm_ev_vehicle>(
            RMM_EV_VEHICLE_EXIT,
            [this](rmm_ev_vehicle& e) -> rmm_result {
                if ((e.reason == RMM_VEHICLE_REASON_PLAYER || e.reason == RMM_VEHICLE_REASON_SWITCH) &&
                    Matches(blockExit_.Str(), e.root_template)) {
                    rmm::Info("apitest: keeping #%d in %s (%s)", e.player, e.root_template, VehReason(e.reason));

                    return RMM_HANDLED;
                }

                return RMM_CONTINUE;
            },
            RMM_PHASE_PRE);
        rmm::On<rmm_ev_vehicle>(RMM_EV_VEHICLE_EXIT, [](rmm_ev_vehicle& e) {
            auto p = rmm::GetPlayer(e.player);
            rmm::Info("apitest: exited #%d %s seat %d %s (player now in %s)", e.player, e.vehicle_template, e.seat,
                      VehReason(e.reason), p && p->vehicle ? p->vehicle_template : "nothing");

            return RMM_CONTINUE;
        });
        rmm::On<rmm_ev_player_spawn>(RMM_EV_PLAYER_SPAWN, [this](rmm_ev_player_spawn& e) {
            std::string kit = spawnKit_.Str();

            if (!kit.empty() && kit != "none")
                rmm::Info("apitest: spawn kit for #%d: %s", e.player,
                          rmm::SetKit(e.player, kit) ? kit.c_str() : "FAILED");

            return RMM_CONTINUE;
        });
        rmm::On<rmm_ev_round_end>(RMM_EV_ROUND_END, [](rmm_ev_round_end& e) {
            rmm::Info("apitest: round_end winner %d reason %d", e.winner, e.reason);

            return RMM_CONTINUE;
        });

        return true;
    }

private:
    rmm::Var blockPickup_, blockVehicles_, spawnKit_, blockExit_;
};

RMM_DECLARE_PLUGIN(ApiTest, "API Test Fixture", "1.0.0", "RMM Team", "Game-mode test hooks", "", "apitest",
                   RMM_GAME_ANY, 0)
