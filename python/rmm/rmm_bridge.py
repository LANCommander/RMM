# Refractor MultiMod - Python bridge for Battlefield 2 / Battlefield 2142.
#
# Runs inside the server's embedded Python 2.3. Registers host event handlers
# and forwards them to the native RMM core through the `_rmm` builtin module,
# and provides helpers the native side calls back into.
#
# Install: add these two lines to the end of admin/default.py's init():
#
#     import rmm_bridge
#     rmm_bridge.init()
#
# (rmm/python is put on sys.path automatically by RMM.)
#
# !! Python 2.3: no decorators, no "x if c else y", no `with`, no sets/sorted.
#
# NOTE: names of host handlers and bf2 object methods below follow the BF2
# server Python API as commonly used by admin scripts. Anything marked VERIFY
# must be confirmed against the shipped python/bf2 package of each game.

import time

import host
import bf2

try:
    import _rmm
except ImportError:
    _rmm = None

# Mirrors include/rmm/rmm_events.h
EV_PLAYER_TEAM = 26
EV_PLAYER_SQUAD = 27
EV_PLAYER_KIT_PICKUP = 29
EV_PLAYER_KIT_DROP = 30
EV_VEHICLE_ENTER = 60
EV_VEHICLE_EXIT = 61

# rmm_game_status
ST_UNKNOWN, ST_PREGAME, ST_PLAYING, ST_ENDGAME, ST_PAUSED, ST_LOADING = 0, 1, 2, 3, 4, 5
# rmm_chat_channel
CH_ALL, CH_TEAM, CH_SQUAD, CH_COMMANDER, CH_SERVER = 0, 1, 2, 3, 4
# rmm_cmd_source
SRC_RCON, SRC_INGAME = 1, 2

FRAME_INTERVAL = 0.1   # bf2.Timer resolution used as the RMM "frame"

_initialised = 0
_timer = None
_status = ST_UNKNOWN
_teams = {}            # player index -> last known team
_pending_auth = {}     # player index -> time first seen, while waiting for a profile id
AUTH_TIMEOUT = 20.0    # seconds to wait for a profile id before authing without one


def _safe(fn):
    """Wrap a handler so an exception never escapes into the engine."""
    def wrapper(*args):
        try:
            return apply(fn, args)
        except:
            import traceback
            traceback.print_exc()
    return wrapper


def _name(obj):
    try:
        return obj.templateName
    except:
        return ""


def _index(player):
    try:
        return player.index
    except:
        return -1


def _player(pid):
    try:
        return bf2.playerManager.getPlayerByIndex(pid)
    except:
        return None


def _pos(obj):
    try:
        p = obj.getPosition()
        return float(p[0]), float(p[1]), float(p[2])
    except:
        return 0.0, 0.0, 0.0


# ---------------------------------------------------------------------------
# Object handles
# ---------------------------------------------------------------------------
# Every host.Object carries `token`, a PyCObject wrapping the engine's
# dice::hfe::world::IObject* (confirmed in bf2_w32ded.exe / bf2 Linux). That
# pointer is the handle: stable for the object's lifetime, identical across
# Python wrappers, and equal to what RMM's native hooks report.
#
# Fallback when a token can't be read (no _rmm.object_ptr): small
# counter handles, matching new wrappers against known objects with == and
# remembering them as aliases. Each wrapper is kept referenced so its id()
# can't be reused while it is in the table. Everything is dropped on map load.

_objects = {}         # handle -> object
_aliases = {}         # id(wrapper) -> (handle, wrapper)
_next_handle = [1]


def _reset_objects():
    _objects.clear()
    _aliases.clear()


def _token_ptr(obj):
    try:
        return _rmm.object_ptr(obj.token)
    except:
        return None


def _handle(obj):
    if obj is None:
        return 0
    h = _token_ptr(obj)
    if h:
        _objects[h] = obj   # keep the newest wrapper for later calls
        return h
    hit = _aliases.get(id(obj))
    if hit is not None:
        return hit[0]
    h = 0
    for k, o in _objects.items():
        try:
            if o == obj:
                h = k
                break
        except:
            pass
    if not h:
        h = _next_handle[0]
        _next_handle[0] = h + 1
        if _next_handle[0] > 0x7ffffff0:
            _next_handle[0] = 1
        _objects[h] = obj
    _aliases[id(obj)] = (h, obj)
    return h


