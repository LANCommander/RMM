// basecommands - core admin toolkit (the RMM equivalent of SourceMod's
// basecommands/basebans/funcommands). Works on every game; commands whose
// capability is missing on the running game say so instead of failing silently.
//
// Commands and the admin flag each one requires:
//   !kick <target> [reason]          c   !ban <target> <minutes> [reason]   d
//   !unban <auth|ip|name> <value>    e   !slay <target>                     f
//   !slap <target> [damage] [launch] f
//   !team <target> <team>            f   !map <map> [mode] [size]           g
//   !restart                         g   !nextmap                           g
//   !maplist                         -   !say <msg>                         i
//   !csay <msg>                      i   !set <setting> [value]             h
//   !gravity [value]                 m   !bots <count> [skill]              h
//   !tickets [team] [value]          h   !who                               b
#include <rmm/rmm.hpp>

#include <cmath>
#include <cstdlib>
#include <cstring>

namespace {

const char* kSettingKeys[RMM_SETTING_COUNT] = {
    "server_name",     "password",     "max_players",   "time_limit",     "score_limit",    "ticket_ratio",
    "rounds_per_map",  "spawn_time",   "man_down_time", "friendly_fire",  "vehicle_ff",     "team_ratio",
    "autobalance",     "gravity",      "game_speed",    "bot_count",      "bot_ratio",      "bot_skill",
    "welcome_message", "voip_enabled", "punkbuster",    "allow_nose_cam", "external_views", "kick_tk_count",
};

int SettingFromName(const std::string& n) {
    for (int i = 0; i < RMM_SETTING_COUNT; ++i)
        if (n == kSettingKeys[i])
            return i;

    return -1;
}

bool RequireCap(const rmm::CmdArgs& a, rmm_cap cap, const char* what) {
    if (rmm::HasCap(cap))
        return true;

    a.Replyf("%s is not supported on %s.", what, rmm::Api().game->name());

    return false;
}

void Announce(const rmm::CmdArgs& a, const std::string& action) {
    rmm::SayAll("[ADMIN] " + rmm::PlayerName(a.Caller()) + " " + action);
    rmm::Info("%s %s", rmm::PlayerName(a.Caller()).c_str(), action.c_str());
}

}  // namespace

