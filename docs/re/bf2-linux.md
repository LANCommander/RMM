# Battlefield 2 Linux server: reverse-engineering notes

Target: the BF2 1.5 Linux dedicated server, `bf2-linuxded-1.5.3153.0-installer.tgz`
(SHA-512 `b8076841…9db3702`, as pinned by
[tractr/battlefield-2-server](https://github.com/tractr/battlefield-2-server);
mirror `https://ftp.bf-games.net/server-files/bf2/`, which only works over
HTTPS since anonymous FTP returns 530).

The `.tgz` holds a Makeself 2.1.4 installer. Its payload is a gzip'd tar
starting after the first 375 lines (`offset=\`head -n 375 "$0" | wc -c\``),
330 901 002 bytes long, so it can be carved out and unpacked without running
the installer.

| Property | `bin/ia-32/bf2` | `bin/amd-64/bf2` |
|---|---|---|
| Format | ELF32 i386, non-PIE | ELF64 x86-64 |
| Symbols | full `.symtab`, 68 367 entries | full `.symtab`, 68 887 entries |
| DWARF | glibc startup files only (3 CUs); no engine types | same |
| Compiler | GCC 3.3.6 (engine), GCC 4.2.4 (glibc crt) | same |
| C++ runtime | `libstdc++.so.5` (COW `std::string`, as on BF1942) | same |
| Python | separate `libdice_py.so` (Python 2.3); `bf2` lists it as NEEDED and imports `Py_Initialize` from it | same |

The engine namespace is `dice::hfe` (BF1942's is `dice::ref2`). Classes have
RTTI on Linux (`_ZTV…`, `_ZTI…`, `_ZTS…`); the Windows `bf2_w32ded.exe` has no
symbols, no RTTI and no exports.

## Tooling

The maintainer's local setup; none of it is in the repository.

- Ghidra projects with full analysis:
  `C:\Development\Tools\ghidra-projects\bf2_lnxded` (ia-32; analysis took
  ~6.5 min) and `bf2_lnxded64` (amd-64). Query them like the BF1942 project
  (`RmmDecompile.java`, see `bf1942-linux.md`).
- Decompiled output: `C:\Development\Tools\re-work\bf2_lnx\decomp`.
- Python helpers in `C:\Development\Tools\re-work\scripts-pe`: `elfinfo.py`
  (symbol search), `elfvt.py` (dump a vtable with slot names, ELF32),
  `peinfo.py`, `makeself_carve.py`.

## Damage

`ServerGameLogic::giveDamage` (`08101db0`) is virtual and has no direct
callers. It:

1. returns unless the target (or a parent, via `+0x28`) has a component at
   `+0x3c` (armour) and the game is playing (`this+0x34 == 1`);
2. ignores damage below 0.1;
3. resolves the attacker: if `attackerObject` is a player-control object
   (`queryInterface(0xc4c5)`, slot 3), `pco->[slot 4]()` gives its `IPlayer*`,
   falling back to the armour's last attacker;
4. applies armour, `GameLogicDamage::calcDamage(target, damage, …)`;
5. calls `GameLogicDamage::serverGiveDamage` (`080dfff0`) with the final
   damage;
6. afterwards, handles team-damage scoring (Python event `0x20` =
   `PlayerTeamDamagePoint`).

`serverGiveDamage` is static (no `this`) and is only called from step 5, so
RMM hooks it. Signature from the mangled name:

```
void GameLogicDamage::serverGiveDamage(IObject* target, float damage, IPlayer* attacker,
     std::list<WeakPtr<IPlayer>>* assists, IObject* attackerObject, int, IObject*, IObject*,
     Vec3, Vec3, int, bool, bool, bool, bool)
```

`Vec3` has a non-trivial copy, so both are passed by hidden reference:
`giveDamage` copies its two vectors to locals and passes `&local_3c`,
`&local_2c`. The hook declares them as pointers and forwards them unchanged,
which is correct for both i386 and x86-64.

RMM's hook (`src/games/refractor2/r2_linux.cpp`):

- Victim: `queryInterface(target, 0xc4c5)` → `pco->[4]()` → `IPlayer*`. It's
  a soldier hit if that player's `getDefaultVehicle()` is the target, which
  fires `PLAYER_DAMAGE`.
- Vehicle hits: `getRootParent(target)`; if the root is a player-control
  object, fire `VEHICLE_DAMAGE`. The root and its occupants (players whose
  `getVehicle()` has the same root) are only computed when a plugin hooks the
  event.
- Both events are PRE, blockable (the original isn't called) and mutable
  (the damage argument).

VERIFY live: that the damage argument is what the soldier loses (no further
scaling inside `serverGiveDamage`), and that blocking it leaves no side
effects (e.g. hit indicators sent elsewhere).

## Players

`dice::hfe::world::playerManager` (`08b1b928`, size 4) is an `IPlayerManager*`.
The decompiled code calls `playerManager->[+0x20](id)`. `PlayerManager`
vtable (`088c8ac0`), slot indices (byte offset = index × pointer size):

| Index | ia-32 offset | Function |
|---|---|---|
| 8 | +0x20 | `getPlayerFromId(int)`: `std::map<int, IPlayer*>` at `this+0x2c` (header pointer at `+0x30`, count at `+0x34`, GCC 3.3 layout) |
| 9 | +0x24 | `getNextPlayerFromId(int)`: needs an existing id; wraps around |
| 15 | +0x3c | `getPlayers()` |

`IPlayer` is the primary base of `dice::hfe::Player` (`IPlayer::getWeakPtr`
is in the primary vtable), so an `IPlayer*` uses the `Player` vtable
(`0888fb00`) directly:

| Index | ia-32 offset | Function |
|---|---|---|
| 18 | +0x48 | `getVehicle() const` |
| 21 | +0x54 | `getId() const`: the Python `Player.index` (`host.pmgr_p_get(name, index)`) |
| 27 | +0x6c | `getIsAlive() const` (bool in `%al`) |
| 41 | +0xa4 | `getDefaultVehicle()`: the soldier |
| 58 | +0xe8 | `getTeam() const`: matches `giveDamage`'s team check |

## Objects

- IIDs have the same values as on BF1942: `IID_IObject` 0xc378,
  `IID_IPlayerControlObject` 0xc4c5, `IID_IPhysicsNode` 0xc422.
- `world::Object` vtable (`088c9660`): slot 3 `queryInterface`, 23
  `queryComponent(unsigned, unsigned)`, 25 `getName`, 27
  `getAbsolutePosition`, 28 `setAbsolutePosition(const Vec3&)`.
- `getRootParent(const IObject*)` (`0831b8b0`): if `flags & 1` (byte `+7`)
  the object is its own root; otherwise the result is cached at `+0xc` and
  computed by recursing on the parent at `+0x28`.
- Template: `Object::Object(const IObjectTemplate*)` stores it at
  `this+0x14` on ia-32 and `this+0x28` on amd-64 (`00689370`).
  `ObjectTemplate::getName() const` is template vtable slot 11 and returns a
  GCC 3 `std::string`.
- On amd-64, every slot index in `[linux_slots]` was checked against that
  build's vtables (Player 18/21/27/41/58, PlayerManager 8, ObjectTemplate 11,
  Object 3). The player-control `getPlayer` is `+0x20` = index 4 in the 64-bit
  `ServerGameLogic::giveDamage`. Field offsets double as expected
  (`getRootParent`: cache `+0x18`, parent `+0x50`).
- Physics: `PhysicsNode`, `PointPhysicsNode` and `StaticPhysicsNode` have
  `get/setPositionalSpeed` (plus `addPositionalSpeed`) by name. RMM doesn't
  use them; BF2 reports no object-velocity capability.

## Python glue

The host module's tables are in the binary; the names below are from
`bf2_w32ded.exe` and are identical on Linux:

- Event handler names: `PlayerConnect`, `PlayerDisconnect`, `PlayerSpawn`,
  `PlayerKilled`, `PlayerDeath`, `PlayerRevived`, `PlayerChangeTeams`,
  `PlayerChangedSquad`, `ChangedCommander`, `ChangedSquadLeader`,
  `PlayerScore`, `PickupKit`, `DropKit`, `PlayerChangeWeapon`, `EnterVehicle`,
  `ExitVehicle`, `VehicleDestroyed`, `ControlPointChangedOwner`,
  `PlayerTeamDamagePoint`, `PlayerGiveAmmoPoint`, `PlayerHealPoint`,
  `PlayerRepairPoint`, `TimeLimitReached`, `TicketLimitReached`, `BeginRound`,
  `EndRound`, `ServerReady`, `ChatMessage`, `RemoteCommand`, `ClientCommand`,
  `ConsoleSendCommand`, `PlayerKicked`, `PlayerBanned`, `DeployTactical`,
  `DeployZipLine`, `DeployGrapplingHook`, `SpottedMessageSent`,
  `ValidatePlayerNameResponse`, `Player{Stats,Awards,Unlocks}Response`, `Reset`.
  There is no damage event.
- `host.Object` methods: `getPosition`, `setPosition`, `getRotation`,
  `setRotation`, `getDamage`, `setDamage`, `getParent`, `getChildren`,
  `isValid`, `getOccupyingPlayers`, `getTeam`, `getIsWreck`,
  `getIsRemoteControlled`, `getTemplateProperty`, `isControlPoint`,
  `isPlayerControlObject`, `hasArmor`, plus `cp_getParam`/`cp_setParam`
  and the `templateName` attribute. There is no velocity.
- Every `host.Object` has `token`, a PyCObject wrapping the
  `dice::hfe::world::IObject*` ("error while extracting
  dice::hfe::world::IObject token"; "the specified object was deleted on
  the engine side (use isValid() to be sure)"). `libdice_py.so` exports
  `PyCObject_AsVoidPtr` and `PyCObject_Type`, so `_rmm.object_ptr(token)`
  returns the pointer. The bridge uses it as the object handle, so Python
  and native handles are the same value. VERIFY live that the token
  pointer and the hook's `getRootParent` result are the same `IObject*`
  for a vehicle root.
- The shipped `python/bf2/*.py` confirms the calls the bridge relies on:
  `objectManager.getObjectsOfType` (`omgr_getObjectsOfType`),
  `Player.getVehicle/getDefaultVehicle/getKit/getProfileId/getAddress/getSquadId`
  (`pmgr_p_get`), `gameLogic.getMapName/getModDir/getTickets/setTickets`,
  and `serverSettings.getGameMode/getMaxPlayers`.

## Not found yet

- Chat before broadcast. Chat currently reaches RMM through the Python
  `ChatMessage` event, after the fact, so it can't be blocked or changed.
  There's no `ChatMessage` symbol in the C++ code; start from the Python
  `ChatMessage` event dispatch (`PythonHost::handleEvent` callers).
- The connection handshake, for rejecting banned players before they load in:
  `GameServer::createPlayer(const std::string&, bool)`.