def _object(h):
    obj = _objects.get(h)
    if obj is None:
        # A handle from a native hook for a vehicle Python hasn't seen yet:
        # registering every vehicle (see vehicles()) picks it up.
        vehicles()
        obj = _objects.get(h)
    if obj is None:
        return None
    try:
        if not obj.isValid():   # VERIFY: PhysicalObject.isValid()
            del _objects[h]
            return None
    except AttributeError:
        pass
    except:
        return None
    return obj


def _root(obj):
    """Top-level parent of an object (vehicle root for a seat)."""
    node = obj
    for i in range(32):
        try:
            parent = node.getParent()   # VERIFY
        except:
            return node
        if parent is None:
            return node
        node = parent
    return node


def _vehicle_of(p):
    """The object a player controls, or None when on foot/dead."""
    try:
        veh = p.getVehicle()
        if veh is not None and veh != p.getDefaultVehicle():
            return veh
    except:
        pass
    return None


# ---------------------------------------------------------------------------
# Called from native code (R2Adapter)
# ---------------------------------------------------------------------------

def log(msg):
    print msg


def exec_(cmd):
    return str(host.rcon_invoke(cmd))

# `exec` is a keyword in Python 2, so expose it via globals().
globals()['exec'] = exec_


def max_players():
    try:
        return int(bf2.serverSettings.getMaxPlayers())   # VERIFY
    except:
        return 64


def player_ids():
    ids = []
    for p in bf2.playerManager.getPlayers():
        ids.append(str(p.index))
    return " ".join(ids)


def player_info(pid):
    """Tuple layout must match R2Adapter::GetPlayer ("sssiiiiiiiiiislsfff")."""
    p = _player(pid)
    if p is None:
        return None
    try:
        auth = str(p.getProfileId())
        if auth == "0":
            auth = ""
    except:
        auth = ""
    kit = ""
    try:
        kit = _name(p.getKit())
    except:
        pass
    vid, vname = 0, ""
    soldier = None
    try:
        soldier = p.getDefaultVehicle()
    except:
        pass
    veh = _vehicle_of(p)
    if veh is not None:
        vid = _handle(veh)
        vname = _name(veh)
    x, y, z = 0.0, 0.0, 0.0
    if soldier is not None:
        x, y, z = _pos(soldier)
    try:
        score, kills, deaths = p.score.score, p.score.kills, p.score.deaths
    except:
        score, kills, deaths = 0, 0, 0
    return (str(p.getName()), str(p.getAddress()), auth,
            int(p.getTeam()), int(p.getSquadId()), int(score), int(kills), int(deaths),
            int(p.getPing()), int(p.isAIPlayer()), int(p.isAlive()), int(p.isCommander()),
            int(p.isSquadLeader()), kit, vid, vname, x, y, z)


def kill_player(pid):
    p = _player(pid)
    if p is None or not p.isAlive():
        return 0
    p.getDefaultVehicle().setDamage(0)   # VERIFY: standard BF2 admin-script idiom
    return 1


def set_team(pid, team):
    p = _player(pid)
    if p is None:
        return 0
    p.setTeam(team)
    return 1


def set_score(pid, score):
    p = _player(pid)
    if p is None:
        return 0
    p.score.score = score
    return 1


def teleport(pid, x, y, z):
    p = _player(pid)
    if p is None or not p.isAlive():
        return 0
    p.getDefaultVehicle().setPosition((x, y, z))   # VERIFY
    return 1


def damage_player(pid, amount, attacker):
    """Lower a living player's soldier hit points. The attacker is not
    credited: BF2's Python API has no way to attribute damage."""
    p = _player(pid)
    if p is None or not p.isAlive():
        return 0
    soldier = p.getDefaultVehicle()
    hp = float(soldier.getDamage()) - float(amount)   # VERIFY: getDamage() = remaining hit points
    if hp < 0.0:
        hp = 0.0
    soldier.setDamage(hp)
    return 1