class BaseCommands : public rmm::Plugin {
    rmm::Var announce_;

public:
    bool OnLoad(bool, std::string&) override {
        announce_ =
            rmm::Var("basecommands.announce", "1", "Announce admin actions in chat", RMM_VAR_ARCHIVE | RMM_VAR_NUMERIC);
        auto announce = [this](const rmm::CmdArgs& a, const std::string& s) {
            if (announce_.Bool())
                Announce(a, s);
        };

        rmm::Command("kick", RMM_CMD_ALL, RMM_ADMFLAG_KICK, "Kick a player", "<target> [reason]",
                     [announce](rmm::CmdArgs& a) {
                         if (a.Count() < 1) {
                             a.Reply("Usage: kick <target> [reason]");

                             return;
                         }

                         std::string reason = a.Count() >= 2 ? a.From(2) : "Kicked by admin";

                         for (rmm_player_t p : rmm::Target(a, a.Arg(1), RMM_TARGET_ALLOW_MULTI)) {
                             std::string name = rmm::PlayerName(p);

                             if (rmm::Api().admin->kick(p, reason.c_str(), a.Caller()))
                                 announce(a, "kicked " + name + " (" + reason + ")");
                             else
                                 a.Reply("Could not kick " + name + ".");
                         }
                     });

        rmm::Command("ban", RMM_CMD_ALL, RMM_ADMFLAG_BAN, "Ban a player (0 minutes = permanent)",
                     "<target> <minutes> [reason]", [announce](rmm::CmdArgs& a) {
                         if (a.Count() < 2) {
                             a.Reply("Usage: ban <target> <minutes> [reason]");

                             return;
                         }

                         int minutes = std::atoi(a.Arg(2).c_str());
                         std::string reason = a.Count() >= 3 ? a.From(3) : "Banned by admin";

                         for (rmm_player_t p : rmm::Target(a, a.Arg(1))) {
                             std::string name = rmm::PlayerName(p);

                             if (rmm::Api().admin->ban(p, minutes, reason.c_str(), a.Caller()))
                                 announce(a,
                                          "banned " + name +
                                              (minutes ? " for " + std::to_string(minutes) + " min" : " permanently") +
                                              " (" + reason + ")");
                             else
                                 a.Reply("Could not ban " + name + ".");
                         }
                     });

        rmm::Command("unban", RMM_CMD_ALL, RMM_ADMFLAG_UNBAN, "Remove a ban", "<auth|ip|name> <value>",
                     [](rmm::CmdArgs& a) {
                         if (a.Count() < 2) {
                             a.Reply("Usage: unban <auth|ip|name> <value>");

                             return;
                         }

                         std::string k = a.Arg(1);
                         int kind = k == "ip" ? RMM_ID_IP : k == "name" ? RMM_ID_NAME : RMM_ID_AUTH;
                         a.Reply(rmm::Api().admin->unban(kind, a.Arg(2).c_str()) ? "Ban removed." : "No such ban.");
                     });

        rmm::Command("slay", RMM_CMD_ALL, RMM_ADMFLAG_SLAY, "Kill a player", "<target>", [announce](rmm::CmdArgs& a) {
            if (!RequireCap(a, RMM_CAP_KILL_PLAYER, "Slaying"))
                return;

            for (rmm_player_t p : rmm::Target(a, a.Arg(1), RMM_TARGET_ALLOW_MULTI | RMM_TARGET_ALIVE_ONLY))
                if (rmm::Api().players->kill(p))
                    announce(a, "slayed " + rmm::PlayerName(p));
        });

        // Launch: upward speed in m/s (the engine's own explosion knock-back
        // animation kicks in above 8 m/s), plus a random sideways shove of 40%
        // of that. A player in a vehicle launches the whole vehicle (everyone
        // in it goes up with them). Games without RMM_CAP_PLAYER_PUSH only
        // take the damage.
        rmm::Command(
            "slap", RMM_CMD_ALL, RMM_ADMFLAG_SLAY, "Damage a player and knock them into the air",
            "<target> [damage=10] [launch=8]", [announce](rmm::CmdArgs& a) {
                bool canDamage = rmm::HasCap(RMM_CAP_DAMAGE_PLAYER), canPush = rmm::HasCap(RMM_CAP_PLAYER_PUSH);

                if (!canDamage && !canPush) {
                    RequireCap(a, RMM_CAP_DAMAGE_PLAYER, "Slapping");

                    return;
                }

                if (a.Count() < 1) {
                    a.Reply("Usage: slap <target> [damage=10] [launch=8]");

                    return;
                }

                float dmg = a.Count() >= 2 ? static_cast<float>(std::atof(a.Arg(2).c_str())) : 10.0f;
                float launch = a.Count() >= 3 ? static_cast<float>(std::atof(a.Arg(3).c_str())) : 8.0f;

                if (!(dmg >= 0.0f) || !(launch >= 0.0f) || launch > 100.0f) {
                    a.Reply("Damage must be >= 0 and launch between 0 and 100 m/s.");

                    return;
                }

                if (dmg == 0.0f && (launch == 0.0f || !canPush)) {
                    a.Reply("Nothing to do: damage and launch are both 0.");

                    return;
                }

                for (rmm_player_t p : rmm::Target(a, a.Arg(1), RMM_TARGET_ALLOW_MULTI | RMM_TARGET_ALIVE_ONLY)) {
                    std::string name = rmm::PlayerName(p);
                    bool pushed = false, damaged = false;

                    // Push first: lethal damage would leave nothing to launch.
                    if (canPush && launch > 0.0f) {
                        float angle = static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX) * 6.2831853f;
                        float side = launch * 0.4f;
                        pushed = rmm::PushPlayer(p, rmm_vec3{std::cos(angle) * side, launch, std::sin(angle) * side});
                    }

                    if (canDamage && dmg > 0.0f)
                        damaged = rmm::DamagePlayer(p, dmg, a.Caller());

                    if (!pushed && !damaged) {
                        a.Reply("Could not slap " + name + ".");
                        continue;
                    }

                    std::string what =
                        damaged ? rmm::Format("slapped %s for %.0f", name.c_str(), dmg) : "slapped " + name;
                    announce(a, pushed ? what + " (launched)" : what);

                    if (damaged)
                        if (auto hp = rmm::GetHealth(p))
                            a.Replyf("%s has %.0f health left.", name.c_str(), hp->current);
                }
            });

