# BF1942 kits, weapons and vehicles

Template names for plugins that match on weapons, kits or vehicles, such as gun game's ladders and its `weapons=` and `gungame.knifeWeapons` settings. The lists come from the 1.61 server's data files (`mods/<mod>/archives/objects.rfa` and each level's `.con` files) for the base game and both expansions. Template names are case-insensitive, both in the engine and in RMM's patterns.

## The weapon reported for a kill

`rmm_ev_player_killed::weapon` and `rmm_ev_player_damage::weapon` hold the template the projectile was fired from. `FireArms::fireBarrel` stores that template's id in every projectile it fires, and the engine passes it on through the damage and scoring code (see "Kill weapon names" in [re/bf1942-linux.md](re/bf1942-linux.md)).

- **On foot**, the weapon is the hand weapon's template, such as `Bar1918`, `Panzershreck`, `KnifeAxis` or `GrenadeAllies`, not the projectile (`barProjectile`), which several weapons share.
- **In a vehicle or stationary gun**, the weapon is the vehicle's root template, such as `T34`, `Hanomag` or `Stationary_mg42`. A tank's cannon and its coaxial MG both report `T34`; there is no way to tell which gun fired.
- **Bot kills** report no weapon: the engine itself passes no weapon id for them.

Not yet checked on a live server: roadkills, mines, explosive packs and artillery. Running the `eventmonitor` plugin and making one kill of each kind will show what they report.

## Hand weapons

Weapons marked † share their projectile with another weapon. The weapon name is what gets reported, so this doesn't affect `weapons=`.

| Template | Weapon | Slot | Carried by (base game) |
|---|---|---|---|
| `KnifeAllies` | knife (Allied) | 1 | every Allied kit |
| `KnifeAxis` | knife (Axis) | 1 | every Axis kit |
| `Colt` | Colt M1911 pistol | 2 | every Allied kit |
| `WalterP38` | Walther P38 pistol | 2 | every Axis kit |
| `Bar1918` | BAR 1918 † | 3 | US, US Marine and GB Assault |
| `JohnsonLMG` | Johnson LMG † | 3 | Canadian Assault |
| `DP` | DP-28 LMG | 3 | Rus Assault |
| `Type99` | Type 99 LMG † | 3 | Jap Assault |
| `Sg44` | StG 44 | 3 | German Assault, German Assault Desert |
| `Thompson` | Thompson SMG | 3 | US, US Marine, GB and Canadian Medic |
| `Mp40` | MP 40 | 3 | German Medic, German Medic Desert |
| `Mp18` | SMG, shown with the Japanese MG icon † | 3 | Rus Medic, Jap Medic |
| `M1Garand` | M1 Garand † | 3 | US Marine Engineer |
| `No4` | Lee-Enfield No.4 † | 3 | US, Rus, GB and Canadian Engineer |
| `K98` | Kar98k † | 3 | German Engineer, German Engineer Desert |
| `Type5` | Type 5 rifle † | 3 | Jap Engineer |
| `No4Sniper` | No.4 with scope † | 3 | every Allied Scout |
| `K98Sniper` | Kar98k with scope † | 3 | every Axis Scout |
| `Bazooka` | Bazooka † | 3 | US, US Marine, Rus, GB and Canadian AT |
| `Panzershreck` | Panzerschreck † | 3 | German, German Desert and Jap AT |
| `GrenadeAllies` | grenade (Allied) | 4 | every Allied kit except Engineers |
| `GrenadeAxis` | grenade (Axis) | 4 | every Axis kit except Engineers |
| `ExpPack` | explosive pack (set off with `Detonator`) | 4 | every Engineer |
| `Landmine` | AT mine | 5 | every Engineer |
| `Binoculars` | binoculars (artillery spotting) | 5 | every Scout |
| `MedPack`, `RepairPack`, `Detonator` | tools, no projectile | | Medics, Engineers |

Expansion hand weapons:

| Template | Mod | Weapon | Notes |
|---|---|---|---|
| `Breda` | xpack1 | Breda 30 LMG † | Italian Assault |
| `Stengun` | xpack1, xpack2 | Sten SMG † | French, It_GB and Commando Medic |
| `K98Bayonet`, `No4Bayonet` | xpack1 | bayonet (melee, slot 1) | Road to Rome Engineers carry these instead of a knife |
| `Gewehr42` | xpack2 | FG 42 † | GermanElite Assault |
| `Gewehr43_zf4` | xpack2 | Gewehr 43 with scope † | GermanElite Scout |
| `K98RifleGrenade` | xpack2 | rifle grenade | GermanElite Engineer |
| `BrenLMG` | xpack2 | Bren † | Commando Assault |
| `Shotgun` | xpack2 | shotgun | Commando Engineer |
| `EliteKnife`, `CommandoKnife` | xpack2 | throwing knives | GermanElite and Commando kits |

The default `gungame.knifeWeapons` is `*knife*,*bayonet*`, which matches every knife and bayonet above.

## Kits (base game, 40)

`set_kit` can hand out any kit on any map, whatever the map's teams. Three US kits are created with unusual capitalisation (`Us_Assault`, `Us_AT`, `UsMarine_Assault`), but lookups ignore case, so `US_Assault` works.

| Set | Scout | Assault | AT | Medic | Engineer |
|---|---|---|---|---|---|
| `German_*` | K98Sniper | Sg44 | Panzershreck | Mp40 | K98 |
| `German_*_Desert` | K98Sniper | Sg44 | Panzershreck | Mp40 | K98 |
| `Jap_*` | K98Sniper | Type99 | Panzershreck | Mp18 | Type5 |
| `US_*` | No4Sniper | Bar1918 | Bazooka | Thompson | No4 |
| `USMarine_*` | No4Sniper | Bar1918 | Bazooka | Thompson | M1Garand |
| `GB_*` | No4Sniper | Bar1918 | Bazooka | Thompson | No4 |
| `Canadian_*` | No4Sniper | JohnsonLMG | Bazooka | Thompson | No4 |
| `Rus_*` | No4Sniper | DP | Bazooka | Mp18 | No4 |

The Desert set's names put the class in the middle, e.g. `German_Scout_Desert`.

Every kit also carries a knife and its side's pistol (Allied: `KnifeAllies`, `Colt`; Axis: `KnifeAxis`, `WalterP38`), plus:
- Scouts: grenades and `Binoculars`;
- Assault, AT and Medic: grenades (Medics also get a `MedPack`);
- Engineers: no grenades; `RepairPack`, `Landmine`, `ExpPack` and `Detonator`.

Expansion kits use the same five classes:
- xpack1: `Italian_*`, `French_*`, `It_GB_Engineer`, `It_GB_Medic`, `It_US_Engineer`, `It_German_Engineer`.
- xpack2: `GermanElite_*` (with `GermanElite_JetPack`), `Commando_*`.

## Vehicles and their guns

Templates are the vehicle's root template: the name used by `rmm spawn`, `rmm objects` and gun game's `vehicle` lines, and the name every gun on the vehicle reports for a kill.

Seats are numbered from 0, the driver (F1); seat 1 is F2, and so on. Seats with no gun listed have none. The numbers follow the order of each vehicle's `PlayerControlObject` parts. On live servers they matched the F keys in tanks, but check with `rmm objects` before relying on them.

In tanks the driver also fires the main gun. In half-tracks the driver can't shoot, so a gun game level for one needs `seat=1`, which makes the half-track a fixed MG nest. With `gungame.vehicleSwitch 1` the player can still switch between driving and the gun.

**Land, base game:**

| Template | Vehicle | Seats and guns |
|---|---|---|
| `Willy` | Willys jeep (not "Willys") | 0 driver, 1 passenger; no guns (roadkills only) |
| `Kubelwagen` | Kübelwagen | 0 driver, 1 passenger; no guns |
| `Lynx`, `BlackMedal` | scout cars | 0 driver (horn), 1 passenger; no guns |
| `M3A1` | M3A1 half-track | 0 driver, **1 Browning**, 2-5 passengers |
| `Hanomag` | Sd.Kfz. 251 half-track | 0 driver, **1 MG42**, 2-5 passengers |
| `Ho-Ha` | Type 1 Ho-Ha half-track | 0 driver, **1 MG42**, **2 MG42**, 3-5 passengers |
| `KettenKrad` | Kettenkrad | 0 driver, **1 MG42** |
| `PanzerIV` | Panzer IV | **0 cannon + coax MG42**, 1 MG42 |
| `Tiger` | Tiger | **0 cannon + coax MG42** |
| `T34` | T-34 | **0 cannon + coax MG42** |
| `T34-85` | T-34/85 | **0 cannon + coax MG42**, 1 MG42 |
| `Sherman` | Sherman | **0 cannon + coax Browning**, 1 Browning |
| `M10` | M10 tank destroyer | **0 cannon + coax Browning** |
| `Chi-ha` | Type 97 Chi-Ha | **0 cannon + coax MG42**, 1 MG42 |
| `Priest`, `Wespe` | self-propelled artillery | 0 driver, **1 cannon** |
| `Sexton` | self-propelled artillery | **0 cannon** |
| `Katyusha` | rocket truck | 0 driver, **1 rockets** |
| `Defgun` | stationary AT gun | **0 cannon** |
| `flak38`, `AA_Allies` | AA guns | **0 AA gun** |
| `Stationary_mg42`, `Stationary_Browning` | stationary MGs | **0 MG** |

**Air, base game:**
- `BF109`, `Spitfire`, `Mustang`, `Yak9`, `Zero` and `Corsair` (fighters with a bomb);
- `Stuka`, `Ilyushin`, `AichiVal`, `Aichival-T` and `SBD`, `SBD-T` (dive bombers and torpedo bombers, with a rear gunner);
- `B17`.

**Sea, base game:**
- `Lcvp`, `Daihatsu`, `Elco80`, `Type38`;
- `Fletcher`, `Fletcher2`, `Hatsuzuki`, `Hatsuzuki2`, `PrinceOW`, `Yamato`;
- `Gato`, `Sub7C`;
- `Enterprise`, `Shokaku` (carriers).

**xpack1:** `M11-39`, `M3Grant`, `M3GMC`, `SturmGeschutz`, `AT25`, `Pak40`, `BF110`, `Mosquito`, `ItLcvp`.

**xpack2:**
- `SturmTiger`, `T95`, `Sherman_T34` (Calliope), `FlakPanzer`, `HD_XA42`, `R75`, `Schwimmwagen`, `LVT4`, `Wasserfall`;
- `Goblin`, `HO229`, `Natter`, `AW52`, `C47`, `Jetpack`, `Raft`.

## Which maps have which vehicles (Conquest, base game)

The vehicles named by each map's spawners and placed objects. Gun game skips ladder levels whose template the running map hasn't loaded. A map may load more templates than it spawns (`rmm find <name>` on a running server will tell), so read this table as the vehicles that are certain to be available. Every map except Kursk has `Stationary_mg42` or `Stationary_Browning`, and almost every map has `Willy`, `Kubelwagen`, `M3A1` and `Hanomag`.

| Map | Tanks and armour | Others |
|---|---|---|
| aberdeen, battleaxe, el_alamein, gazala, tobruk | PanzerIV, Tiger, Sherman, M10 | Priest, Wespe, Defgun (aberdeen) |
| battle_of_the_bulge, bocage, market_garden, omaha_beach | PanzerIV, Tiger, Sherman, M10 | Priest, Wespe |
| berlin, kharkov, kursk, stalingrad | PanzerIV, Tiger, T34, T34-85 | Katyusha, Wespe |
| liberation_of_caen | PanzerIV, Sherman | Sexton, Wespe, Pak40, KettenKrad, Lynx (no jeeps) |
| battle_of_britain | none | half-tracks and jeeps only |
| guadalcanal, iwo_jima, midway | Sherman, Chi-ha | Priest, Wespe, Defgun (iwo_jima) |
| wake | Sherman, Chi-ha | Defgun |
| invasion_of_the_philippines | Sherman, Chi-ha | Ho-Ha, BlackMedal (no Kubelwagen or Hanomag) |
| coral_sea | none (carriers and planes) | |

xpack1 maps use a mix of SturmGeschutz, PanzerIV, M3Grant, M10, M11-39 and Pak40. xpack2 maps use a mix of SturmTiger, T95, Sherman_T34, FlakPanzer, PanzerIV, Tiger and Sherman.