def player_health(pid):
    """(hit points, max hit points or -1) of a living player, else None."""
    p = _player(pid)
    if p is None or not p.isAlive():
        return None
    try:
        return (float(p.getDefaultVehicle().getDamage()), -1.0)
    except:
        return None


# ---- world objects --------------------------------------------------------

PCO_TYPE = 'dice.hfe.world.ObjectTemplate.PlayerControlObject'   # VERIFY


def vehicles():
    """'handle|template|isroot' entries separated by spaces."""
    try:
        objs = bf2.objectManager.getObjectsOfType(PCO_TYPE)   # VERIFY
    except:
        return ""
    out = []
    for o in objs:
        try:
            out.append('%d|%s|%d' % (_handle(o), _name(o), int(_root(o) is o)))
        except:
            pass
    return " ".join(out)


def object_valid(h):
    return int(_object(h) is not None)


def _occupant_ids(root_handle):
    ids = []
    for p in bf2.playerManager.getPlayers():
        veh = _vehicle_of(p)
        if veh is not None and _handle(_root(veh)) == root_handle:
            ids.append(p.index)
    return ids


def object_occupants(h):
    o = _object(h)
    if o is None:
        return ""
    return " ".join(map(str, _occupant_ids(_handle(_root(o)))))


def object_info(h):
    """Tuple layout must match R2Adapter::GetObjectInfo ("slsfffffffii")."""
    o = _object(h)
    if o is None:
        return None
    r = _root(o)
    x, y, z = _pos(o)
    rx, ry, rz = 0.0, 0.0, 0.0
    try:
        rot = r.getRotation()   # (yaw, pitch, roll) degrees
        rx, ry, rz = float(rot[0]), float(rot[1]), float(rot[2])
    except:
        pass
    try:
        health = float(r.getDamage())   # VERIFY
    except:
        health = -1.0
    rh = _handle(r)
    return (_name(o), rh, _name(r), x, y, z, rx, ry, rz, health, len(_occupant_ids(rh)), int(r is o))


def set_object_position(h, x, y, z):
    o = _object(h)
    if o is None:
        return 0
    _root(o).setPosition((x, y, z))   # VERIFY
    return 1


def set_object_rotation(h, yaw, pitch, roll):
    o = _object(h)
    if o is None:
        return 0
    _root(o).setRotation((yaw, pitch, roll))   # VERIFY
    return 1


def change_map(name, mode, size):
    """Run a map now: reuse its maplist entry or append one, then switch."""
    lines = str(host.rcon_invoke("maplist.list")).splitlines()
    index = -1
    i = 0
    for line in lines:
        parts = line.replace('"', ' ').replace(':', ' ').split()
        # expected: <index> <map> <mode> <size>   VERIFY output format
        if len(parts) >= 4 and parts[1].lower() == name.lower() and parts[2].lower() == mode.lower() \
                and parts[3] == str(size):
            index = int(parts[0])
            break
        i = i + 1
    if index < 0:
        host.rcon_invoke('maplist.append %s %s %d' % (name, mode, size))
        index = len(str(host.rcon_invoke("maplist.list")).splitlines()) - 1
    host.rcon_invoke("admin.nextLevel %d" % index)
    host.rcon_invoke("admin.runNextLevel")
    return 1


def get_tickets(team):
    return int(bf2.gameLogic.getTickets(team))


def set_tickets(team, tickets):
    bf2.gameLogic.setTickets(team, tickets)
    return 1


def handle_rcon(cmd):
    """Hook for admin/default.py's TCP rcon dispatcher: returns RMM's output
    if RMM handled the command, otherwise None (let default.py handle it)."""
    if _rmm is None:
        return None
    return _rmm.rcon(SRC_RCON, -1, cmd)


# ---------------------------------------------------------------------------
# Host event handlers
# ---------------------------------------------------------------------------

def _try_auth(p):
    # Waits for a GameSpy profile id. LAN/offline servers never get one (it
    # stays 0), so after AUTH_TIMEOUT seconds the player is authed without it:
    # IP and name admins/bans still apply.
    now = time.time()
    first = _pending_auth.get(p.index, now)
    try:
        if p.getProfileId() or p.isAIPlayer() or now - first >= AUTH_TIMEOUT:
            if _pending_auth.has_key(p.index):
                del _pending_auth[p.index]
            _rmm.player_auth(p.index)
            return 1
    except:
        pass
    _pending_auth[p.index] = first
    return 0