        rmm::Command("team", RMM_CMD_ALL, RMM_ADMFLAG_SLAY, "Move a player to a team", "<target> <1|2>",
                     [announce](rmm::CmdArgs& a) {
                         if (!RequireCap(a, RMM_CAP_SET_TEAM, "Changing teams"))
                             return;

                         int team = std::atoi(a.Arg(2).c_str());

                         if (team != 1 && team != 2) {
                             a.Reply("Team must be 1 or 2.");

                             return;
                         }

                         for (rmm_player_t p : rmm::Target(a, a.Arg(1), RMM_TARGET_ALLOW_MULTI))
                             if (rmm::Api().players->set_team(p, team))
                                 announce(a, "moved " + rmm::PlayerName(p) + " to team " + std::to_string(team));
                     });

        rmm::Command("map", RMM_CMD_ALL, RMM_ADMFLAG_CHANGEMAP, "Change map now", "<map> [mode] [size]",
                     [announce](rmm::CmdArgs& a) {
                         if (!RequireCap(a, RMM_CAP_CHANGE_MAP, "Changing map"))
                             return;

                         if (a.Count() < 1) {
                             a.Reply("Usage: map <map> [mode] [size]");

                             return;
                         }

                         std::string map = a.Arg(1), mode = a.Arg(2);

                         if (rmm::Api().game->change_map(map.c_str(), mode.empty() ? nullptr : mode.c_str(),
                                                         std::atoi(a.Arg(3).c_str())))
                             announce(a, "changed map to " + map);
                         else
                             a.Reply("Map change failed.");
                     });

        rmm::Command("restart", RMM_CMD_ALL, RMM_ADMFLAG_CHANGEMAP, "Restart the current map", "",
                     [announce](rmm::CmdArgs& a) {
                         if (rmm::Api().game->restart_map())
                             announce(a, "restarted the map");
                         else
                             a.Reply("Restart not supported.");
                     });

        rmm::Command("nextmap", RMM_CMD_ALL, RMM_ADMFLAG_CHANGEMAP, "Run the next map in the rotation", "",
                     [announce](rmm::CmdArgs& a) {
                         if (rmm::Api().game->run_next_map())
                             announce(a, "skipped to the next map");
                         else
                             a.Reply("Not supported.");
                     });

        rmm::Command("maplist", RMM_CMD_ALL, 0, "Show the map rotation", "", [](rmm::CmdArgs& a) {
            if (!RequireCap(a, RMM_CAP_MAPLIST, "The maplist"))
                return;

            int n = rmm::Api().game->maplist_count();

            for (int i = 0; i < n; ++i) {
                rmm_map_info m{};
                m.struct_size = sizeof m;

                if (rmm::Api().game->maplist_get(i, &m))
                    a.Replyf("%d: %s %s %d", i, m.map, m.mode, m.size);
            }
        });

        rmm::Command("say", RMM_CMD_ALL, RMM_ADMFLAG_CHAT, "Admin message to all", "<message>",
                     [](rmm::CmdArgs& a) { rmm::SayAll("[ADMIN] " + a.All()); });

        rmm::Command("csay", RMM_CMD_ALL, RMM_ADMFLAG_CHAT, "Centre-screen admin message", "<message>",
                     [](rmm::CmdArgs& a) {
                         if (!rmm::Api().game->center_message(a.All().c_str()))
                             rmm::SayAll("[ADMIN] " + a.All());
                     });

        rmm::Command("set", RMM_CMD_ALL, RMM_ADMFLAG_CONFIG, "Get/set a portable server setting", "<setting> [value]",
                     [announce](rmm::CmdArgs& a) {
                         int s = SettingFromName(a.Arg(1));

                         if (s < 0) {
                             std::string all;

                             for (int i = 0; i < RMM_SETTING_COUNT; ++i)
                                 if (rmm::Api().vars->setting_var(i))
                                     all += std::string(kSettingKeys[i]) + " ";

                             a.Reply("Settings on this game: " + all);

                             return;
                         }

                         const char* var = rmm::Api().vars->setting_var(s);

                         if (!var) {
                             a.Replyf("'%s' is not available on %s.", kSettingKeys[s], rmm::Api().game->name());

                             return;
                         }

                         if (a.Count() < 2) {
                             auto v = rmm::GetSetting(static_cast<rmm_setting>(s));
                             a.Replyf("%s (%s) = %s", kSettingKeys[s], var, v ? v->c_str() : "?");

                             return;
                         }

                         if (s == RMM_SETTING_PASSWORD && !rmm::HasFlags(a.Caller(), RMM_ADMFLAG_PASSWORD)) {
                             a.Reply("Need password access.");

                             return;
                         }

                         if (rmm::SetSetting(static_cast<rmm_setting>(s), a.From(2)))
                             announce(a, std::string("set ") + kSettingKeys[s] + " to " +
                                             (s == RMM_SETTING_PASSWORD ? "***" : a.From(2)));
                         else
                             a.Reply("Failed to set value.");
                     });