def onPlayerConnect(p):
    _teams[p.index] = p.getTeam()
    _rmm.player_connect(p.index, str(p.getName()), str(p.getAddress()))
    if _player(p.index) is not None:
        _try_auth(p)


def onPlayerDisconnect(p):
    if _teams.has_key(p.index):
        del _teams[p.index]
    if _pending_auth.has_key(p.index):
        del _pending_auth[p.index]
    _rmm.player_disconnect(p.index)


def onPlayerSpawn(p, soldier):
    x, y, z = _pos(soldier)
    kit = ""
    try:
        kit = _name(p.getKit())
    except:
        pass
    _rmm.player_spawn(p.index, kit, x, y, z)


def onPlayerKilled(victim, attacker, weapon, assists, obj):
    vid = _index(victim)
    aid = -1
    vehicle = ""
    tk = 0
    if attacker is not None:
        aid = _index(attacker)
        try:
            tk = int(aid != vid and attacker.getTeam() == victim.getTeam())
            v = attacker.getVehicle()
            if v is not None and v != attacker.getDefaultVehicle():
                vehicle = _name(v)
        except:
            pass
    _rmm.player_killed(vid, aid, _name(weapon), vehicle, tk)


def onPlayerRevived(revivee, medic):   # VERIFY handler name/args
    _rmm.player_revived(_index(revivee), _index(medic))


def onPlayerChangeTeams(p, humanHasSpawned):
    old = _teams.get(p.index, -1)
    new = p.getTeam()
    _teams[p.index] = new
    _rmm.player_change(EV_PLAYER_TEAM, p.index, old, new)


def onPlayerChangedSquad(p, oldSquad, newSquad):
    _rmm.player_change(EV_PLAYER_SQUAD, p.index, int(oldSquad), int(newSquad))


def onPlayerScore(p, difference):
    try:
        total = p.score.score
    except:
        total = 0
    _rmm.player_score(p.index, int(difference), int(total))


def onPickupKit(p, kit):
    _rmm.player_kit(EV_PLAYER_KIT_PICKUP, p.index, _name(kit))


def onDropKit(p, kit):
    _rmm.player_kit(EV_PLAYER_KIT_DROP, p.index, _name(kit))


def _seat(player, vehicle):
    try:
        if vehicle.getOccupyingPlayers()[0] == player:   # VERIFY
            return 0
    except:
        pass
    return -1


def onEnterVehicle(p, vehicle, freeSoldier=False):
    seat = _seat(p, vehicle)
    root = _root(vehicle)
    _rmm.vehicle(EV_VEHICLE_ENTER, p.index, _handle(vehicle), _name(vehicle), seat, int(seat == 0),
                 _handle(root), _name(root))


def onExitVehicle(p, vehicle):
    root = _root(vehicle)
    _rmm.vehicle(EV_VEHICLE_EXIT, p.index, _handle(vehicle), _name(vehicle), -1, 0, _handle(root), _name(root))


def onVehicleDestroyed(vehicle, attacker):
    _rmm.vehicle_destroyed(_handle(vehicle), _name(vehicle), _index(attacker))


def onControlPointChangedOwner(cp, attackingTeam):
    try:
        new = int(cp.cp_getParam('team'))   # VERIFY
    except:
        new = attackingTeam
    name = _name(cp)
    _rmm.control_point(_handle(cp), name, -1, new)


_CHANNELS = {"global": CH_ALL, "team": CH_TEAM, "squad": CH_SQUAD, "commander": CH_COMMANDER}
_PREFIXES = ("HUD_TEXT_CHAT_TEAM", "HUD_TEXT_CHAT_SQUAD", "HUD_CHAT_DEADPREFIX", "*\xa71DEAD\xa70*")   # VERIFY


def onChatMessage(playerId, text, channel, flags):
    text = str(text)
    for pre in _PREFIXES:
        if text.startswith(pre):
            text = text[len(pre):].strip()
    ch = _CHANNELS.get(str(channel).lower(), CH_ALL)
    if playerId < 0:
        ch = CH_SERVER
    _rmm.chat(int(playerId), ch, text)


def onRemoteCommand(playerId, cmd):
    # In-game "rcon <command>". default.py handles login separately.
    if playerId < 0:
        return
    out = _rmm.rcon(SRC_INGAME, int(playerId), str(cmd))
    if out is not None:
        host.rcon_feedback(playerId, out)


def _map_info():
    m, mode, mod, size = "", "", "", 0
    try:
        m = bf2.gameLogic.getMapName()            # VERIFY
    except:
        pass
    try:
        mode = bf2.serverSettings.getGameMode()   # VERIFY
    except:
        pass
    try:
        mod = bf2.gameLogic.getModDir()           # VERIFY
    except:
        pass
    return str(m), str(mode), str(mod), int(size)


_STATUS_MAP = {}


def onGameStatusChanged(status):
    global _status
    new = _STATUS_MAP.get(status, ST_UNKNOWN)
    old = _status
    _status = new
    if new == ST_PREGAME:
        _reset_objects()   # the old level's objects are gone
        m, mode, mod, size = _map_info()
        _rmm.map_load(m, mode, mod, size)
    _rmm.game_status(old, new)
    if new == ST_PLAYING and old != ST_PAUSED:
        m, mode, mod, size = _map_info()
        _rmm.round_start(m, mode, mod, size)
    if new == ST_ENDGAME:
        winner = 0
        try:
            t1, t2 = bf2.gameLogic.getTickets(1), bf2.gameLogic.getTickets(2)
            if t1 > t2:
                winner = 1
            elif t2 > t1:
                winner = 2
        except:
            pass
        _rmm.round_end(winner, 0)


def _on_frame(data=None):
    # Retry auth for players whose profile id was not known at connect.
    for pid in _pending_auth.keys():
        p = _player(pid)
        if p is None:
            del _pending_auth[pid]
        else:
            _try_auth(p)
    _rmm.frame()


_HANDLERS = (
    ("PlayerConnect", onPlayerConnect),
    ("PlayerDisconnect", onPlayerDisconnect),
    ("PlayerSpawn", onPlayerSpawn),
    ("PlayerKilled", onPlayerKilled),
    ("PlayerRevived", onPlayerRevived),
    ("PlayerChangeTeams", onPlayerChangeTeams),
    ("PlayerChangedSquad", onPlayerChangedSquad),
    ("PlayerScore", onPlayerScore),
    ("PickupKit", onPickupKit),
    ("DropKit", onDropKit),
    ("EnterVehicle", onEnterVehicle),
    ("ExitVehicle", onExitVehicle),
    ("VehicleDestroyed", onVehicleDestroyed),
    ("ControlPointChangedOwner", onControlPointChangedOwner),
    ("ChatMessage", onChatMessage),
    ("RemoteCommand", onRemoteCommand),
)


def init():
    global _initialised, _timer
    if _initialised:
        return
    if _rmm is None:
        print "[RMM] _rmm native module not present - is RMM loaded into the server?"
        return
    _initialised = 1

    gs = bf2.GameStatus
    for attr, val in (("PreGame", ST_PREGAME), ("Playing", ST_PLAYING), ("EndGame", ST_ENDGAME),
                      ("Paused", ST_PAUSED), ("Restart", ST_LOADING), ("NotConnected", ST_UNKNOWN)):
        if hasattr(gs, attr):
            _STATUS_MAP[getattr(gs, attr)] = val

    for name, fn in _HANDLERS:
        try:
            host.registerHandler(name, _safe(fn), 1)
        except:
            print "[RMM] could not register handler", name
    host.registerGameStatusHandler(_safe(onGameStatusChanged))

    _timer = bf2.Timer(_safe(_on_frame), FRAME_INTERVAL, 1)
    _timer.setRecurring(FRAME_INTERVAL)

    # Players already on the server (bridge loaded mid-game).
    for p in bf2.playerManager.getPlayers():
        _teams[p.index] = p.getTeam()

    _rmm.ready()
    print "[RMM] Python bridge initialised"