        rmm::Command("gravity", RMM_CMD_ALL, RMM_ADMFLAG_CHEATS, "Get/set gravity", "[value]",
                     [announce](rmm::CmdArgs& a) {
                         if (!rmm::Api().vars->setting_var(RMM_SETTING_GRAVITY)) {
                             a.Reply("Gravity is not controllable on this game.");

                             return;
                         }

                         if (a.Count() < 1) {
                             auto g = rmm::GetSetting(RMM_SETTING_GRAVITY);
                             a.Reply("Gravity = " + g.value_or("?"));

                             return;
                         }

                         if (rmm::SetSetting(RMM_SETTING_GRAVITY, a.Arg(1)))
                             announce(a, "set gravity to " + a.Arg(1));
                     });

        rmm::Command("bots", RMM_CMD_ALL, RMM_ADMFLAG_CONFIG, "Set bot count/ratio and skill", "<count|ratio%> [skill]",
                     [announce](rmm::CmdArgs& a) {
                         if (!RequireCap(a, RMM_CAP_BOTS, "Bot control"))
                             return;

                         if (a.Count() < 1) {
                             a.Reply("Usage: bots <count|ratio%> [skill]");

                             return;
                         }

                         std::string v = a.Arg(1);
                         bool ratio = !v.empty() && v.back() == '%';

                         if (ratio)
                             v.pop_back();

                         rmm_setting s = ratio ? RMM_SETTING_BOT_RATIO : RMM_SETTING_BOT_COUNT;

                         if (!rmm::Api().vars->setting_var(s))
                             s = ratio ? RMM_SETTING_BOT_COUNT : RMM_SETTING_BOT_RATIO;

                         bool ok = rmm::SetSetting(s, v);

                         if (a.Count() >= 2)
                             ok = rmm::SetSetting(RMM_SETTING_BOT_SKILL, a.Arg(2)) && ok;

                         if (ok)
                             announce(a, "changed bot settings (" + a.All() + "); applies next round on most games");
                         else
                             a.Reply("Failed to change bot settings.");
                     });

        rmm::Command("tickets", RMM_CMD_ALL, RMM_ADMFLAG_CONFIG, "Show or set team tickets", "[team] [value]",
                     [announce](rmm::CmdArgs& a) {
                         if (!RequireCap(a, RMM_CAP_TICKETS, "Ticket control"))
                             return;

                         if (a.Count() < 2) {
                             a.Replyf("Tickets: team 1 = %d, team 2 = %d", rmm::Api().game->get_tickets(1),
                                      rmm::Api().game->get_tickets(2));

                             return;
                         }

                         int team = std::atoi(a.Arg(1).c_str()), value = std::atoi(a.Arg(2).c_str());

                         if (rmm::Api().game->set_tickets(team, value))
                             announce(a, "set team " + a.Arg(1) + " tickets to " + a.Arg(2));
                     });

        rmm::Command(
            "who", RMM_CMD_ALL, RMM_ADMFLAG_GENERIC, "List players with ids and admin flags", "", [](rmm::CmdArgs& a) {
                for (rmm_player_t p : rmm::Players()) {
                    auto info = rmm::GetPlayer(p);

                    if (!info)
                        continue;

                    uint32_t f = rmm::Api().admin->get_flags(p);
                    std::string flags;

                    for (int i = 0; i < 26; ++i)
                        if (f & (1u << i))
                            flags += static_cast<char>('a' + i);

                    a.Replyf("#%d %s team %d score %d auth %s %s", p, info->name, info->team, info->score,
                             info->auth_id[0] ? info->auth_id : "-", flags.empty() ? "" : ("[" + flags + "]").c_str());
                }
            });

        return true;
    }
};

RMM_DECLARE_PLUGIN(BaseCommands, "Basic Commands", "1.0.0", "RMM Team", "Kick/ban/map/settings/bots admin commands", "",
                   "basecmds", RMM_GAME_ANY, 0)
