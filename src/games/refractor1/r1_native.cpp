// Refractor 1 adapter - the native backend, for both server builds:
//   Linux   bf1942_lnxded.dynamic (GCC 3.2.3, i386): functions and globals by
//           mangled name from the executable's .symtab (gamedata [linux]).
//   Windows BF1942_w32ded.exe (VC++ 7.0, x86, stripped): addresses for the
//           one known build (gamedata [windows], checked against the PE
//           timestamp), found by matching the Linux functions in Ghidra.
//
// The engine is the same source on both, so the hooks and the logic are shared;
// r1_abi.h covers the calling-convention, std::string and container
// differences, and every struct/vtable offset comes from gamedata
// ([linux_offsets] / [windows_offsets]). Evidence: docs/re/bf1942-linux.md and
// docs/re/bf1942-windows.md.
#include "r1_adapter.h"

#include "../../core/core.h"
#include "../../core/platform.h"
#include "../../core/util.h"
#include "../common/elf_symbols.h"
#include "../common/hooks.h"
#include "r1_abi.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <vector>

#ifdef _WIN32
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    include <windows.h>
#endif

namespace rmm {

namespace {

// void dice::ref2::io::OldConsole::executeLine(const std::string& line, std::string& out,
//                                              bool, bool, bool, bool, bool)
//   io::Console vtable +0x8 (Windows +0x4). The 1.61 server runs *every* console line through
//   it: .con scripts, the server console and remote admin. (The newer
//   console::Parser / LocalConsole classes are linked in but unused.) The
//   engine's own callers pass all five flags false. Bools are pushed as ints.
using OldExecuteLineFn = void(R1_THIS*)(void* console, es::String* line, es::String* out, int b3, int b4, int b5,
                                        int b6, int b7);
// Linux: dice::bf::Setup::updateConsole() - called once per main-loop iteration.
// Windows: updateConsole is inlined; Setup::mainLoop() (one iteration per call,
// returns false to quit) is hooked instead.
using UpdateConsoleFn = int(R1_THIS*)(void* setup);

// void GameServer::handleChatMessage(const ChatMessage&)   (virtual; also reached via say())
using HandleChatFn = void(R1_THIS*)(void* gameServer, uint8_t* msg);
// void cdKeyAuthorizeCallback(int gameid, int localid, int authenticated, char* errmsg, void* instance)
//   GameSpy SDK callback; instance is the GameServer*. The Windows build's
//   whole-program optimisation turned it and gcd_getkeyhash into __fastcall
//   (gameid in ECX, localid in EDX).
#ifdef _WIN32
using CdKeyAuthFn = void(__fastcall*)(int gameId, int localId, int authenticated, char* errmsg, void* instance);
using GetKeyHashFn = const char*(__fastcall*)(int gameId, int localId);
#else
using CdKeyAuthFn = void (*)(int gameId, int localId, int authenticated, char* errmsg, void* instance);
// const char* gcd_getkeyhash(int gameid, int localid)   (C, GameSpy SDK)
using GetKeyHashFn = const char* (*)(int gameId, int localId);
#endif
// void GameServer::say(int id, const std::string&, bool teamOnly, bool flag)
//   Windows: a method of the GameServer base at +0x144 (gamedata gs_this_say).
using SayFn = void(R1_THIS*)(void* gameServer, int id, es::String* msg, int teamOnly, int flag);
// void GameServer::sayClient(int id, std::string msg, bool, bool)
//   std::string by value: Itanium ABI passes a pointer to a caller-owned copy;
//   VC++ copies the object itself onto the stack and the callee destroys it.
#ifdef _WIN32
using SayClientFn = void(R1_THIS*)(void* gameServer, int id, es::String msgByValue, int a, int b);
#else
using SayClientFn = void (*)(void* gameServer, int id, es::String* msgCopy, int a, int b);
#endif
// std::string GameServer::getPlayerIPAndPort(int) const
//   Returned via hidden pointer. Linux: declaring a by-value POD return makes
//   the compiler use the same i386 psABI convention (callee pops the pointer).
//   Windows: `this` in ECX, then the return slot, then the id (base +0x144).
#ifdef _WIN32
using GetIpFn = es::String*(R1_THIS*)(void* gameServer, es::String* ret, int id);
#else
using GetIpFn = es::String (*)(void* gameServer, int id);
#endif

// void io::OldConsole::output(const std::string&) - vtable +0x10. Everything
// executeLine prints goes through it, so RMM captures it during its own Exec.
using ConsoleOutputFn = void(R1_THIS*)(void* console, es::String* line);
// int GameServer::destroyPlayer(BFPlayer*) - the engine frees a player slot
using DestroyPlayerFn = int(R1_THIS*)(void* gameServer, uint8_t* player);
// Game status transitions (GameServer::update's state machine):
//   void gameStatusPreGame(float)   - every frame while in pre-game
//   void gameStatusFirstPlaying()   - once, entering Playing (round start)
//   void gameStatusFirstEndGame()   - once, entering EndGame (round end)
using StatusTickFn = void(R1_THIS*)(void* gameServer, float dt);
using StatusFirstFn = void(R1_THIS*)(void* gameServer);
// GamePlayMode -> name, as dice::bf::gpmToString (08067880) maps it. Not called:
// it returns const std::string& (calling it with a by-value return convention
// kills the server) and treats any other value as a *fatal* error
// ("The game type is not defined...").
// Gamedata [linux] gpm_names (comma list by number, e.g. BFV adds GPM_CCOMBAT=5,
// GPM_EVOL=6) overrides the BF1942 table. Plain arrays: Attach() fills them
// from the LD_PRELOAD constructor, before dynamic initializers run.
constexpr int kMaxGpm = 16;
char g_gpmNames[kMaxGpm][32] = {};
bool g_gpmNamesSet = false;

const char* GpmName(int gpm) {
    if (g_gpmNamesSet)
        return gpm >= 0 && gpm < kMaxGpm ? g_gpmNames[gpm] : "";

    switch (gpm) {
        case 1:
            return "GPM_CTF";
        case 2:
            return "GPM_CQ";
        case 3:
            return "GPM_TDM";
        case 4:
            return "GPM_COOP";
        case 5:
            return "GPM_OBJECTIVEMODE";
        default:
            return "";
    }
}

void SetGpmNames(const std::string& list) {
    if (list.empty())
        return;

    size_t start = 0;

    for (int i = 0; i < kMaxGpm && start <= list.size(); ++i) {
        size_t comma = list.find(',', start);
        std::string name = Trim(list.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
        std::snprintf(g_gpmNames[i], sizeof g_gpmNames[i], "%s", name.c_str());

        if (comma == std::string::npos)
            break;

        start = comma + 1;
    }

    g_gpmNamesSet = true;
}

// int GameServer::spawnPlayer(BFPlayer*)          - always returns 0; success sets player+0x79
using SpawnFn = int(R1_THIS*)(void* gameServer, uint8_t* player);
// int GameServer::enterVehicle(BFPlayer*, IPlayerObject*, bool isDefault, bool isFake)
using EnterVehicleFn = int(R1_THIS*)(void* gameServer, uint8_t* player, void* object, int isDefault, int isFake);
// int GameServer::exitVehicle(BFPlayer*, bool isFake)
using ExitVehicleFn = int(R1_THIS*)(void* gameServer, uint8_t* player, int isFake);
// void GameServer::handleScore(BFPlayer* player, ScoreMsg, BFPlayer* victim, int weaponTemplateId, int)
using HandleScoreFn = void(R1_THIS*)(void* gameServer, uint8_t* player, int type, uint8_t* victim, int weaponId,
                                     int extra);

// ScoreMsg values (from handleScore's event-log switch)
enum ScoreMsg {
    kFlagCapture = 0,
    kAttack,
    kDefence,
    kKill,
    kDeath,
    kDeathNoMsg,
    kTK,
    kSpawned,
    kObjective,
    kObjectiveTK
};

// Virtual slots (verified against the vtables in the binary)
uint32_t g_vtTemplateManagerGetTemplate = 0x50;  // ObjectTemplateManager::getTemplate(unsigned) const (BFV 0x60)
// ObjectTemplate::getName() const -> const std::string&. Gamedata
// vt_template_get_name: BFV's vtables start with both destructors, so it is +0x1c there.
uint32_t g_vtTemplateGetName = 0x14;

SpawnFn g_spawn = nullptr;
EnterVehicleFn g_enterVehicle = nullptr;
ExitVehicleFn g_exitVehicle = nullptr;
HandleScoreFn g_handleScore = nullptr;
uintptr_t g_templateManagerGlobal = 0;  // address of IObjectTemplateManager*

OldExecuteLineFn g_oldExecuteLine = nullptr;  // original (trampoline if hooked)
UpdateConsoleFn g_updateConsole = nullptr;
HandleChatFn g_handleChat = nullptr;
CdKeyAuthFn g_cdKeyAuth = nullptr;
GetKeyHashFn g_getKeyHash = nullptr;
SayFn g_say = nullptr;
SayClientFn g_sayClient = nullptr;
GetIpFn g_getIp = nullptr;
// BFV's getPlayerIPAndPort prints the IPv4 octets in reverse (10.0.1.192 -> "192.1.0.10:port").
// Gamedata [linux] player_ip_reversed = 1 swaps them back.
bool g_ipReversed = false;

std::string UnreverseIp(const std::string& s) {
    size_t colon = s.find(':');
    std::string ip = s.substr(0, colon), rest = colon == std::string::npos ? "" : s.substr(colon);
    std::string o[4];
    size_t start = 0;

    for (int i = 0; i < 4; ++i) {
        size_t dot = ip.find('.', start);

        if ((i < 3) != (dot != std::string::npos))
            return s;  // not a dotted quad: leave as is

        o[i] = ip.substr(start, i < 3 ? dot - start : std::string::npos);
        start = dot + 1;
    }

    return o[3] + "." + o[2] + "." + o[1] + "." + o[0] + rest;
}

ConsoleOutputFn g_consoleOutput = nullptr;  // original OldConsole::output (trampoline if hooked)
DestroyPlayerFn g_destroyPlayer = nullptr;
StatusTickFn g_statusPreGame = nullptr;
StatusTickFn g_statusPlaying = nullptr;  // BFV: per-frame gameStatusPlaying(float), no "first" variant
StatusFirstFn g_statusFirstPlaying = nullptr;
StatusFirstFn g_statusFirstEndGame = nullptr;
uintptr_t g_setupGlobal = 0;       // address of the dice::bf::Setup* global
uint32_t g_offSetupLevel = 0x23c;  // Setup::getCurrentLevel(): CampaignEntry {level, gpm, mod}
// GamePlayMode (int) and mod (std::string) of the current level. BF1942 keeps
// them in the CampaignEntry (level+4, level+8); BFV's setCurrentLevel copies
// them to separate Setup fields. 0 = level-relative default.
uint32_t g_offSetupGpm = 0, g_offSetupMod = 0;

void* g_gameServer = nullptr;        // the dice::bf::GameServer instance (primary base)
uintptr_t g_gameInstanceGlobal = 0;  // &dice::bf::game (gamedata game_instance)
// VC++ passes a method introduced by a secondary base class that base's
// address as `this`: GameServer::say, getPlayerIPAndPort and destroyPlayer
// expect GameServer+0x144 on Windows (gamedata gs_this_*; 0 on Linux).
uint32_t g_thisSay = 0, g_thisGetIp = 0, g_thisDestroyPlayer = 0;

/// The GameServer as a given method expects `this`, or nullptr.
void* GsThis(uint32_t adjust = 0) { return g_gameServer ? static_cast<uint8_t*>(g_gameServer) + adjust : nullptr; }

/// Remember the GameServer seen by a hook. When the dice::bf::game global is
/// known it is the source instead (see GameServerNow), so a hook whose `this`
/// is a base subobject can't store the wrong pointer.
void CaptureGameServer(void* gs, uint32_t thisAdjust = 0) {
    if (!g_gameServer && !g_gameInstanceGlobal && gs)
        g_gameServer = static_cast<uint8_t*>(gs) - thisAdjust;
}

/// The GameServer: read from bf::game (gamedata game_instance) when known, else
/// what the hooks captured.
uintptr_t GameServerNow() {
    if (g_gameInstanceGlobal) {
        uintptr_t gs = 0;

        if (elf::SafeReadT(g_gameInstanceGlobal, gs) && gs)
            g_gameServer = reinterpret_cast<void*>(gs);
    }

    return reinterpret_cast<uintptr_t>(g_gameServer);
}

int g_rmmExecDepth = 0;             // >0 while RMM itself runs a console command
uint32_t g_vtConsoleOutput = 0x10;  // OldConsole::output(const std::string&) (Windows 0xc)
uint32_t g_execFlags = 0;           // executeLine flags for RMM's own Exec (gamedata console_exec_flags)
// While RMM's own Exec wants the result, OldConsole::output appends here
// instead of printing. Plain pointer: constant-initialized (see g_physicsSetSpeed).
std::string* g_captureOut = nullptr;
int g_printBypass = 0;              // >0 while RMM prints its own lines (never captured)
uintptr_t g_mainConsoleGlobal = 0;  // address of the io::Console* mainConsole global
uintptr_t g_oldConsoleVptr = 0;     // expected vptr of that object (OldConsole)

/// The server's main console (an io::OldConsole), or nullptr until it exists.
void* MainConsole() {
    uintptr_t console = 0, vptr = 0;

    if (!g_mainConsoleGlobal || !elf::SafeReadT(g_mainConsoleGlobal, console) || !console)
        return nullptr;

    if (g_oldConsoleVptr && (!elf::SafeReadT(console, vptr) || vptr != g_oldConsoleVptr))
        return nullptr;

    return reinterpret_cast<void*>(console);
}

// ChatMessage layout (confirmed from both handleChatMessage and say()).
struct ChatLayout {
    uint32_t kind = 0x00, text = 0x04, textCap = 64, length = 0x48, sender = 0x4c, serverFlag = 0x50;
} g_chat;

std::map<int, std::string> g_authIds;  // GameSpy localid (== player id) -> CD-key hash

// Field offsets used by the event hooks (copied from R1Adapter::off_ at attach).
uint32_t g_offId = 0x58, g_offVehicle = 0x4c, g_offSoldier = 0x68, g_offAlive = 0x79, g_offTemplate = 0x4c;
// BFPlayer round stats, incremented by ScoreManager::scoreEvent (a parallel set lives at +0xf4..+0x114).
uint32_t g_offScore = 0xc0, g_offKills = 0xa4, g_offDeaths = 0xa0;
// GameServer+0x18c is the NetServer; NetServer::getPing(int, int*) const is vtable +0x8c.
uint32_t g_offNetServer = 0x18c;
uintptr_t g_netServerVptr = 0;
uint32_t g_vtNetServerGetPing = 0x8c;  // gamedata vt_netserver_get_ping (BFV: 0x94)
// ControlPoint: team at +0x174 (setTeam), ControlPointTemplate* at +0x164.
uint32_t g_offCpTeam = 0x174, g_offCpTemplate = 0x164;

// void ControlPoint::setTeam(unsigned int)
using CpSetTeamFn = void(R1_THIS*)(uint8_t* controlPoint, unsigned team);
CpSetTeamFn g_cpSetTeam = nullptr;
// Windows: setTeam is inlined into ControlPoint::gotControl(int team) / lostControl(int),
// which have the same shape (thiscall, one int); both are hooked instead.
CpSetTeamFn g_cpGotControl = nullptr, g_cpLostControl = nullptr;
// void ControlPoint::handleFrameUpdate(float): the whole capture logic; skipped for locked points.
using CpFrameFn = void(R1_THIS*)(uint8_t* controlPoint, float dt);
CpFrameFn g_cpFrameUpdate = nullptr;
// ControlPoint state (see docs/re "Control points"): lose/take timers, taking team, state
// (1 lost, 2 losing, 3 getting, 4 controlled); template take/lose times, unableToChangeTeam
// byte, onlyTakeableByTeam.
uint32_t g_offCpLoseTimer = 0x168, g_offCpTakeTimer = 0x16c, g_offCpTakingTeam = 0x170, g_offCpState = 0x17c;
uint32_t g_offCpTmplTakeTime = 0x1f4, g_offCpTmplLoseTime = 0x1f8, g_offCpTmplFixed = 0x200,
         g_offCpTmplOnlyTeam = 0x214;
uint32_t g_cpClassId = 0x94a5;  // CID_ControlPointTemplate (gamedata cp_template_cid)
bool g_cpLayoutKnown = false;
// Locked control points (ControlPoint*). A plain array: the frame hook reads it every frame.
constexpr int kMaxCpLocks = 128;
uintptr_t g_cpLocks[kMaxCpLocks] = {};
int g_cpLockCount = 0;
bool g_cpQuiet = false;  // set_control_point raises its own CONTROL_POINT

// void GameServer::_giveDamage(IObject* target, float damage, int attackerId, int, int,
//                              Pos3 pos, int, bool, bool, bool)
//   Final (post-armour) damage; every damage path ends here. Pos3 = 3 floats by value.
using GiveDamageFn = void(R1_THIS*)(void* gameServer, uint8_t* target, float damage, int attackerId, int a5, int a6,
                                    float x, float y, float z, int a8, int b9, int b10, int b11);
GiveDamageFn g_giveDamage = nullptr;
uintptr_t g_playerManagerGlobal = 0;  // copied from the adapter for the damage hook
uint32_t g_offPlayerList = 0x0c;

// Deaths reported without a matching Kill/TK in the same tick are flushed
// next frame as world/suicide deaths (attacker = none).
std::map<int, std::string> g_pendingDeaths;  // victim id -> weapon
std::set<int> g_killedThisTick;              // victims already reported by a Kill/TK this tick

// ---- safe engine helpers ------------------------------------------------------

// Which engine object calls are safe to make. Linux: every slot is verified
// (the defaults in this file). Windows: VC++ orders these vtables differently,
// so a group is enabled only once gamedata [windows_offsets] lists all of its
// slots (see R1Adapter::AttachNative).
bool g_templatesOk = true;  // ObjectTemplate getName / getClassID
bool g_objectModel = true;  // IObject queryInterface/queryComponent/position, armour, PCO seat
// Fields RMM writes: only with offsets known for this build.
bool g_scoreFieldsOk = true, g_spawnFieldsOk = true;

template <class Fn> Fn VirtualAt(uintptr_t object, uint32_t slotOffset) {
    uintptr_t vtable = 0, fn = 0;

    if (!object || !elf::SafeReadT(object, vtable) || !elf::SafeReadT(vtable + slotOffset, fn) || !fn)
        return nullptr;

    return reinterpret_cast<Fn>(fn);
}

std::string TemplateName(uintptr_t tmpl) {
    if (!g_templatesOk)
        return {};

    using GetNameFn = es::String*(R1_THIS*)(uintptr_t self);
    auto getName = VirtualAt<GetNameFn>(tmpl, g_vtTemplateGetName);

    return getName ? es::Read(getName(tmpl)) : std::string();
}

/// Template name of a world object (vehicle, soldier): object+0x4c is its template.
std::string ObjectTemplateName(uintptr_t object) {
    uintptr_t tmpl = 0;

    if (!object || !elf::SafeReadT(object + g_offTemplate, tmpl))
        return {};

    return TemplateName(tmpl);
}

// ObjectTemplate::getClassID() const is vtable +0xc. The engine tests
// "== CID_BFSoldierTemplate" (0x9493, a .rodata constant) to tell soldiers
// apart, e.g. in changeKit and before checkForHitOnSoldier.
uint32_t g_vtTemplateGetClassId = 0x0c;  // gamedata vt_template_get_class_id (BFV: +0x14)
uint32_t g_soldierClassId = 0x9493;      // overridden from gamedata symbol soldier_template_cid

uint32_t TemplateClassId(uintptr_t tmpl) {
    if (!g_templatesOk)
        return 0;

    using Fn = uint32_t(R1_THIS*)(uintptr_t self);
    auto getClassId = tmpl ? VirtualAt<Fn>(tmpl, g_vtTemplateGetClassId) : nullptr;

    return getClassId ? getClassId(tmpl) : 0;
}

/// Template class id of a world object (0 if unknown).
uint32_t ObjectClassId(uintptr_t object) {
    uintptr_t tmpl = 0;

    if (!object || !elf::SafeReadT(object + g_offTemplate, tmpl) || !tmpl)
        return 0;

    return TemplateClassId(tmpl);
}

bool IsSoldier(uintptr_t object) { return object && ObjectClassId(object) == g_soldierClassId; }

std::string WeaponName(int templateId) {
    uintptr_t mgr = 0;

    if (!g_templateManagerGlobal || !elf::SafeReadT(g_templateManagerGlobal, mgr) || !mgr)
        return {};

    using GetTemplateFn = uintptr_t(R1_THIS*)(uintptr_t self, unsigned id);
    auto getTemplate = VirtualAt<GetTemplateFn>(mgr, g_vtTemplateManagerGetTemplate);
    uintptr_t tmpl = getTemplate ? getTemplate(mgr, static_cast<unsigned>(templateId)) : 0;

    return tmpl ? TemplateName(tmpl) : std::string();
}

int PlayerId(const uint8_t* player) {
    int32_t id = -1;

    if (!player || !elf::SafeRead(player + g_offId, &id, sizeof id))
        return -1;

    return id;
}

/// The object the player controls, or 0 when on foot (in their own soldier) or dead.
uintptr_t PlayerVehicle(const uint8_t* player) {
    uintptr_t vehicle = 0, soldier = 0;

    if (!player)
        return 0;

    elf::SafeRead(player + g_offVehicle, &vehicle, sizeof vehicle);
    elf::SafeRead(player + g_offSoldier, &soldier, sizeof soldier);

    return vehicle != soldier ? vehicle : 0;
}

// ---- object model helpers (mirror the engine's own interface walks) ----------
// Slots verified on concrete vtables (Object, BFSoldier, PlayerControlObject, Armor).
// The non-constexpr ones differ in BFV and come from gamedata.
uint32_t g_vtQueryInterface = 0x08;    // queryInterface(unsigned) const (BFV 0x10)
uint32_t kVtQueryComponent = 0x28;     // queryComponent(unsigned, unsigned) (same in BFV)
uint32_t kVtAbsolutePosition = 0x38;   // getAbsolutePosition() const -> const Pos3& (same in BFV)
uint32_t g_vtArmorIsDestroyed = 0xc8;  // Armor::isDestroyed() (BFV 0xd0)
uint32_t g_vtPcoId = 0x2c;             // on the IPlayerControlObject interface (the "pco_id" the engine logs; BFV 0x34)
constexpr uint32_t kIidComposite = 0xc378, kIidArmor = 0xc4a4, kIidPlayerControl = 0xc4c5;
uint32_t g_offParent = 0x50;  // ICompositeObject -> parent (BFV: IObject -> parent, also 0x50)
// BF1942 walks parents through the ICompositeObject interface. BFV has no
// such interface: IObject itself holds the parent, and the engine finds the
// IPlayerControlObject with dynamic_cast (gamedata object_parent_direct = 1
// plus the two typeinfo symbols).
bool g_directParent = false;
uintptr_t g_tiObject = 0, g_tiPlayerControl = 0;

// GCC 3 code returns bool in %al only; read 32 bits and mask.
inline bool LowByte(uint32_t v) { return (v & 0xff) != 0; }

uintptr_t QueryInterface(uintptr_t obj, uint32_t iid) {
    if (!g_objectModel)
        return 0;

    using Fn = uintptr_t(R1_THIS*)(uintptr_t self, unsigned iid);
    auto fn = VirtualAt<Fn>(obj, g_vtQueryInterface);

    return fn ? fn(obj, iid) : 0;
}

/// Canonical pointer for an object: its ICompositeObject (IID 0xc378, shared
/// with IObject), which is what the engine's own parent walks use. In BFV
/// the IObject pointer itself.
uintptr_t Composite(uintptr_t obj) {
    if (g_directParent)
        return obj;

    uintptr_t c = obj ? QueryInterface(obj, kIidComposite) : 0;

    return c ? c : obj;
}

/// Parent of a canonical node, as a canonical node (0 at the top).
uintptr_t ParentOf(uintptr_t node) {
    uintptr_t parent = 0;

    if (!node || !elf::SafeReadT(node + g_offParent, parent) || !parent)
        return 0;

    return g_directParent ? parent : QueryInterface(parent, kIidComposite);
}

/// Walk from @p object up its parents, as the engine does, until
/// @p probe returns non-zero for a node.
template <class Probe> uintptr_t WalkUp(uintptr_t object, Probe probe) {
    uintptr_t node = g_directParent ? object : (object ? QueryInterface(object, kIidComposite) : 0);

    for (int guard = 0; node && guard < 32; ++guard) {
        if (uintptr_t hit = probe(node))
            return hit;

        node = ParentOf(node);
    }

    return 0;
}

uintptr_t FindArmor(uintptr_t object) {
    if (!g_objectModel)
        return 0;

    return WalkUp(object, [](uintptr_t n) -> uintptr_t {
        using Fn = uintptr_t(R1_THIS*)(uintptr_t self, unsigned a, unsigned b);
        auto fn = VirtualAt<Fn>(n, kVtQueryComponent);

        return fn ? fn(n, kIidArmor, kIidArmor) : 0;
    });
}

uintptr_t FindPlayerControl(uintptr_t object) {
    if (g_directParent) {
        // Exactly the engine's loop: __dynamic_cast(node, &IObject, &IPlayerControlObject, -2).
        auto cast = es::DynamicCast();

        if (!cast || !g_tiObject || !g_tiPlayerControl)
            return 0;

        return WalkUp(object, [cast](uintptr_t n) {
            return reinterpret_cast<uintptr_t>(cast(reinterpret_cast<const void*>(n),
                                                    reinterpret_cast<const void*>(g_tiObject),
                                                    reinterpret_cast<const void*>(g_tiPlayerControl), -2));
        });
    }

    return WalkUp(object, [](uintptr_t n) { return QueryInterface(n, kIidPlayerControl); });
}

bool ArmorDestroyed(uintptr_t armor) {
    if (!g_objectModel)
        return false;

    using Fn = uint32_t(R1_THIS*)(uintptr_t self);
    auto fn = VirtualAt<Fn>(armor, g_vtArmorIsDestroyed);

    return fn && LowByte(fn(armor));
}

int SeatOf(uintptr_t object) {
    uintptr_t pco = FindPlayerControl(object);
    using Fn = int(R1_THIS*)(uintptr_t self);
    auto fn = pco ? VirtualAt<Fn>(pco, g_vtPcoId) : nullptr;

    return fn ? fn(pco) : -1;
}

bool AbsolutePosition(uintptr_t object, rmm_vec3& out) {
    if (!g_objectModel)
        return false;

    using Fn = const float*(R1_THIS*)(uintptr_t self);
    auto fn = VirtualAt<Fn>(object, kVtAbsolutePosition);
    const float* p = fn ? fn(object) : nullptr;
    float v[3];

    if (!p || !elf::SafeRead(p, v, sizeof v))
        return false;

    out = rmm_vec3{v[0], v[1], v[2]};

    return true;
}

// ---- world objects ----------------------------------------------------------
// dice::ref2::world::objectManager (an ObjectManager*) keeps two GCC 3
// std::map<unsigned, IObject*> keyed by object id (IObject+0x48):
//   +0x94  every registered object       (ObjectManager::registerObject)
//   +0x11c player-control objects (PCOs)  (ObjectManager::addPco / getPcoMap)
// A map member is a pointer to its heap header node; header->parent (+4) is
// the root, and nodes are {color, parent, left, right, key@+0x10, value@+0x14}.

// bool GameServer::killPlayer(int id): getPlayerFromId + killPlayer(BFPlayer*, false)
using KillPlayerFn = uint32_t(R1_THIS*)(void* gameServer, int id);
// ICompositeObject* getRootParent(const ICompositeObject*) - cached at +0xd4 by the engine
// (BFV: IObject* getRootParent(const IObject*))
using GetRootParentFn = uintptr_t (*)(uintptr_t composite);

KillPlayerFn g_killPlayer = nullptr;
GetRootParentFn g_getRootParent = nullptr;
uintptr_t g_objectManagerGlobal = 0;  // address of the ObjectManager* global
uintptr_t g_objectManagerVptr = 0;    // expected vptr (0 = don't check)
uint32_t g_offObjectId = 0x48, g_offOmObjects = 0x94, g_offOmPcoMap = 0x11c;
// Accepted PhysicsNode::setPositionalSpeed implementations (PhysicsNode,
// PointPhysicsNode, StaticPhysicsNode). A component whose vtable slot +0x40
// isn't one of these is not touched.
// Plain array, not std::vector: Attach() runs from the LD_PRELOAD constructor,
// before this file's dynamic initializers, and a vector's initializer would
// then wipe what Attach() stored. Anything Attach() fills must be
// constant-initialized.
uintptr_t g_physicsSetSpeed[3] = {};
size_t g_physicsSetSpeedCount = 0;

constexpr uint32_t kIidPhysicsNode = 0xc422;  // IID_IPhysicsNode
uint32_t kVtSetAbsolutePosition = 0x3c;       // BObject/BCompositeObject::setAbsolutePosition(const Pos3&)
// IPhysicsNode slots, identical on PhysicsNode, PointPhysicsNode and StaticPhysicsNode
uint32_t kVtPhysGetSpeed = 0x38, kVtPhysGetRotSpeed = 0x3c, kVtPhysSetSpeed = 0x40, kVtPhysSetRotSpeed = 0x44,
         kVtPhysSetAwake = 0xd4;
uint32_t g_vtArmorMaxHitPoints = 0x14, g_vtArmorHitPoints = 0x1c;  // float getters (BFV 0x1c / 0x24)

// ---- every world object, spawn/remove, terrain --------------------------------
// ObjectManager::addObjectToGrid files every *root* object (flag 0x2000000 at
// IObject+4) in one of two more id-keyed std::maps: +0x48 (collision grid,
// getAllObjectsFromGrid / getObjectFromGrid(id)) or +0x60 when flag 0x200 is
// set (getObjectFromNoCollisionGrid(id)). Buildings, props, kits on the
// ground, vehicles and soldiers are all in one of them.
uint32_t g_offOmGrid = 0x48, g_offOmGridNoCollision = 0x60;
uint32_t g_offNetworkable = 0x68;  // IObject -> networkable (0 = server-only object)

// const IObjectTemplate* ObjectTemplateManager::getTemplate(const std::string&) const
//   Hash lookup, case-insensitive. It writes a terminator into the string's
//   buffer, so it must be an engine string RMM owns (es::Scoped).
using GetTemplateByNameFn = uintptr_t(R1_THIS*)(uintptr_t self, es::String* name);
// IObject* GameServer::createObjectOnAllClients(const IObjectTemplate*, Pos3, Vec3 ypr, bool, int ghostId)
//   What GameServer::spawnObject(tmpl, pos, rot) calls (bool = true, ghostId = 0)
//   and how changeKit/spawnPlayer create kits. Pos3/Vec3 are 3 floats by value.
//   Returns NULL for a vehicle within 2 m of a living soldier.
using CreateObjectFn = uintptr_t(R1_THIS*)(void* gameServer, uintptr_t tmpl, float x, float y, float z, float yaw,
                                           float pitch, float roll, int readyClientsOnly, int ghostId);
// bool GameServer::destroyObject(IObject*) - vtable +0x54. Kills players sitting
// in it, tells every client (DestroyObjectEvent), drops the ghost, then the
// object's own destroy (deferred to the end of the frame by ObjectManager).
using DestroyObjectFn = uint32_t(R1_THIS*)(void* gameServer, uintptr_t object);
// BaseMatrix4<float>& setRotation<float>(BaseMatrix4<float>&, const BaseVector3<float>&)
//   Resets the 3x3 part, then yaw/pitch/roll in degrees (rotateZDeg); keeps the translation row.
using SetRotationFn = float* (*)(float* mat16, const float* yawPitchRoll);
// void world::setTransformation(IObject*, const Mat4&) - the absolute-matrix
// setter createObjectOnAllClients uses after setRotation.
using SetTransformationFn = void (*)(uintptr_t object, const float* mat16);
uint32_t kVtGetTransformation = 0x40;  // IObject: const Mat4& (16 floats), copied by createObjectOnAllClients

GetTemplateByNameFn g_getTemplateByName = nullptr;
CreateObjectFn g_createObject = nullptr;
DestroyObjectFn g_destroyObject = nullptr;
SetRotationFn g_setRotation = nullptr;
SetTransformationFn g_setTransformation = nullptr;
#ifdef _WIN32
// Windows: createObjectOnAllClients's helper does both steps:
// void __fastcall setObjectRotation(IObject* obj, const Vec3* yawPitchRoll) -
// copies the relative (with a parent) or absolute matrix, setRotation, sets it back.
using ObjectSetRotationFn = void(__fastcall*)(uintptr_t object, const float* yawPitchRoll);
ObjectSetRotationFn g_objectSetRotation = nullptr;
#endif
/// Rotation can be set: either path.
bool CanRotate() {
#ifdef _WIN32
    if (g_objectSetRotation)
        return true;
#endif
    return g_setRotation && g_setTransformation;
}

// geom::ITerrain* geom::terrainBase; getHeight(float x, float z) const is
// vtable +0x44 on PatchTerrain (the real terrain; NullTerrain before a map loads).
uintptr_t g_terrainGlobal = 0, g_patchTerrainVptr = 0;
uint32_t g_vtTerrainGetHeight = 0x44;

// ---- bots -------------------------------------------------------------------------
// void ai::AIPlayer::addInput(AIPlayer*, PlayerInput*): the one place a bot's
// input reaches the engine (BotMain::updatePlayerAction -> PlayerAction::set ->
// ActionBuffer::pushBack at BFPlayer+0x144, like a client's input). AIPlayer+0
// is the BFPlayer*. PlayerInput = Input<PlayerInputMap, 55>: float value[55],
// then a 64-bit "mapped" mask at +0xdc (PlayerAction::set reads a value only if
// its bit is set; axes 0..5 become 12.4 fixed point, the rest are buttons > 0.5).
using AddInputFn = void(R1_THIS*)(uintptr_t aiPlayer, uint8_t* input);
AddInputFn g_addInput = nullptr;

// bool ai::BAPAChangeWeapon::execute(Bot*, bool& running, bool& done): a bot's
// weapon switch action. Its constructor caches the target item object at +0x1c
// (slot at +0x18) and execute calls that item's vtable every frame until the
// switch finishes. A live kit swap (set_kit, e.g. gun game levelling a bot up)
// destroys the old items, and execute then calls through freed memory (SIGSEGV
// at eip 0, BAPAChangeWeapon::execute+0x28d). The hook redoes
// the constructor's lookup on every call: Bot vfunc +0xd0 = info handle, resolved
// through ai::informationManager (8-byte entries, object* at -8, u16 serial at
// -2 from entry end), info+0x34 = soldier interface, its vfunc +0x1c = item in slot.
using ChangeWeaponExecFn = uint32_t(R1_THIS*)(uintptr_t self, uintptr_t bot, bool* running, bool* done);
ChangeWeaponExecFn g_changeWeaponExec = nullptr;
uintptr_t g_aiInfoManager = 0;  // address of ai::informationManager (a pointer to the table pointer)

// The bot plan holds more than the switch action: script variables of type
// ai::Weapon* (BAPConWeaponMagAmmo::evaluate hits __cxa_pure_virtual on a freed
// one). So after a live kit swap on a bot, RMM resets its brain
// the engine's way: BotMain::reset() (vtable +0x2c) deletes the plan (+0x74) and
// flags a replan, and clears behaviours, memory and fire objects. Kit swaps on
// bots are always deferred to RMM's frame (outside AIMain::action) for this.
using BotFromPlayerIdFn = uintptr_t(R1_THIS*)(uintptr_t botManager, int32_t playerId);
BotFromPlayerIdFn g_botFromPlayerId = nullptr;
// Windows has no separate getBotFromPlayerId: BotManager::getBot(int botId) (vtable +0x18)
// with the bot id from the player-id table does the same.
using BotGetBotFn = uintptr_t(R1_THIS*)(uintptr_t botManager, int32_t botId);
BotGetBotFn g_botGetBot = nullptr;

inline bool HaveBotLookup() { return g_botFromPlayerId || g_botGetBot; }

uintptr_t g_botMainReset = 0;  // BotMain::reset(); must match the bot's vtable +0x2c
uint32_t kVtBotReset = 0x2c;
uint32_t g_inputSlots = 55;      // gamedata playerinput_slots
uint32_t g_offInputMask = 0xdc;  // gamedata playerinput_mask
uint32_t g_inputSize = 0xec;     // gamedata playerinput_size (GameServer copies 0x3b dwords per input)
// Bot removal = AIPlayer::removePlayerFromGame (what ~BotMain runs), after
// ai.deactivateBot detached the AI: GameServer::exitVehicle(player, false),
// destroyObject(player->defaultCamera, +0x74, set by setDefaultCamera), then
// GameServer::destroyPlayer(player) (queryInterface(0x1d4c1) vtable +0x18c).
// Raw addresses: calling them goes through RMM's own hooks, so VEHICLE_EXIT
// and the CD-key bookkeeping see the removal like any other.
uintptr_t g_exitVehicleAddr = 0, g_destroyPlayerAddr = 0;
uint32_t g_offDefaultCamera = 0x74;  // gamedata player_default_camera
// Co-op refills itself: gameStatusPreGame/FirstPlaying/Playing create an AI
// player and IAIMain::activateBot it (reusing a deactivated brain) while
// players (GameServer+0x18, decremented by destroyPlayer) < max players (+0x1c,
// setMaxNrOfPlayers) and active bots != IAISettings::getMaxNBots. To keep a
// removed bot's slot empty for the rest of the round, the player count is
// raised by the number of removed bots while those functions run (+0x18 is
// only read by that refill loop there; +0x1c also drives ticket bleed, so it
// is left alone). Lowering maxNBots instead crashes the server (SAI and
// AIRadio size per-bot data by it).
int g_botSlotsHeld = 0;
uint32_t g_offPlayerCount = 0x18;  // gamedata gameserver_player_count
// ai::IBotManager::instance -> BotManager; +0x38 = std::vector<int> player id ->
// bot id (-1 = none; deactivateBot* resets it). Used instead of ai.getAllBots,
// whose SmartFunctionContainer::getAllBots calls AIPlayer::getName() on every
// brain, including detached ones (player NULL) -> SIGSEGV after a removal.
uintptr_t g_botManagerGlobal = 0;
uint32_t g_offBotPidTable = 0x38;  // gamedata botmanager_pid_table

struct HoldBotSlots {
    uintptr_t gs;
    int32_t added;  // exactly what was added (a removal during the call must not unbalance it)

    explicit HoldBotSlots(void* gameServer)
        : gs(g_botSlotsHeld > 0 ? reinterpret_cast<uintptr_t>(gameServer) : 0), added(g_botSlotsHeld) {
        if (gs)
            *reinterpret_cast<int32_t*>(gs + g_offPlayerCount) += added;
    }

    ~HoldBotSlots() {
        if (gs)
            *reinterpret_cast<int32_t*>(gs + g_offPlayerCount) -= added;
    }
};

// ---- kits, round end, score, tickets --------------------------------------------
// See docs/re/bf1942-linux.md "Kits, round end, score and tickets".
//
// void GameServer::handlePickup(IPlayerObject* item, IPlayer* player, IPlayerObject* soldier, bool)
//   (vtable +0x40). Every item a soldier takes goes through it: kits (class
//   0x9496), CTF flags, hand weapons. It tells every client (pickupKit
//   event), then the item's IPickup (0xc49c) +0xc -> Kit::handlePickup ->
//   soldier vtable +0x174 = BFSoldier::pickupKit, which drops the soldier's
//   current kit (vtable +0x178 = dropKit) and arms them with the new one.
//   spawnPlayer and changeKit hand out kits with it too.
using HandlePickupFn = void(R1_THIS*)(void* gameServer, uintptr_t item, uint8_t* player, uintptr_t soldier, int flag);
// void GameServer::changeKit(BFPlayer*, unsigned char kit) - the engine's own
// kit swap for a living soldier: create kit, handlePickup, player+0x80 = kit,
// destroyObject(old kit), selectBestLoadedWeapon. Only a context marker for RMM.
using ChangeKitFn = void(R1_THIS*)(void* gameServer, uint8_t* player, int kit);
// bool BFSoldier::dropKit() - the carried kit falls to the ground.
using DropKitFn = uint32_t(R1_THIS*)(uintptr_t soldier);
// void BFSoldier::selectBestLoadedWeapon()
using SoldierVoidFn = void(R1_THIS*)(uintptr_t soldier);
// void GameServer::setGameStatus(GameStatus) - 2 = EndGame (vtable +0x78)
using SetGameStatusFn = void(R1_THIS*)(void* gameServer, int status);
// ScoreManager::setWinner(uchar) / setVictoryType(uchar) (vtable +0x54 / +0x5c);
// getWinner() / getVictoryType() return uchar (in %al).
using ScoreSetFn = void(R1_THIS*)(uintptr_t scoreManager, int value);
using ScoreGetFn = uint32_t(R1_THIS*)(uintptr_t scoreManager);
// TeamScore* ScoreManager::getTeamScore(unsigned team) (vtable +0x10); tickets at +0x48
using GetTeamScoreFn = uintptr_t(R1_THIS*)(uintptr_t scoreManager, unsigned team);
// void GameServer::setCurrentNumberOfTickets(unsigned team, unsigned tickets) -> TeamScore::setTickets
using SetTicketsFn = void(R1_THIS*)(void* gameServer, unsigned team, unsigned tickets);

HandlePickupFn g_handlePickup = nullptr;  // original (trampoline if hooked)
uintptr_t g_handlePickupAddr = 0;         // the function itself: RMM's own calls go through the hook
ChangeKitFn g_changeKit = nullptr;
DropKitFn g_dropKit = nullptr;
SoldierVoidFn g_selectBestWeapon = nullptr;
SetGameStatusFn g_setGameStatus = nullptr;
ScoreSetFn g_scoreSetWinner = nullptr, g_scoreSetVictoryType = nullptr;
ScoreGetFn g_scoreGetWinner = nullptr, g_scoreGetVictoryType = nullptr;
GetTeamScoreFn g_getTeamScore = nullptr;
SetTicketsFn g_setCurrentTickets = nullptr;
uintptr_t g_scoreManagerGlobal = 0;     // address of the ScoreManager* global
uint32_t g_kitClassId = 0x9496;         // CID_KitTemplate (gamedata kit_template_cid)
uint32_t g_offTeamScoreTickets = 0x48;  // gamedata teamscore_tickets
uint32_t g_offScoreTotal = 0x114;       // BFPlayer: second score counter scoreEvent keeps in step with +0xc0
int32_t g_endGameStatus = 2;            // GameStatus EndGame (handleScore / gameStatusPlaying)

// Why the kit hooks are running (rmm_kit_reason), -1 = nobody said: a pickup
// from the ground (handlePickup) or e.g. a death drop (dropKit).
int g_kitContext = -1;

struct KitContext {
    int saved;

    explicit KitContext(int reason) : saved(g_kitContext) { g_kitContext = reason; }

    ~KitContext() { g_kitContext = saved; }
};

// ---- vehicle enter/exit reasons --------------------------------------------------
// void GameServer::checkPlayerTriggers(BFPlayer*, PlayerInput) - a player's (or
//   bot's) input this tick: the use key -> toggleEntryPoint (enter on foot,
//   exit in a vehicle after PlayerControlObject::isAllowedToExit); F1-F8 ->
//   exitVehicle(player, false) then enterVehicle(seat) *without* checking the
//   exit's result; the kit pickup key -> findKitObject(pos, 1.1) + handlePickup.
//   PlayerInput is passed by value (0xec bytes, = gamedata playerinput_size).
// void GameServer::toggleEntryPoint(BFPlayer*, bool) - also called by
//   spawnPlayer/reSpawnPlayer to put a player spawning at a vehicle into it.
// Every other exitVehicle call is the game's own (death, respawn, removal...).
#ifdef _WIN32
constexpr uint32_t kPlayerInputSize = 0xf0;  // VC++ aligns the 64-bit mask to 8: mask at +0xe0
#else
constexpr uint32_t kPlayerInputSize = 0xec;
#endif
struct PlayerInputBlob {
    uint32_t d[kPlayerInputSize / 4];
};

using CheckTriggersFn = void(R1_THIS*)(void* gameServer, uint8_t* player, PlayerInputBlob input);
using ToggleEntryFn = void(R1_THIS*)(void* gameServer, uint8_t* player, int flag);
CheckTriggersFn g_checkTriggers = nullptr;
ToggleEntryFn g_toggleEntry = nullptr;
uintptr_t g_enterVehicleAddr = 0;  // raw GameServer::enterVehicle (RMM's own calls go through the hook)
bool g_triggersHooked = false;

// ---- items (docs/re/bf1942-linux.md "Weapon switching") --------------------------
// A soldier keeps one item (weapon/tool) per slot: object ids at soldier+0x3ac
// (an int array), the slot count at its template+0x2d4, the held slot at
// soldier+0x3b8. Slot = the item template's itemIndex (1 knife, 2 pistol, 3
// main weapon, 4 grenade/explosive, 5+ tools).
// void BFSoldier::enableItem(char slot) (vtable +0x100) - hold that slot's
//   item. Called by BFSoldier::handleMessage for the weapon keys (messages
//   10..17 = slots 1..8) and by the game itself (selectBestLoadedWeapon,
//   pickupKit, Kit::enable, the detonator after an explosive pack...).
// void BFSoldier::enableItemForceNetwork(char slot) (vtable +0x104) - the same
//   without the firing checks; BFSoldierNetworkable::setNetUpdate calls it with
//   the replicated held slot (state bit 0x100) - only for soldiers the receiving
//   side doesn't control itself (networkable+0x14 == 0).
// IObject* BFSoldier::getItem(int slot) (vtable +0xec).
// ICompositeObject* BFSoldier::removeItem(int slot) (vtable +0xf0) - detaches
//   the item and clears the slot; the object lives on.
// void BFSoldier::handleMessage(TemplateMessage, IPlayer*) - hooked only to
//   tell the player's weapon keys from the game's own switches.
using EnableItemFn = void(R1_THIS*)(uintptr_t soldier, int slot);  // char, passed as int on the stack
using GetItemFn = uintptr_t(R1_THIS*)(uintptr_t soldier, int slot);
using RemoveItemFn = uintptr_t(R1_THIS*)(uintptr_t soldier, int slot);
using SoldierMessageFn = void(R1_THIS*)(uintptr_t soldier, int msg, void* player);
EnableItemFn g_enableItem = nullptr, g_enableItemNet = nullptr;  // originals (trampolines if hooked)
uintptr_t g_enableItemAddr = 0;                                  // raw: RMM's own select goes through the hook
GetItemFn g_getItem = nullptr;
RemoveItemFn g_removeItem = nullptr;
SoldierMessageFn g_soldierMessage = nullptr;
bool g_itemsHooked = false;
// Windows: getItem/removeItem belong to the soldier's item-container base (soldier+0x11c).
uint32_t g_thisItemContainer = 0;
uint32_t g_offSoldierItemIds = 0x3ac, g_offSoldierActiveItem = 0x3b8, g_offSoldierTemplateSlots = 0x2d4;
int g_itemContext = -1;  // rmm_weapon_reason of the switch in progress, -1 = the game's own

struct ItemContext {
    int saved;

    explicit ItemContext(int reason) : saved(g_itemContext) { g_itemContext = reason; }

    ~ItemContext() { g_itemContext = saved; }
};

// ---- map object spawners and spawn timing -------------------------------------
// int ObjectSpawner::spawnObject() returns the new object's id (-1 if none). It
//   calls GameServer::spawnObject(tmpl, Pos3, Vec3) through bf::game +0x50 and,
//   only when that returns an object, calcSpawnDelay() (the respawn timer at
//   +0x120). On -1, handleFrameUpdate tries again the next frame, so a blocked
//   spawn sets the timer itself. Team at +0x134.
// IObject* GameServer::spawnObject(const IObjectTemplate*, Pos3, Vec3) - just
//   createObjectOnAllClients(..., 1, 0). RMM's own spawn_object doesn't use it.
// GameServer spawn fields (killPlayer, gameStatusPlaying): +0x201 waves on,
//   +0x204 delay (waves off), +0x214 time between waves (game.serverSpawnTime).
using SpawnerSpawnFn = int(R1_THIS*)(uintptr_t spawner);
using ServerSpawnObjectFn = uintptr_t(R1_THIS*)(void* gameServer, uintptr_t tmpl, float x, float y, float z, float yaw,
                                                float pitch, float roll);
using SpawnerCalcDelayFn = void(R1_THIS*)(uintptr_t spawner);
SpawnerSpawnFn g_spawnerSpawn = nullptr;
ServerSpawnObjectFn g_serverSpawnObject = nullptr;
SpawnerCalcDelayFn g_spawnerCalcDelay = nullptr;
uintptr_t g_spawnerContext = 0;  // the ObjectSpawner inside spawnObject(), 0 = none
bool g_spawnerBlocked = false;   // its spawn was blocked
uint32_t g_offSpawnerTimer = 0x120, g_offSpawnerTeam = 0x134;
uint32_t g_offSpawnWaves = 0x201, g_offSpawnDelay = 0x204, g_offSpawnWaveTime = 0x214;
constexpr float kBlockedSpawnerRetry = 5.0f;  // at least this long between two tries of a blocked spawner

int g_vehContext = -1;         // rmm_vehicle_reason of the enter/exit in progress, -1 = the game's own
bool g_switchBlocked = false;  // a seat switch's exit was blocked: swallow the enter that follows

struct VehicleContext {
    int saved;

    explicit VehicleContext(int reason) : saved(g_vehContext) { g_vehContext = reason; }

    ~VehicleContext() { g_vehContext = saved; }
};

uintptr_t ScoreManager() {
    uintptr_t sm = 0;

    return g_scoreManagerGlobal && elf::SafeReadT(g_scoreManagerGlobal, sm) ? sm : 0;
}

uintptr_t ObjectManager() {
    uintptr_t om = 0, vptr = 0;

    if (!g_objectManagerGlobal || !elf::SafeReadT(g_objectManagerGlobal, om) || !om)
        return 0;

    if (g_objectManagerVptr && (!elf::SafeReadT(om, vptr) || vptr != g_objectManagerVptr))
        return 0;

    return om;
}

/// Node of @p key in the std::map<unsigned, T> object at @p obj + @p off, or 0.
uintptr_t MapNode(uintptr_t obj, uint32_t off, uint32_t key) {
    constexpr r1abi::MapLayout L = r1abi::kMap;
    uintptr_t header = 0, node = 0;

    if (!obj || !elf::SafeReadT(obj + off + L.header, header) || !header || !elf::SafeReadT(header + L.root, node))
        return 0;

    for (int guard = 0; node && node != header && guard < 64; ++guard) {
        uint32_t k = 0;

        if (!elf::SafeReadT(node + L.key, k))
            return 0;

        if (k == key)
            return node;

        if (!elf::SafeReadT(node + (key < k ? L.left : L.right), node))
            return 0;
    }

    return 0;
}

/// Value stored under @p key in the object map at @p om + @p off, or 0.
uintptr_t MapFind(uintptr_t om, uint32_t off, uint32_t key) {
    uintptr_t node = MapNode(om, off, key), v = 0;

    return node && elf::SafeReadT(node + r1abi::kMap.value, v) ? v : 0;
}

/// Every value of the object map at @p om + @p off (unordered).
std::vector<uintptr_t> MapValues(uintptr_t om, uint32_t off) {
    constexpr r1abi::MapLayout L = r1abi::kMap;
    std::vector<uintptr_t> out, stack;
    uintptr_t header = 0, root = 0;

    if (!om || !elf::SafeReadT(om + off + L.header, header) || !header || !elf::SafeReadT(header + L.root, root))
        return out;

    if (root && root != header)
        stack.push_back(root);

    for (int guard = 0; !stack.empty() && guard < 131072; ++guard) {  // big maps: several thousand objects
        uintptr_t n = stack.back(), v = 0, l = 0, r = 0;
        stack.pop_back();

        if (!elf::SafeReadT(n + L.value, v) || !elf::SafeReadT(n + L.left, l) || !elf::SafeReadT(n + L.right, r))
            break;

        if (v)
            out.push_back(v);

        if (l && l != header)
            stack.push_back(l);

        if (r && r != header)
            stack.push_back(r);
    }

    return out;
}

uintptr_t RootOf(uintptr_t obj) {
    uintptr_t comp = Composite(obj);

    if (!comp)
        return 0;

    if (g_getRootParent) {
        uintptr_t r = g_getRootParent(comp);

        return r ? r : comp;
    }

    for (int guard = 0; guard < 32; ++guard) {  // same walk as getRootParent, without its cache
        uintptr_t parent = ParentOf(comp);

        if (!parent)
            return comp;

        comp = parent;
    }

    return comp;
}

uint32_t ObjectId(uintptr_t obj) {
    uint32_t id = 0;

    if (obj)
        elf::SafeReadT(obj + g_offObjectId, id);

    return id;
}

/// Public handle: object id in the high half, so a freed and reused address
/// never validates as the old object.
rmm_object_t Handle(uintptr_t obj) {
    uintptr_t c = Composite(obj);

    return c ? (static_cast<rmm_object_t>(ObjectId(c)) << 32) | static_cast<uint32_t>(c) : 0;
}

/// The live object behind a handle, or 0 if it no longer exists.
uintptr_t Resolve(rmm_object_t h) {
    uintptr_t ptr = static_cast<uintptr_t>(static_cast<uint32_t>(h));
    uint32_t id = static_cast<uint32_t>(h >> 32);
    uintptr_t om = ObjectManager();

    if (!ptr || !om)
        return 0;

    if (id) {
        uintptr_t v = MapFind(om, g_offOmPcoMap, id);

        if (v && Composite(v) == ptr)
            return ptr;

        v = MapFind(om, g_offOmObjects, id);

        if (v && Composite(v) == ptr)
            return ptr;

        // Any other root object (building, kit, spawned prop...): the grid maps.
        v = MapFind(om, g_offOmGrid, id);

        if (v && Composite(v) == ptr)
            return ptr;

        v = MapFind(om, g_offOmGridNoCollision, id);

        if (v && Composite(v) == ptr)
            return ptr;
    }

    // Roots that aren't PCOs themselves (e.g. a hull carrying gun seats) are
    // reached through their children. Only trust ptr once it's proven live.
    for (uintptr_t o : MapValues(om, g_offOmPcoMap)) {
        uintptr_t c = Composite(o);

        if ((c == ptr || RootOf(o) == ptr) && ObjectId(ptr) == id)
            return ptr;
    }

    return 0;
}

/// IPhysicsNode component of @p root, validated against the known implementations.
uintptr_t PhysicsNodeOf(uintptr_t root) {
    if (!g_objectModel)
        return 0;

    using Fn = uintptr_t(R1_THIS*)(uintptr_t self, unsigned a, unsigned b);
    auto qc = VirtualAt<Fn>(root, kVtQueryComponent);
    uintptr_t node = qc ? qc(root, kIidPhysicsNode, kIidPhysicsNode) : 0;
    uintptr_t vt = 0, fn = 0;

    if (!node || !elf::SafeReadT(node, vt) || !elf::SafeReadT(vt + kVtPhysSetSpeed, fn))
        return 0;

    for (size_t i = 0; i < g_physicsSetSpeedCount; ++i)
        if (fn == g_physicsSetSpeed[i])
            return node;

    return 0;
}

bool ReadVec(uintptr_t object, uint32_t slot, rmm_vec3& out) {
    if (!g_objectModel)
        return false;

    using Fn = const float*(R1_THIS*)(uintptr_t self);
    auto fn = VirtualAt<Fn>(object, slot);
    const float* p = fn ? fn(object) : nullptr;
    float v[3];

    if (!p || !elf::SafeRead(p, v, sizeof v))
        return false;

    out = rmm_vec3{v[0], v[1], v[2]};

    return true;
}

float ArmorFloat(uintptr_t armor, uint32_t slot) {
    if (!g_objectModel)
        return -1.0f;

    using Fn = float(R1_THIS*)(uintptr_t self);  // float returns in st(0) on i386, as GCC 3 does
    auto fn = armor ? VirtualAt<Fn>(armor, slot) : nullptr;

    return fn ? fn(armor) : -1.0f;
}

// Kit names: GameServer+0x4c is a GCC 3 std::map<int, std::string> filled by
// Game::setKit(team, kit, name) with key team*256 + kit (e.g. 256 German_Scout
// .. 516 Rus_Engineer); changeKit() looks it up the same way.
// The player's kit index is BFPlayer+0x80 and the team BFPlayer+0x7c.
// GCC 3 _Rb_tree: the map holds a header-node pointer; the header's parent is
// the root; nodes are {color, parent, left, right, key, value}.
// BFV keeps one such map per team (Game::getKits(team): team 1 -> +0x5c,
// otherwise +0x50; same keys): gamedata gameserver_kit_map_team1/_team2.
uint32_t g_offKitMap = 0x4c, g_offKitIndex = 0x80, g_offTeam = 0x7c;
uint32_t g_offKitMapTeam[3] = {0, 0, 0};  // 0 = use g_offKitMap

std::string KitName(int team, int kitIndex) {
    if (team <= 0 || kitIndex < 0 || kitIndex > 0xff)
        return {};

    const uint32_t wanted = static_cast<uint32_t>(team * 0x100 + kitIndex);  // keys are small and positive
    uint32_t off = (team <= 2 && g_offKitMapTeam[team]) ? g_offKitMapTeam[team] : g_offKitMap;
    uintptr_t node = MapNode(reinterpret_cast<uintptr_t>(g_gameServer), off, wanted);
    es::String s{};

    return node && elf::SafeReadT(node + r1abi::kMap.value, s) ? es::Read(&s) : std::string();
}

/// Kit slot (0..) of @p kitTemplate in @p team's spawn menu, or -1.
int KitSlot(int team, const std::string& kitTemplate) {
    for (int k = 0; k < 16; ++k) {
        std::string n = KitName(team, k);

        if (!n.empty() && IEquals(n, kitTemplate))
            return k;
    }

    return -1;
}

// The kit a soldier actually carries. BFSoldier::getKitId() is
// soldier+0x408 (-1 = none): the id of the kit object, which the engine looks
// up in the grid maps (objectManager getObjectFromGrid, then
// getObjectFromNoCollisionGrid; changeKit, dropKit, selectBestLoadedWeapon).
uint32_t g_offSoldierKitId = 0x408;  // gamedata soldier_kit_id

uintptr_t KitObjectById(uint32_t id) {
    uintptr_t om = ObjectManager();

    if (!om || id == 0xffffffffu)
        return 0;

    uintptr_t o = MapFind(om, g_offOmGrid, id);

    return o ? o : MapFind(om, g_offOmGridNoCollision, id);
}

uintptr_t SoldierKit(uintptr_t soldier) {
    uint32_t id = 0xffffffffu;

    return soldier && elf::SafeReadT(soldier + g_offSoldierKitId, id) ? KitObjectById(id) : 0;
}

/// A living player's kit is what their soldier carries (set_kit and pickups
/// can give kits that aren't in their team's spawn menu); otherwise the kit
/// chosen in the spawn menu (BFPlayer+0x80 in the team's kit map).
std::string PlayerKitName(const uint8_t* player) {
    uint8_t alive = 0;
    uintptr_t soldier = 0;

    if (player && elf::SafeRead(player + g_offAlive, &alive, 1) && alive &&
        elf::SafeReadT(reinterpret_cast<uintptr_t>(player) + g_offSoldier, soldier)) {
        if (uintptr_t kit = SoldierKit(soldier)) {
            std::string name = ObjectTemplateName(kit);

            if (!name.empty())
                return name;
        }
    }

    int32_t team = 0, kit = -1;

    if (!player || !elf::SafeReadT(reinterpret_cast<uintptr_t>(player) + g_offTeam, team) ||
        !elf::SafeReadT(reinterpret_cast<uintptr_t>(player) + g_offKitIndex, kit))
        return {};

    return KitName(team, kit);
}

int32_t ReadInt(const uint8_t* base, uint32_t off) {
    int32_t v = 0;

    if (base)
        elf::SafeRead(base + off, &v, sizeof v);

    return v;
}

/// Ping in ms via NetServer::getPing, or -1. Only called when the object at
/// GameServer+0x18c really is a NetServer (vptr check).
int32_t PlayerPing(int id) {
    uintptr_t net = 0, vptr = 0;

    if (!g_gameServer || !g_netServerVptr)
        return -1;

    if (!elf::SafeReadT(reinterpret_cast<uintptr_t>(g_gameServer) + g_offNetServer, net) || !net)
        return -1;

    if (!elf::SafeReadT(net, vptr) || vptr != g_netServerVptr)
        return -1;

    using GetPingFn = uint32_t(R1_THIS*)(uintptr_t self, int id, int* out);  // returns GCC 3 bool in %al
    auto getPing = VirtualAt<GetPingFn>(net, g_vtNetServerGetPing);
    int ping = -1;

    return getPing && LowByte(getPing(net, id, &ping)) ? ping : -1;
}

bool PlayerAlive(const uint8_t* player) {
    uint8_t alive = 0;

    return player && elf::SafeRead(player + g_offAlive, &alive, 1) && alive != 0;
}

Core* ReadyCore() {
    R1Adapter* a = R1Adapter::Instance();

    return a && a->CorePtr()->Ready() ? a->CorePtr() : nullptr;
}

/// Call @p fn(BFPlayer*) for every player until it returns true. Walks the
/// player list (≤ a few dozen entries).
template <class Fn> void ForEachPlayer(Fn fn) {
    uintptr_t pm = 0, sentinel = 0, node = 0;

    if (!g_playerManagerGlobal || !elf::SafeReadT(g_playerManagerGlobal, pm) || !pm)
        return;

    if (!elf::SafeReadT(pm + g_offPlayerList + r1abi::kListHeader, sentinel) || !sentinel ||
        !elf::SafeReadT(sentinel, node))
        return;

    for (int guard = 0; node && node != sentinel && guard < 512; ++guard) {
        uintptr_t player = 0;

        if (!elf::SafeReadT(node + 2 * sizeof(uintptr_t), player))
            return;

        if (player && fn(reinterpret_cast<uint8_t*>(player)))
            return;

        if (!elf::SafeReadT(node, node))
            return;
    }
}

uint8_t* PlayerById(int id) {
    uint8_t* found = nullptr;
    ForEachPlayer([&](uint8_t* p) { return PlayerId(p) == id ? (found = p, true) : false; });

    return found;
}

uintptr_t PlayerSoldier(const uint8_t* player) {
    uintptr_t soldier = 0;

    return player && elf::SafeReadT(reinterpret_cast<uintptr_t>(player) + g_offSoldier, soldier) ? soldier : 0;
}

/// Living players sitting anywhere in the vehicle whose root is @p root.
std::vector<rmm_player_t> OccupantsOfRoot(uintptr_t root) {
    std::vector<rmm_player_t> ids;

    if (!root)
        return ids;

    ForEachPlayer([&](uint8_t* p) {
        uintptr_t v = PlayerAlive(p) ? PlayerVehicle(p) : 0;

        if (v && RootOf(v) == root)
            ids.push_back(PlayerId(p));

        return false;
    });

    return ids;
}

/// Seat objects of the vehicle whose root is @p root: the
/// player-control objects (ObjectManager's PCO map) under it, with their
/// position number (the "pco_id" checkPlayerTriggers compares with F1-F8,
/// vtable +0x2c), lowest first.
std::vector<std::pair<int, uintptr_t>> SeatsOf(uintptr_t root) {
    std::vector<std::pair<int, uintptr_t>> seats;

    for (uintptr_t o : MapValues(ObjectManager(), g_offOmPcoMap)) {
        uintptr_t c = Composite(o);

        if (c && RootOf(c) == root && !IsSoldier(c))
            seats.emplace_back(SeatOf(c), c);
    }

    std::sort(seats.begin(), seats.end());

    return seats;
}

/// The living player sitting in this very seat object, or -1.
int SeatOccupant(uintptr_t seat) {
    int id = -1;
    ForEachPlayer([&](uint8_t* p) {
        uintptr_t v = PlayerAlive(p) ? PlayerVehicle(p) : 0;

        return v && Composite(v) == seat ? (id = PlayerId(p), true) : false;
    });

    return id;
}

// players->damage() called while the engine is inside _giveDamage (e.g. from
// a PLAYER_DAMAGE handler reflecting friendly fire) is applied at the end of
// the frame instead of re-entering _giveDamage.
int g_giveDamageDepth = 0;

struct PendingDamage {
    int victim;
    float amount;
    int attacker;
};

std::vector<PendingDamage> g_pendingDamage;

// world->damage_object waits the same way, and goes through
// Hook_GiveDamage for its destroyed detection with the damage events skipped
// (the hook clears the flag on entry, so nested hits still raise theirs).
struct PendingObjectDamage {
    rmm_object_t object;
    float amount;
    int attacker;
};

std::vector<PendingObjectDamage> g_pendingObjectDamage;
bool g_skipDamageEvents = false;

// set_kit from inside _giveDamage (a PLAYER_KILLED or PLAYER_DAMAGE handler:
// kills are scored in there) waits for the end of the frame as well; creating
// objects in the middle of the engine's damage code is not something it does.
struct PendingKit {
    int player;
    std::string kit;
};

std::vector<PendingKit> g_pendingKits;

// enter_vehicle / exit_vehicle from inside damage code wait the same way.
struct PendingVehicleOp {
    int player;
    rmm_object_t vehicle;
    int seat;
    bool enter;
};

std::vector<PendingVehicleOp> g_pendingVehicleOps;

// Every console line (scripts, server console, remote admin). RMM handles its
// own commands here and the engine never sees them; everything else passes
// through untouched. Lines RMM itself executes (depth > 0) are not intercepted.
void R1_HOOK Hook_OldExecuteLine(void* console R1_EDX, es::String* line, es::String* out, int b3, int b4, int b5,
                                 int b6, int b7) {
    R1Adapter* a = R1Adapter::Instance();

    if (a && g_rmmExecDepth == 0 && a->CorePtr()->Ready()) {
        std::string text = es::Read(line);
        std::string reply;

        // TODO(verify live): tell the server console, remote admin and script
        // lines apart (they all arrive here). Treated as console for now.
        if (a->CorePtr()->OnConsoleCommand(RMM_SRC_CONSOLE, RMM_PLAYER_NONE, text, &reply)) {
            if (out)
                es::Assign(out, reply);

            // The engine prints results from inside executeLine (`out` alone
            // shows nothing), so print through the console that
            // received the line - OldConsole::output (vtable +0x10, Windows +0xc).
            using OutputFn = void(R1_THIS*)(void* self, es::String* line);

            if (auto output = VirtualAt<OutputFn>(reinterpret_cast<uintptr_t>(console), g_vtConsoleOutput)) {
                if (!reply.empty() && reply.back() != '\n')
                    reply += '\n';

                es::Scoped s(reply.c_str());
                ++g_printBypass;
                output(console, s.get());
                --g_printBypass;
            }

            return;
        }
    }

    g_oldExecuteLine(console, line, out, b3, b4, b5, b6, b7);
}

// OldConsole::output: while RMM's own Exec asked for the result, collect the
// text instead of printing it (GetEngineVar, maplist, `rcon` from chat...).
void R1_HOOK Hook_ConsoleOutput(void* console R1_EDX, es::String* line) {
    if (g_captureOut && g_rmmExecDepth > 0 && g_printBypass == 0) {
        *g_captureOut += es::Read(line);

        return;
    }

    g_consoleOutput(console, line);
}

// GameServer::destroyPlayer: the slot is freed here, before any reuse, so this
// is the only safe moment to forget the player's CD-key hash. (Erasing it from
// the 0.5 s player poll would race with the next occupant's auth callback.)
int R1_HOOK Hook_DestroyPlayer(void* gameServer R1_EDX, uint8_t* player) {
    CaptureGameServer(gameServer, g_thisDestroyPlayer);
    int32_t id = -1;

    if (player && elf::SafeReadT(reinterpret_cast<uintptr_t>(player) + g_offId, id) && id >= 0)
        g_authIds.erase(id);

    return g_destroyPlayer(gameServer, player);
}

// AIPlayer::addInput: every bot input tick. Records what the engine's AI wants
// (bots->get_input) and, for bots a plugin controls, hands the engine a
// modified copy. The AI's own record is never written: BotMain keeps state in
// it between ticks.
void R1_HOOK Hook_AddInput(uintptr_t aiPlayer R1_EDX, uint8_t* input) {
    Core* core = ReadyCore();
    uintptr_t player = 0;

    if (!core || !input || !aiPlayer || !elf::SafeReadT(aiPlayer, player) || !player)
        return g_addInput(aiPlayer, input);

    const uint32_t slots = std::min<uint32_t>(g_inputSlots, RMM_BOT_INPUT_SLOTS);
    const uint64_t valid = slots >= 64 ? ~0ull : ((1ull << slots) - 1);
    BotControl::Frame f;
    std::memcpy(f.value, input, slots * sizeof(float));
    std::memcpy(&f.mask, input + g_offInputMask, sizeof f.mask);
    f.mask &= valid;

    if (!core->BotInput().Filter(PlayerId(reinterpret_cast<uint8_t*>(player)), f, core->Time()))
        return g_addInput(aiPlayer, input);

    alignas(8) uint8_t copy[0x200];
    std::memcpy(copy, input, std::min<uint32_t>(g_inputSize, sizeof copy));
    std::memcpy(copy, f.value, slots * sizeof(float));
    uint64_t mask = f.mask & valid;
    std::memcpy(copy + g_offInputMask, &mask, sizeof mask);
    g_addInput(aiPlayer, copy);
}

// See ChangeWeaponExecFn. Re-resolves the cached item; if the slot is empty now
// (the new kit has no such item) the action ends, as it does when the bot has
// no soldier. Anything unreadable falls through to the original.
// Slots/fields (gamedata cw_*): Bot vfunc info handle, soldier-interface vfunc
// item-in-slot, the action's slot and item fields. Windows: 0xcc, 0x18, 0x1c, 0x20.
uint32_t g_cwBotHandle = 0xd0, g_cwSoldierItem = 0x1c, g_cwSlot = 0x18, g_cwItem = 0x1c;

uint32_t R1_HOOK Hook_ChangeWeaponExec(uintptr_t self R1_EDX, uintptr_t bot, bool* running, bool* done) {
    using VGetU32 = uint32_t(R1_THIS*)(uintptr_t);
    using VGetItem = uintptr_t(R1_THIS*)(uintptr_t, int32_t);
    uintptr_t botVt = 0, getHandle = 0, imTable = 0, imPtr = 0;

    if (!self || !bot || !g_aiInfoManager || !elf::SafeReadT(bot, botVt) || !botVt ||
        !elf::SafeReadT(botVt + g_cwBotHandle, getHandle) || !getHandle || !elf::SafeReadT(g_aiInfoManager, imPtr) ||
        !imPtr || !elf::SafeReadT(imPtr, imTable) || !imTable)
        return g_changeWeaponExec(self, bot, running, done);

    const uint32_t handle = reinterpret_cast<VGetU32>(getHandle)(bot);

    if ((handle & 0xffff) == 0)
        return g_changeWeaponExec(self, bot, running, done);

    const uintptr_t entry = imTable + (handle & 0xffff) * 8;
    uint16_t serial = 0;
    uintptr_t info = 0, soldier = 0, soldierVt = 0, getItem = 0;
    int32_t slot = 0;

    if (!elf::SafeReadT(entry - 2, serial) || serial != (handle >> 16) || !elf::SafeReadT(entry - 8, info) || !info ||
        info == static_cast<uintptr_t>(-0x24) || !elf::SafeReadT(info + 0x34, soldier) || !soldier ||
        !elf::SafeReadT(soldier, soldierVt) || !soldierVt || !elf::SafeReadT(soldierVt + g_cwSoldierItem, getItem) ||
        !getItem || !elf::SafeReadT(self + g_cwSlot, slot))
        return g_changeWeaponExec(self, bot, running, done);

    const uintptr_t item = reinterpret_cast<VGetItem>(getItem)(soldier, slot);
    uintptr_t tmpl = 0;
    uint32_t input = 0;

    if (!item || !elf::SafeReadT(item + 0xc, tmpl) || !tmpl || !elf::SafeReadT(tmpl + 0x40, input)) {
        if (running)
            *running = false;

        if (done)
            *done = true;

        return 1;
    }

    *reinterpret_cast<uintptr_t*>(self + g_cwItem) = item;
    const uint64_t mask = input < 64 ? (1ull << input) : 0;  // the ctor's input bit at +0x8
    std::memcpy(reinterpret_cast<void*>(self + 0x8), &mask, sizeof mask);

    return g_changeWeaponExec(self, bot, running, done);
}

void R1_HOOK Hook_StatusPreGame(void* gameServer R1_EDX, float dt) {
    CaptureGameServer(gameServer);
    {
        HoldBotSlots hold(gameServer);
        g_statusPreGame(gameServer, dt);
    }

    if (R1Adapter* a = R1Adapter::Instance())
        a->OnGameStatus(RMM_STATUS_PREGAME);
}

// BFV has no gameStatusFirstPlaying: gameStatusPlaying(float) runs every
// playing frame, and OnGameStatus only reports the change.
void R1_HOOK Hook_StatusPlaying(void* gameServer R1_EDX, float dt) {
    CaptureGameServer(gameServer);
    {
        HoldBotSlots hold(gameServer);
        g_statusPlaying(gameServer, dt);
    }

    if (R1Adapter* a = R1Adapter::Instance())
        a->OnGameStatus(RMM_STATUS_PLAYING);
}

void R1_HOOK Hook_StatusFirstPlaying(void* gameServer R1_EDX) {
    CaptureGameServer(gameServer);
    {
        HoldBotSlots hold(gameServer);
        g_statusFirstPlaying(gameServer);
    }

    if (R1Adapter* a = R1Adapter::Instance())
        a->OnGameStatus(RMM_STATUS_PLAYING);
}

void R1_HOOK Hook_StatusFirstEndGame(void* gameServer R1_EDX) {
    CaptureGameServer(gameServer);
    g_statusFirstEndGame(gameServer);

    if (R1Adapter* a = R1Adapter::Instance())
        a->OnGameStatus(RMM_STATUS_ENDGAME);
}

void R1_HOOK Hook_HandleChat(void* gameServer R1_EDX, uint8_t* msg) {
    CaptureGameServer(gameServer);
    R1Adapter* a = R1Adapter::Instance();
    int32_t sender = -1, kind = 1;
    std::memcpy(&sender, msg + g_chat.sender, sizeof sender);
    std::memcpy(&kind, msg + g_chat.kind, sizeof kind);

    // Only real player chat; server messages (say(-1,...), flag set) pass through.
    if (a && a->CorePtr()->Ready() && sender >= 0 && msg[g_chat.serverFlag] == 0) {
        char* text = reinterpret_cast<char*>(msg + g_chat.text);
        std::string original(text, strnlen(text, g_chat.textCap));
        std::string line = original;

        if (a->CorePtr()->OnChat(sender, kind == 0 ? RMM_CHAT_TEAM : RMM_CHAT_ALL, line))
            return;  // blocked

        if (line != original) {
            size_t n = std::min<size_t>(line.size(), g_chat.textCap - 1);
            std::memset(text, 0, g_chat.textCap);
            std::memcpy(text, line.data(), n);
            uint32_t len = static_cast<uint32_t>(n);
            std::memcpy(msg + g_chat.length, &len, sizeof len);
        }
    }

    g_handleChat(gameServer, msg);
}

#ifdef _WIN32
void __fastcall Hook_CdKeyAuth(int gameId, int localId, int authenticated, char* errmsg, void* instance) {
#else
void Hook_CdKeyAuth(int gameId, int localId, int authenticated, char* errmsg, void* instance) {
#endif
    g_cdKeyAuth(gameId, localId, authenticated, errmsg, instance);
    CaptureGameServer(instance);

    if (localId < 0 || !authenticated)
        return;  // -1 = the server's own key check

    std::string hash;

    if (g_getKeyHash) {
        const char* h = g_getKeyHash(gameId, localId);

        if (h)
            hash = h;
    }

    // Recorded even if empty (CD-key checking disabled): auth still completed.
    g_authIds[localId] = hash;
}

// Spawn: success is visible only as player+0x79 (alive) going 0 -> 1.
int R1_HOOK Hook_Spawn(void* gameServer R1_EDX, uint8_t* player) {
    CaptureGameServer(gameServer);
    bool wasAlive = PlayerAlive(player);
    int r;
    {
        KitContext kc(RMM_KIT_REASON_SPAWN);  // the spawn-menu kit is handed out in here
        r = g_spawn(gameServer, player);
    }

    Core* core = ReadyCore();

    if (core && !wasAlive && PlayerAlive(player)) {
        rmm_vec3 pos{0, 0, 0};
        uintptr_t soldier = 0;

        if (elf::SafeRead(player + g_offSoldier, &soldier, sizeof soldier) && soldier)
            AbsolutePosition(soldier, pos);

        std::string kit = PlayerKitName(player);
        rmm_ev_player_spawn ev{PlayerId(player), pos, kit.c_str()};
        core->Dispatch(RMM_EV_PLAYER_SPAWN, &ev);
    }

    return r;
}

// Vehicles: report only real changes of the controlled object, ignoring the
// player's own soldier (isDefault on spawn) and "fake" transitions.
// PRE handlers run before the engine acts and can keep the player
// out (enterVehicle then returns 0, its own failure result, without touching
// anything); POST handlers run once the player is in.
// The reason comes from the context (see VehicleContext). A seat
// switch whose exit was blocked must not go on to enter the new seat.
int R1_HOOK Hook_EnterVehicle(void* gameServer R1_EDX, uint8_t* player, void* object, int isDefault, int isFake) {
    const int reason = g_vehContext >= 0 ? g_vehContext : RMM_VEHICLE_REASON_GAME;

    if (reason == RMM_VEHICLE_REASON_SWITCH && g_switchBlocked)
        return 0;

    uintptr_t before = PlayerVehicle(player);
    Core* core = ReadyCore();
    const bool real = core && object && !(isDefault & 0xff) && !(isFake & 0xff);
    std::string tmpl, rootTmpl;
    rmm_ev_vehicle ev{};

    if (real) {
        uintptr_t target = Composite(reinterpret_cast<uintptr_t>(object));
        uintptr_t root = RootOf(target);
        tmpl = ObjectTemplateName(target);
        rootTmpl = root ? ObjectTemplateName(root) : tmpl;
        // Seat = the "pco_id" the engine logs: IPlayerControlObject found by
        // walking up from the entered object, vtable +0x2c. 0 = driver (VERIFY).
        int seat = SeatOf(reinterpret_cast<uintptr_t>(object));
        ev = rmm_ev_vehicle{PlayerId(player),
                            Handle(target),
                            tmpl.c_str(),
                            seat,
                            seat == 0 ? RMM_TRUE : RMM_FALSE,
                            Handle(root ? root : target),
                            rootTmpl.c_str(),
                            reason};
        // A switch's exit already happened (blocking its enter would leave the
        // player on foot), and plugins' own calls are final.
        bool canBlock = reason != RMM_VEHICLE_REASON_SWITCH && reason != RMM_VEHICLE_REASON_PLUGIN;

        if (core->Events().DispatchPre(RMM_EV_VEHICLE_ENTER, &ev, canBlock) >= RMM_HANDLED)
            return 0;  // kept out
    }

    int r = g_enterVehicle(gameServer, player, object, isDefault, isFake);
    uintptr_t after = PlayerVehicle(player);

    if (real && after && after != before) {
        // Report what the engine actually put the player in.
        if (Handle(after) != ev.vehicle) {
            tmpl = ObjectTemplateName(after);
            ev.vehicle = Handle(after);
            ev.vehicle_template = tmpl.c_str();
        }

        core->Events().DispatchPost(RMM_EV_VEHICLE_ENTER, &ev);
    }

    return r;
}

// PRE before the engine acts. Blockable only when the player chose
// to leave (use key, seat switch): every caller of those paths treats a 0
// result as "didn't happen" (toggleEntryPoint returns; the switch's enter is
// swallowed in Hook_EnterVehicle). The game's own exits are never blocked.
int R1_HOOK Hook_ExitVehicle(void* gameServer R1_EDX, uint8_t* player, int isFake) {
    const int reason = g_vehContext >= 0 ? g_vehContext : RMM_VEHICLE_REASON_GAME;
    uintptr_t before = PlayerVehicle(player);
    Core* core = ReadyCore();
    const bool real = core && before && !(isFake & 0xff);
    // Read everything while the object is certainly still alive.
    std::string tmpl = real ? ObjectTemplateName(before) : std::string();
    uintptr_t root = real ? RootOf(before) : 0;
    std::string rootTmpl = root ? ObjectTemplateName(root) : tmpl;
    rmm_object_t handle = Handle(before), rootHandle = root ? Handle(root) : handle;
    int seat = real ? SeatOf(before) : -1;
    rmm_ev_vehicle ev{PlayerId(player), handle,           tmpl.c_str(), seat, seat == 0 ? RMM_TRUE : RMM_FALSE,
                      rootHandle,       rootTmpl.c_str(), reason};

    if (real) {
        bool canBlock = reason == RMM_VEHICLE_REASON_PLAYER || reason == RMM_VEHICLE_REASON_SWITCH;

        if (core->Events().DispatchPre(RMM_EV_VEHICLE_EXIT, &ev, canBlock) >= RMM_HANDLED) {
            if (reason == RMM_VEHICLE_REASON_SWITCH)
                g_switchBlocked = true;

            return 0;  // kept in
        }
    }

    int r = g_exitVehicle(gameServer, player, isFake);

    if (real && PlayerVehicle(player) != before)
        core->Events().DispatchPost(RMM_EV_VEHICLE_EXIT, &ev);

    return r;
}

void R1_HOOK Hook_CheckPlayerTriggers(void* gameServer R1_EDX, uint8_t* player, PlayerInputBlob input) {
    // Without the toggleEntryPoint hook a use-key exit would pass for a switch.
    if (!g_triggersHooked)
        return g_checkTriggers(gameServer, player, input);

    VehicleContext vc(RMM_VEHICLE_REASON_SWITCH);  // what F1-F8 do; the use key overrides it below
    g_switchBlocked = false;
    g_checkTriggers(gameServer, player, input);
    g_switchBlocked = false;
}

void R1_HOOK Hook_ToggleEntryPoint(void* gameServer R1_EDX, uint8_t* player, int flag) {
    // From checkPlayerTriggers: the use key. From (re)spawnPlayer: spawning into a vehicle.
    VehicleContext vc(g_vehContext == RMM_VEHICLE_REASON_SWITCH ? RMM_VEHICLE_REASON_PLAYER : RMM_VEHICLE_REASON_SPAWN);
    g_toggleEntry(gameServer, player, flag);
}

// Kills and deaths come from the scoring code. Observe only - never suppress.
void R1_HOOK Hook_HandleScore(void* gameServer R1_EDX, uint8_t* player, int type, uint8_t* victim, int weaponId,
                              int extra) {
    int32_t scoreBefore = ReadInt(player, g_offScore);
    g_handleScore(gameServer, player, type, victim, weaponId, extra);
    Core* core = ReadyCore();

    if (!core)
        return;

    int32_t scoreAfter = ReadInt(player, g_offScore);

    if (scoreAfter != scoreBefore && PlayerId(player) >= 0) {
        rmm_ev_player_score sev{PlayerId(player), scoreAfter - scoreBefore, scoreAfter};
        core->Dispatch(RMM_EV_PLAYER_SCORE, &sev);
    }

    if ((type == kKill || type == kTK) && victim) {
        int victimId = PlayerId(victim), attackerId = PlayerId(player);

        if (victimId < 0)
            return;

        g_pendingDeaths.erase(victimId);
        g_killedThisTick.insert(victimId);
        std::string weapon = WeaponName(weaponId);
        // The engine scores killing yourself as TK (own grenade -> ScoreMsg TK
        // with attacker == victim); report it as a suicide only.
        bool suicide = attackerId == victimId;
        rmm_ev_player_killed ev{victimId,
                                attackerId,
                                weapon.c_str(),
                                "",
                                type == kTK && !suicide ? RMM_TRUE : RMM_FALSE,
                                suicide ? RMM_TRUE : RMM_FALSE};
        core->Dispatch(RMM_EV_PLAYER_KILLED, &ev);
    } else if (type == kDeath || type == kDeathNoMsg) {
        // The engine also scores a Death for the victim of a Kill/TK, after it.
        // Only deaths without a killer become world deaths.
        int victimId = PlayerId(player);

        if (victimId >= 0 && !g_killedThisTick.count(victimId))
            g_pendingDeaths[victimId] = WeaponName(weaponId);
    }
}

// Control points: gotControl(team) -> setTeam(team), lostControl() -> setTeam(0).
// setTeam is a no-op for locked points, so compare the team field around it.
//
// Who took it: ControlPoint::handleFrameUpdate decides with a plain radius
// test - living players (BFPlayer+0x79) whose controlled object
// (BFPlayer vfunc +0x3c, getVehicle) is within template+0x1dc of the point's
// getAbsolutePosition (+0x38) - and then scores every such player of the
// capturing team. RMM repeats that test right after setTeam.
uint32_t g_offCpRadius = 0x1dc;

std::vector<rmm_player_t> PlayersAtPoint(uintptr_t cp, uintptr_t tmpl, int32_t oldTeam, int32_t newTeam) {
    std::vector<rmm_player_t> ids;
    float radius = 0;
    rmm_vec3 at{0, 0, 0};

    if (!tmpl || !elf::SafeReadT(tmpl + g_offCpRadius, radius) || !(radius > 0) || !AbsolutePosition(cp, at))
        return ids;

    ForEachPlayer([&](uint8_t* p) {
        if (!PlayerAlive(p))
            return false;

        int32_t team = ReadInt(p, g_offTeam);
        // Captured: the new owner's players. Neutralised: the attackers.
        bool wanted = newTeam != 0 ? team == newTeam : team != 0 && team != oldTeam;
        uintptr_t controlled = 0;
        elf::SafeReadT(reinterpret_cast<uintptr_t>(p) + g_offVehicle, controlled);
        rmm_vec3 pos{0, 0, 0};

        if (wanted && controlled && AbsolutePosition(controlled, pos)) {
            float dx = pos.x - at.x, dy = pos.y - at.y, dz = pos.z - at.z;

            if (dx * dx + dy * dy + dz * dz <= radius * radius)
                ids.push_back(PlayerId(p));
        }

        return false;
    });

    return ids;
}

void CpTeamChanged(uint8_t* cp, int32_t before);

void R1_HOOK Hook_CpSetTeam(uint8_t* cp R1_EDX, unsigned team) {
    int32_t before = ReadInt(cp, g_offCpTeam);
    g_cpSetTeam(cp, team);
    CpTeamChanged(cp, before);
}

void R1_HOOK Hook_CpGotControl(uint8_t* cp R1_EDX, unsigned team) {
    int32_t before = ReadInt(cp, g_offCpTeam);
    g_cpGotControl(cp, team);
    CpTeamChanged(cp, before);
}

void R1_HOOK Hook_CpLostControl(uint8_t* cp R1_EDX, unsigned team) {
    int32_t before = ReadInt(cp, g_offCpTeam);
    g_cpLostControl(cp, team);
    CpTeamChanged(cp, before);
}

bool CpLocked(uintptr_t cp) {
    for (int i = 0; i < g_cpLockCount; ++i)
        if (g_cpLocks[i] == cp)
            return true;

    return false;
}

/// Locked points skip the engine's capture logic entirely, so nothing changes
/// there: no progress, no neutralising, no capture score.
void R1_HOOK Hook_CpFrameUpdate(uint8_t* cp R1_EDX, float dt) {
    if (g_cpLockCount && CpLocked(reinterpret_cast<uintptr_t>(cp)))
        return;

    g_cpFrameUpdate(cp, dt);
}

/// CONTROL_POINT when the team field changed across the engine call.
void CpTeamChanged(uint8_t* cp, int32_t before) {
    int32_t after = ReadInt(cp, g_offCpTeam);
    Core* core = ReadyCore();

    if (!core || after == before || g_cpQuiet)
        return;

    uintptr_t tmpl = 0;
    elf::SafeReadT(reinterpret_cast<uintptr_t>(cp) + g_offCpTemplate, tmpl);
    std::string name = tmpl ? TemplateName(tmpl) : std::string();
    std::vector<rmm_player_t> who = PlayersAtPoint(reinterpret_cast<uintptr_t>(cp), tmpl, before, after);
    rmm_ev_control_point ev{
        Handle(reinterpret_cast<uintptr_t>(cp)), name.c_str(), before, after, static_cast<int32_t>(who.size()),
        who.empty() ? nullptr : who.data()};
    core->Dispatch(RMM_EV_CONTROL_POINT, &ev);
}

/// Player whose soldier is @p object, or -1.
int PlayerIdForSoldier(uintptr_t object) {
    int id = -1;

    if (object)
        ForEachPlayer([&](uint8_t* p) { return PlayerSoldier(p) == object ? (id = PlayerId(p), true) : false; });

    return id;
}

/// Player id behind the IPlayer* the pickup code passes (a BFPlayer for every
/// caller seen; fall back to the virtual getId, vtable +0x48 (Windows +0x44), like the engine).
uint32_t g_vtPlayerGetId = 0x48;

int PickupPlayerId(uint8_t* player) {
    int id = PlayerId(player);

    if (id >= 0 && PlayerById(id) == player)
        return id;

    using GetIdFn = int(R1_THIS*)(uint8_t* self);
    auto getId = player ? VirtualAt<GetIdFn>(reinterpret_cast<uintptr_t>(player), g_vtPlayerGetId) : nullptr;

    return getId ? (getId(player) & 0xff) : -1;
}

// KIT_PICKUP. Blockable only for a pickup from the ground: at spawn,
// in changeKit and in RMM's own set_kit the caller goes on to destroy the old
// kit, so skipping the hand-over there would leave the soldier unarmed.
// Blocked: nothing happens, the kit stays where it lies.
void R1_HOOK Hook_HandlePickup(void* gameServer R1_EDX, uintptr_t item, uint8_t* player, uintptr_t soldier, int flag) {
    CaptureGameServer(gameServer);
    Core* core = ReadyCore();

    if (!core || !item || ObjectClassId(item) != g_kitClassId)
        return g_handlePickup(gameServer, item, player, soldier, flag);

    const int reason = g_kitContext >= 0 ? g_kitContext : RMM_KIT_REASON_PICKUP;
    std::string kit = ObjectTemplateName(item);
    rmm_ev_player_kit ev{PickupPlayerId(player), kit.c_str(), Handle(item), reason};

    if (core->Events().DispatchPre(RMM_EV_PLAYER_KIT_PICKUP, &ev, reason == RMM_KIT_REASON_PICKUP) >= RMM_HANDLED) {
        core->Log().Debug("kit pickup blocked: #%d %s", ev.player, kit.c_str());

        return;
    }

    {
        KitContext kc(reason);  // the old kit dropped in here is dropped for the same reason
        g_handlePickup(gameServer, item, player, soldier, flag);
    }

    core->Events().DispatchPost(RMM_EV_PLAYER_KIT_PICKUP, &ev);
}

void R1_HOOK Hook_ChangeKit(void* gameServer R1_EDX, uint8_t* player, int kit) {
    CaptureGameServer(gameServer);
    KitContext kc(RMM_KIT_REASON_CHANGE);
    g_changeKit(gameServer, player, kit);
}

// ---- items ----------------------------------------------------------------------

/// Number of item slots of a soldier (its template's), 0 if unreadable.
int ItemSlotCount(uintptr_t soldier) {
    uintptr_t tmpl = 0;
    int32_t n = 0;

    if (!soldier || !elf::SafeReadT(soldier + g_offTemplate, tmpl) || !tmpl ||
        !elf::SafeReadT(tmpl + g_offSoldierTemplateSlots, n) || n < 0 || n > 32)
        return 0;

    return n;
}

/// The slot the soldier holds, -1 if none.
int ActiveItemSlot(uintptr_t soldier) {
    int32_t s = -1;

    return soldier && elf::SafeReadT(soldier + g_offSoldierActiveItem, s) ? s : -1;
}

/// The item object in @p slot, 0 if empty. Checks the id table first, as enableItem does.
uintptr_t ItemAt(uintptr_t soldier, int slot) {
    uintptr_t ids = 0;
    int32_t id = -1;

    if (!g_getItem || slot < 1 || slot >= ItemSlotCount(soldier) ||
        !elf::SafeReadT(soldier + g_offSoldierItemIds, ids) || !ids ||
        !elf::SafeReadT(ids + static_cast<uintptr_t>(slot) * 4, id) || id == -1)
        return 0;

    return g_getItem(soldier + g_thisItemContainer, slot);
}

std::string ItemName(uintptr_t soldier, int slot) {
    uintptr_t item = slot >= 0 ? ItemAt(soldier, slot) : 0;

    return item ? ObjectTemplateName(item) : std::string();
}

/// Shared by both enable hooks: raise WEAPON_SWITCH around the engine's switch.
void ItemSwitch(EnableItemFn original, uintptr_t soldier, int slot, int reason) {
    slot = static_cast<signed char>(slot);
    Core* core = ReadyCore();
    int from = ActiveItemSlot(soldier);

    if (!core || !g_itemsHooked || slot == from || slot < 1 || !ItemAt(soldier, slot))
        return original(soldier, slot);

    int player = PlayerIdForSoldier(soldier);

    if (player < 0)
        return original(soldier, slot);

    std::string fromName = ItemName(soldier, from), toName = ItemName(soldier, slot);
    rmm_ev_weapon_switch ev{player, from, slot, fromName.c_str(), toName.c_str(), reason};
    bool canBlock = reason == RMM_WEAPON_REASON_PLAYER || reason == RMM_WEAPON_REASON_NETWORK;

    if (core->Events().DispatchPre(RMM_EV_PLAYER_WEAPON_SWITCH, &ev, canBlock) >= RMM_HANDLED) {
        core->Log().Debug("weapon switch blocked: #%d %d:%s -> %d:%s (reason %d)", player, from, fromName.c_str(), slot,
                          toName.c_str(), reason);

        return;
    }

    original(soldier, slot);

    if (ActiveItemSlot(soldier) == slot)
        core->Events().DispatchPost(RMM_EV_PLAYER_WEAPON_SWITCH, &ev);
}

void R1_HOOK Hook_EnableItem(uintptr_t soldier R1_EDX, int slot) {
    ItemSwitch(g_enableItem, soldier, slot, g_itemContext >= 0 ? g_itemContext : RMM_WEAPON_REASON_GAME);
}

void R1_HOOK Hook_EnableItemNet(uintptr_t soldier R1_EDX, int slot) {
    ItemSwitch(g_enableItemNet, soldier, slot,
               g_itemContext == RMM_WEAPON_REASON_PLUGIN ? g_itemContext : RMM_WEAPON_REASON_NETWORK);
}

// Weapon keys arrive as messages 10..17 (slots 1..8); fire, reload etc. can
// switch too (detonator after an explosive pack): those are the game's.
void R1_HOOK Hook_SoldierMessage(uintptr_t soldier R1_EDX, int msg, void* player) {
    ItemContext ic(msg >= 10 && msg <= 17 ? RMM_WEAPON_REASON_PLAYER : RMM_WEAPON_REASON_GAME);
    g_soldierMessage(soldier, msg, player);
}

// SPAWNER_SPAWN: ObjectSpawner::spawnObject marks the context, the GameServer
// call inside it raises the event (that's where template and position are).
int R1_HOOK Hook_SpawnerSpawn(uintptr_t spawner R1_EDX) {
    uintptr_t savedCtx = g_spawnerContext;
    bool savedBlocked = g_spawnerBlocked;
    g_spawnerContext = spawner;
    g_spawnerBlocked = false;
    int id = g_spawnerSpawn(spawner);

    if (g_spawnerBlocked) {
        // No object, so the engine didn't restart the respawn timer: do it, or it retries every frame.
        if (g_spawnerCalcDelay)
            g_spawnerCalcDelay(spawner);

        float t = 0.0f;

        if (elf::SafeReadT(spawner + g_offSpawnerTimer, t) && !(t >= kBlockedSpawnerRetry))
            *reinterpret_cast<float*>(spawner + g_offSpawnerTimer) = kBlockedSpawnerRetry;
    }

    g_spawnerContext = savedCtx;
    g_spawnerBlocked = savedBlocked;

    return id;
}

uintptr_t R1_HOOK Hook_ServerSpawnObject(void* gs R1_EDX, uintptr_t tmpl, float x, float y, float z, float yaw,
                                         float pitch, float roll) {
    Core* core = ReadyCore();
    uintptr_t spawner = g_spawnerContext;

    if (!core || !spawner)
        return g_serverSpawnObject(gs, tmpl, x, y, z, yaw, pitch, roll);

    g_spawnerContext = 0;  // only the spawner's own call
    CaptureGameServer(gs);
    std::string name = TemplateName(tmpl);
    int32_t team = 0;
    elf::SafeReadT(spawner + g_offSpawnerTeam, team);
    rmm_ev_spawner ev{Handle(spawner), name.c_str(), rmm_vec3{x, y, z}, rmm_vec3{yaw, pitch, roll}, team, 0};

    if (core->Events().DispatchPre(RMM_EV_SPAWNER_SPAWN, &ev, true) >= RMM_HANDLED) {
        g_spawnerBlocked = true;

        return 0;  // the engine's own "could not create" path
    }

    uintptr_t obj = g_serverSpawnObject(gs, tmpl, x, y, z, yaw, pitch, roll);
    ev.object = obj ? Handle(obj) : 0;
    core->Events().DispatchPost(RMM_EV_SPAWNER_SPAWN, &ev);

    return obj;
}

// KIT_DROP (observe-only): read the kit before it leaves the soldier.
uint32_t R1_HOOK Hook_DropKit(uintptr_t soldier R1_EDX) {
    Core* core = ReadyCore();
    uintptr_t kitObj = core ? SoldierKit(soldier) : 0;

    if (!kitObj)
        return g_dropKit(soldier);

    std::string kit = ObjectTemplateName(kitObj);
    rmm_object_t handle = Handle(kitObj);
    int player = PlayerIdForSoldier(soldier);
    uint32_t r = g_dropKit(soldier);

    if (player >= 0 && !kit.empty()) {
        rmm_ev_player_kit ev{player, kit.c_str(), handle, g_kitContext >= 0 ? g_kitContext : RMM_KIT_REASON_OTHER};
        core->Dispatch(RMM_EV_PLAYER_KIT_DROP, &ev);
    }

    return r;
}

/// Keeps g_giveDamageDepth balanced on every return path.
struct DamageScope {
    DamageScope() { ++g_giveDamageDepth; }

    ~DamageScope() { --g_giveDamageDepth; }
};

// _giveDamage PRE hook: PLAYER_DAMAGE for soldiers, VEHICLE_DAMAGE for
// player-controllable objects; both blockable and mutable.
void R1_HOOK Hook_GiveDamage(void* gameServer R1_EDX, uint8_t* target, float damage, int attackerId, int a5, int a6,
                             float x, float y, float z, int a8, int b9, int b10, int b11) {
    DamageScope scope;
    const bool skipEvents = g_skipDamageEvents;  // damage_object: no PRE events for this hit only
    g_skipDamageEvents = false;
    // a6 is the weapon template id: _giveDamage passes it to both
    // Armor::setLastHitDamageType() and handleScore(..., weaponId) for kills.
    Core* core = ReadyCore();
    int victim = -1;
    rmm_player_t attacker = attackerId >= 0 ? attackerId : RMM_PLAYER_NONE;

    if (core && damage > 0.0f && !skipEvents) {
        victim = PlayerIdForSoldier(reinterpret_cast<uintptr_t>(target));

        if (victim >= 0) {
            float amount = damage;
            std::string weapon = WeaponName(a6);
            rmm_ev_player_damage ev{victim, attacker, &amount, weapon.c_str()};

            if (core->Dispatch(RMM_EV_PLAYER_DAMAGE, &ev) >= RMM_HANDLED)
                return;  // blocked

            if (!(amount >= 0.0f))
                amount = 0.0f;  // also catches NaN

            damage = amount;
        } else if (target && core->Events().HasHooks(RMM_EV_VEHICLE_DAMAGE) &&
                   FindPlayerControl(reinterpret_cast<uintptr_t>(target))) {
            // Only built when someone listens: this runs for every hit on every vehicle.
            uintptr_t root = RootOf(reinterpret_cast<uintptr_t>(target));
            std::vector<rmm_player_t> occupants = OccupantsOfRoot(root);
            std::string tmpl = ObjectTemplateName(root), weapon = WeaponName(a6);
            float amount = damage;
            rmm_ev_vehicle_damage ev{Handle(root),
                                     tmpl.c_str(),
                                     attacker,
                                     &amount,
                                     weapon.c_str(),
                                     static_cast<int32_t>(occupants.size()),
                                     occupants.empty() ? nullptr : occupants.data()};

            if (core->Dispatch(RMM_EV_VEHICLE_DAMAGE, &ev) >= RMM_HANDLED)
                return;  // blocked

            if (!(amount >= 0.0f))
                amount = 0.0f;

            damage = amount;
        }
    }

    // Destruction: the engine logs "destroyVehicle" for targets with a
    // player-control object whose armour this hit finishes (hit points -
    // damage <= 0.001). Other armoured objects (bridges, buildings...) are
    // destroyed the same way but not logged. Compare the armour state around
    // the call instead of re-deriving the maths: isDestroyed() or hit points
    // at/below the engine's own threshold.
    uintptr_t t = reinterpret_cast<uintptr_t>(target);
    uintptr_t armor = 0;
    bool isVehicle = false, wasDestroyed = true;

    if (core && victim < 0 && target && damage > 0.0f) {
        // Soldiers are player-control objects with armour too, and some hits on
        // them don't match a player (dead or disconnecting player, soldier
        // thrown out of a destroyed vehicle): never report them as destroyed.
        bool soldier = IsSoldier(t);
        isVehicle = !soldier && FindPlayerControl(t) != 0;

        if (isVehicle || (!soldier && core->Events().HasHooks(RMM_EV_OBJECT_DESTROYED))) {
            armor = FindArmor(t);

            if (armor)
                wasDestroyed = ArmorDestroyed(armor) || ArmorFloat(armor, g_vtArmorHitPoints) <= 0.001f;
        }
    }

    std::string targetName = armor && !wasDestroyed ? ObjectTemplateName(t) : "";
    std::string weaponName = armor && !wasDestroyed ? WeaponName(a6) : "";

    g_giveDamage(gameServer, target, damage, attackerId, a5, a6, x, y, z, a8, b9, b10, b11);

    if (armor && !wasDestroyed && (ArmorDestroyed(armor) || ArmorFloat(armor, g_vtArmorHitPoints) <= 0.001f)) {
        uintptr_t root = RootOf(t);

        if (isVehicle) {
            std::string rootName = root ? ObjectTemplateName(root) : targetName;
            rmm_ev_vehicle ev{attacker,           Handle(t),
                              targetName.c_str(), -1,
                              RMM_FALSE,          Handle(root ? root : t),
                              rootName.c_str(),   RMM_VEHICLE_REASON_GAME};
            core->Dispatch(RMM_EV_VEHICLE_DESTROYED, &ev);
        } else {
            rmm_ev_object_destroyed ev{Handle(t), targetName.c_str(), attacker, weaponName.c_str()};
            core->Dispatch(RMM_EV_OBJECT_DESTROYED, &ev);
        }
    }
}

// Same argument pattern the engine uses for its own direct _giveDamage calls
// (GameServer::gameStatusPlaying): material 0, weapon -1, int -1, then
// bools true. attacker -1 = none (_giveDamage null-checks getPlayerFromId).
// Calls the original (trampoline), so no RMM damage events are raised.
bool ApplyDamage(int victim, float amount, int attacker) {
    uint8_t* p = PlayerById(victim);
    uintptr_t soldier = PlayerAlive(p) ? PlayerSoldier(p) : 0;

    if (!soldier || !g_giveDamage || !g_gameServer)
        return false;

    rmm_vec3 pos{0, 0, 0};
    AbsolutePosition(soldier, pos);
    DamageScope scope;
    g_giveDamage(g_gameServer, reinterpret_cast<uint8_t*>(soldier), amount, attacker >= 0 ? attacker : -1, 0, -1, pos.x,
                 pos.y, pos.z, -1, 1, 1, 1);

    return true;
}

/// Root of a handle if it can take damage_object hits: armoured, not a soldier, not destroyed yet.
uintptr_t DamageableRoot(rmm_object_t handle) {
    uintptr_t obj = Resolve(handle), root = obj ? RootOf(obj) : 0;

    if (!root || IsSoldier(root))
        return 0;

    uintptr_t armor = FindArmor(root);

    if (!armor || ArmorDestroyed(armor) || ArmorFloat(armor, g_vtArmorHitPoints) <= 0.001f)
        return 0;

    return root;
}

// The engine's direct-call argument pattern again (see ApplyDamage),
// aimed at the root, through the hook so VEHICLE_DESTROYED / OBJECT_DESTROYED
// are reported. Whatever a weapon kill does next (explosion, wreck, killing
// the occupants) is the engine's own handling of a finished armour.
// TODO(verify live): explosion effect on clients, occupants killed + credited.
bool ApplyObjectDamage(rmm_object_t handle, float amount, int attacker) {
    uintptr_t root = DamageableRoot(handle);

    if (!root || !g_giveDamage || !g_gameServer)
        return false;

    rmm_vec3 pos{0, 0, 0};
    AbsolutePosition(root, pos);
    g_skipDamageEvents = true;
    Hook_GiveDamage(g_gameServer R1_EDX_ARG, reinterpret_cast<uint8_t*>(root), amount, attacker >= 0 ? attacker : -1, 0,
                    -1, pos.x, pos.y, pos.z, -1, 1, 1, 1);
    g_skipDamageEvents = false;

    return true;
}

void FlushPendingDamage() {
    if (!g_pendingDamage.empty()) {
        auto batch = std::move(g_pendingDamage);
        g_pendingDamage.clear();

        for (auto& d : batch)
            ApplyDamage(d.victim, d.amount, d.attacker);
    }

    if (!g_pendingObjectDamage.empty()) {
        auto batch = std::move(g_pendingObjectDamage);
        g_pendingObjectDamage.clear();

        for (auto& d : batch)
            ApplyObjectDamage(d.object, d.amount, d.attacker);
    }
}

void FlushPendingDeaths(Core& core) {
    g_killedThisTick.clear();

    if (g_pendingDeaths.empty())
        return;

    auto deaths = std::move(g_pendingDeaths);
    g_pendingDeaths.clear();
    R1Adapter* a = R1Adapter::Instance();

    for (auto& d : deaths) {
        // The engine also kills the soldier of a disconnecting player, after
        // the name is cleared; that is a leave, not a death.
        rmm_player_info info{};
        info.struct_size = sizeof info;

        if (!a || !a->GetPlayer(d.first, info) || !info.name[0])
            continue;

        rmm_ev_player_killed ev{d.first, RMM_PLAYER_NONE, d.second.c_str(), "", RMM_FALSE, RMM_FALSE};
        core.Dispatch(RMM_EV_PLAYER_KILLED, &ev);
    }
}

int R1_HOOK Hook_UpdateConsole(void* setup R1_EDX) {
    int r = g_updateConsole(setup);

    if (R1Adapter* a = R1Adapter::Instance())
        a->OnEngineFrame();

    return r;
}

}  // namespace

#ifdef _WIN32
const char* R1Adapter::Plat() { return "windows"; }
#else
const char* R1Adapter::Plat() { return "linux"; }
#endif

// Linux: [linux] key = mangled (or plain C) symbol name, looked up in .symtab.
// Windows: [windows] key = absolute address in the executable (the server is
// not relocatable), valid only for the build named by [windows] pe_timestamp.
uintptr_t R1Adapter::Sym(const std::string& key, bool required, std::string& err) {
    std::string name = data_.Get(Plat(), key);
#ifdef _WIN32
    uintptr_t addr = name.empty() ? 0 : static_cast<uintptr_t>(std::strtoul(name.c_str(), nullptr, 0));
#else
    uintptr_t addr = name.empty() ? 0 : elf::Self().Find(name);
#endif
    if (!addr) {
        // [<plat>] not_used: keys this build deliberately doesn't have (it uses another
        // path, or the default is right), so their absence isn't worth a warning.
        if (name.empty() && !required) {
            const std::string list = "," + data_.Get(Plat(), "not_used") + ",";
            std::string compact;

            for (char ch : list)
                if (ch != ' ' && ch != '\t')
                    compact += ch;

            if (compact.find("," + key + ",") != std::string::npos)
                return 0;
        }

        std::string msg = "symbol '" + key + "' (" + (name.empty() ? "not in gamedata" : name) + ") not found";

        if (required)
            err = msg;
        else
            core_->Log().Warn("%s: %s", shortName_.c_str(), msg.c_str());
    }

    return addr;
}

uint32_t R1Adapter::Off(const std::string& key, uint32_t def) const {
    std::string v = data_.Get(std::string(Plat()) + "_offsets", key);

    return v.empty() ? def : static_cast<uint32_t>(std::strtoul(v.c_str(), nullptr, 0));
}

/// True if [<plat>_offsets] defines every key. Linux: always (the defaults in
/// this file are the verified Linux values).
static bool HasOffsets(const Ini& data, std::initializer_list<const char*> keys) {
#ifdef _WIN32
    for (const char* k : keys)
        if (!data.Has("windows_offsets", k))
            return false;
#else
    (void)data;
    (void)keys;
#endif
    return true;
}

#ifdef _WIN32
/// The addresses in [windows] belong to exactly one build: compare the
/// executable's PE header with the one the gamedata was made from.
static bool CheckWindowsBuild(const Ini& data, std::string& err) {
    auto* base = reinterpret_cast<const uint8_t*>(GetModuleHandleA(nullptr));
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    uint32_t stamp = nt->FileHeader.TimeDateStamp, size = nt->OptionalHeader.SizeOfImage;
    uint32_t wantStamp = static_cast<uint32_t>(std::strtoul(data.Get("windows", "pe_timestamp").c_str(), nullptr, 0));
    uint32_t wantSize = static_cast<uint32_t>(std::strtoul(data.Get("windows", "pe_image_size").c_str(), nullptr, 0));

    if (reinterpret_cast<uintptr_t>(base) != 0x400000 || stamp != wantStamp || (wantSize && size != wantSize)) {
        err = Format("this server executable (PE timestamp 0x%08x, image size 0x%x at %p) is not the build the "
                     "gamedata [windows] addresses are for (0x%08x, %s). RMM supports the BF1942 1.61 "
                     "BF1942_w32ded.exe; the server will run without RMM.",
                     stamp, size, static_cast<const void*>(base), wantStamp, data.Get("windows", "build").c_str());

        return false;
    }

    return true;
}
#endif

bool R1Adapter::AttachNative(std::string& err) {
    if (data_.Get(Plat(), "enabled", "0") != "1") {
        err = std::string(GameName()) + " native support is disabled in gamedata ([" + Plat() + "] enabled=0)";

        return false;
    }
#ifdef _WIN32
    if (!CheckWindowsBuild(data_, err))
        return false;

    core_->Log().Info("%s: Windows server build %s", shortName_.c_str(), data_.Get("windows", "build").c_str());
#else
    if (!elf::Self().LoadSelf(err)) {
        err = "cannot read the server's symbol table: " + err +
              " (RMM needs bf1942_lnxded.dynamic; the .static build cannot be LD_PRELOADed)";

        return false;
    }

    core_->Log().Info("%s: %zu symbols indexed from the server executable", shortName_.c_str(), elf::Self().Size());
#endif
    if (!es::Init(err))
        return false;

    // Calling-convention and layout differences that are plain numbers.
    g_vtConsoleOutput = Off("vt_console_output", g_vtConsoleOutput);
    g_execFlags = Off("console_exec_flags", 0);
    g_thisSay = Off("gs_this_say", 0);
    g_thisGetIp = Off("gs_this_get_player_ip", 0);
    g_thisDestroyPlayer = Off("gs_this_destroy_player", 0);
    kVtQueryComponent = Off("vt_object_query_component", kVtQueryComponent);
    kVtAbsolutePosition = Off("vt_object_absolute_position", kVtAbsolutePosition);
    kVtSetAbsolutePosition = Off("vt_object_set_absolute_position", kVtSetAbsolutePosition);
    kVtGetTransformation = Off("vt_object_get_transformation", kVtGetTransformation);
    kVtPhysGetSpeed = Off("vt_physics_get_speed", kVtPhysGetSpeed);
    kVtPhysGetRotSpeed = Off("vt_physics_get_rot_speed", kVtPhysGetRotSpeed);
    kVtPhysSetSpeed = Off("vt_physics_set_speed", kVtPhysSetSpeed);
    kVtPhysSetRotSpeed = Off("vt_physics_set_rot_speed", kVtPhysSetRotSpeed);
    kVtPhysSetAwake = Off("vt_physics_set_awake", kVtPhysSetAwake);
    kVtBotReset = Off("vt_bot_reset", kVtBotReset);
    g_vtPlayerGetId = Off("vt_player_get_id", g_vtPlayerGetId);
    g_thisItemContainer = Off("soldier_item_container_this", 0);
    g_cwBotHandle = Off("cw_bot_info_handle", g_cwBotHandle);
    g_cwSoldierItem = Off("cw_soldier_item", g_cwSoldierItem);
    g_cwSlot = Off("cw_action_slot", g_cwSlot);
    g_cwItem = Off("cw_action_item", g_cwItem);
    g_templatesOk = HasOffsets(data_, {"vt_template_get_name", "vt_template_get_class_id"});
    g_objectModel =
        g_templatesOk && HasOffsets(data_, {"vt_object_query_interface", "vt_object_query_component",
                                            "vt_object_absolute_position", "object_parent", "vt_pco_id",
                                            "vt_armor_is_destroyed", "vt_armor_hit_points", "vt_armor_max_hit_points"});
    g_scoreFieldsOk = HasOffsets(data_, {"player_score", "player_score_total"});
    g_spawnFieldsOk =
        HasOffsets(data_, {"gameserver_spawn_waves", "gameserver_spawn_delay", "gameserver_spawn_wave_time"});

    if (!g_objectModel)
        core_->Log().Info("%s: object vtable slots not in gamedata yet: positions, health, seats and vehicle details "
                          "are off",
                          shortName_.c_str());

    // bf::game: the GameServer, before any hook could capture it.
    g_gameInstanceGlobal = Sym("game_instance", false, err);

    // ---- required: console + frame ------------------------------------------------
    // Console = the engine's io::OldConsole: mainConsole global (validated by
    // vptr) for executing and printing, OldConsole::executeLine detoured to
    // intercept RMM commands. See docs/re/bf1942-linux.md "Console".
    uintptr_t execLine = Sym("console_execute", true, err);

    if (!execLine)
        return false;

    uintptr_t updateConsole = Sym("server_frame", true, err);

    if (!updateConsole)
        return false;

    mainConsoleGlobal_ = Sym("main_console", true, err);

    if (!mainConsoleGlobal_)
        return false;

    if (uintptr_t vt = Sym("oldconsole_vtable", false, err))
        oldConsoleVptr_ = vt + Off("vtable_address_point", 8);

    g_mainConsoleGlobal = mainConsoleGlobal_;
    g_oldConsoleVptr = oldConsoleVptr_;

    g_oldExecuteLine = reinterpret_cast<OldExecuteLineFn>(execLine);

    if (hooks::Detour(reinterpret_cast<void*>(execLine), reinterpret_cast<void*>(&Hook_OldExecuteLine),
                      reinterpret_cast<void**>(&g_oldExecuteLine))) {
        consoleHooked_ = true;
        eventFlags_[RMM_EV_RCON_COMMAND] = RMM_EVF_SUPPORTED | RMM_EVF_BLOCKABLE;
    } else {
        // Not fatal: RMM can still execute commands, just not intercept typed ones.
        g_oldExecuteLine = reinterpret_cast<OldExecuteLineFn>(execLine);
        core_->Log().Warn("%s: could not detour OldConsole::executeLine (%s); bare RMM console commands unavailable",
                          shortName_.c_str(), hooks::LastDetourError());
    }

    // Engine command output (optional): without it Exec still works, but
    // reading engine variables and the maplist returns nothing.
    if (uintptr_t output = Sym("console_output", false, err)) {
        if (hooks::Detour(reinterpret_cast<void*>(output), reinterpret_cast<void*>(&Hook_ConsoleOutput),
                          reinterpret_cast<void**>(&g_consoleOutput)))
            outputHooked_ = true;
        else
            core_->Log().Warn("%s: could not detour OldConsole::output (%s); engine command output unavailable",
                              shortName_.c_str(), hooks::LastDetourError());
    }

    if (!hooks::Detour(reinterpret_cast<void*>(updateConsole), reinterpret_cast<void*>(&Hook_UpdateConsole),
                       reinterpret_cast<void**>(&g_updateConsole))) {
        err = "could not detour the server_frame function: " + std::string(hooks::LastDetourError());

        return false;
    }

    // ---- optional: chat (blockable + mutable: runs before broadcast) -------------------
    g_chat.kind = Off("chat_kind", g_chat.kind);
    g_chat.text = Off("chat_text", g_chat.text);
    g_chat.textCap = Off("chat_text_capacity", g_chat.textCap);
    g_chat.length = Off("chat_length", g_chat.length);
    g_chat.sender = Off("chat_sender", g_chat.sender);
    g_chat.serverFlag = Off("chat_server_flag", g_chat.serverFlag);

    if (uintptr_t chat = Sym("chat", false, err)) {
        if (hooks::Detour(reinterpret_cast<void*>(chat), reinterpret_cast<void*>(&Hook_HandleChat),
                          reinterpret_cast<void**>(&g_handleChat)))
            eventFlags_[RMM_EV_CHAT] = RMM_EVF_SUPPORTED | RMM_EVF_BLOCKABLE | RMM_EVF_MUTABLE;
        else
            core_->Log().Warn("%s: could not detour handleChatMessage", shortName_.c_str());
    }

    // ---- optional: CD-key authentication -> auth_id ---------------------------------------
    g_getKeyHash = reinterpret_cast<GetKeyHashFn>(Sym("gcd_getkeyhash", false, err));

    if (uintptr_t auth = Sym("cdkey_auth", false, err)) {
        if (!hooks::Detour(reinterpret_cast<void*>(auth), reinterpret_cast<void*>(&Hook_CdKeyAuth),
                           reinterpret_cast<void**>(&g_cdKeyAuth)))
            core_->Log().Warn("%s: could not detour cdKeyAuthorizeCallback; auth ids unavailable", shortName_.c_str());
        else
            authHooked_ = true;
    }

    // Forget a CD-key hash exactly when the engine frees the slot. Without
    // this hook the player poll does it (racy when a slot is reused quickly).
    if (uintptr_t destroy = Sym("destroy_player", false, err)) {
        if (hooks::Detour(reinterpret_cast<void*>(destroy), reinterpret_cast<void*>(&Hook_DestroyPlayer),
                          reinterpret_cast<void**>(&g_destroyPlayer)))
            destroyHooked_ = true;
        else
            core_->Log().Warn("%s: could not detour destroyPlayer", shortName_.c_str());
    }

    authTimeout_ = std::atof(data_.Get("game", "auth_timeout", "20").c_str());

    // ---- optional: gameplay events --------------------------------------------------------
    g_offAlive = Off("player_alive", g_offAlive);
    g_offTemplate = Off("object_template", g_offTemplate);
    g_vtTemplateGetName = Off("vt_template_get_name", g_vtTemplateGetName);
    g_vtTemplateGetClassId = Off("vt_template_get_class_id", g_vtTemplateGetClassId);
    g_templateManagerGlobal = Sym("template_manager", false, err);
    g_vtTemplateManagerGetTemplate = Off("vt_template_manager_get_template", g_vtTemplateManagerGetTemplate);
    // Object model (vehicles, damage): BFV differs from BF1942 here.
    g_vtQueryInterface = Off("vt_object_query_interface", g_vtQueryInterface);
    g_vtArmorIsDestroyed = Off("vt_armor_is_destroyed", g_vtArmorIsDestroyed);
    g_vtArmorMaxHitPoints = Off("vt_armor_max_hit_points", g_vtArmorMaxHitPoints);
    g_vtArmorHitPoints = Off("vt_armor_hit_points", g_vtArmorHitPoints);
    g_vtPcoId = Off("vt_pco_id", g_vtPcoId);
    g_offParent = Off("object_parent", g_offParent);
    g_directParent = data_.Get(Plat(), "object_parent_direct", "0") == "1";

    if (g_directParent) {
        g_tiObject = Sym("object_typeinfo", false, err);
        g_tiPlayerControl = Sym("pco_typeinfo", false, err);

        if (!g_tiObject || !g_tiPlayerControl)
            core_->Log().Warn("%s: object_parent_direct set but the typeinfo symbols are missing; "
                              "vehicles won't be recognised",
                              shortName_.c_str());
    }

    struct EventHook {
        const char* key;
        void* detour;
        void** original;
        std::vector<int32_t> events;
    };

    const EventHook eventHooks[] = {
        {"spawn", reinterpret_cast<void*>(&Hook_Spawn), reinterpret_cast<void**>(&g_spawn), {RMM_EV_PLAYER_SPAWN}},
        {"enter_vehicle",
         reinterpret_cast<void*>(&Hook_EnterVehicle),
         reinterpret_cast<void**>(&g_enterVehicle),
         {RMM_EV_VEHICLE_ENTER}},
        {"exit_vehicle",
         reinterpret_cast<void*>(&Hook_ExitVehicle),
         reinterpret_cast<void**>(&g_exitVehicle),
         {RMM_EV_VEHICLE_EXIT}},
        {"score",
         reinterpret_cast<void*>(&Hook_HandleScore),
         reinterpret_cast<void**>(&g_handleScore),
         {RMM_EV_PLAYER_KILLED, RMM_EV_PLAYER_SCORE}},
        {"cp_set_team",
         reinterpret_cast<void*>(&Hook_CpSetTeam),
         reinterpret_cast<void**>(&g_cpSetTeam),
         {RMM_EV_CONTROL_POINT}},
        // Windows: setTeam is inlined into these two (use instead of cp_set_team)
        {"cp_got_control",
         reinterpret_cast<void*>(&Hook_CpGotControl),
         reinterpret_cast<void**>(&g_cpGotControl),
         {RMM_EV_CONTROL_POINT}},
        {"cp_lost_control",
         reinterpret_cast<void*>(&Hook_CpLostControl),
         reinterpret_cast<void**>(&g_cpLostControl),
         {RMM_EV_CONTROL_POINT}},
        // Kits (handle_pickup: blockable for pickups from the ground, see below)
        {"handle_pickup",
         reinterpret_cast<void*>(&Hook_HandlePickup),
         reinterpret_cast<void**>(&g_handlePickup),
         {RMM_EV_PLAYER_KIT_PICKUP}},
        {"drop_kit",
         reinterpret_cast<void*>(&Hook_DropKit),
         reinterpret_cast<void**>(&g_dropKit),
         {RMM_EV_PLAYER_KIT_DROP}},
        {"change_kit", reinterpret_cast<void*>(&Hook_ChangeKit), reinterpret_cast<void**>(&g_changeKit), {}},
    };

    // Damage needs the player list to identify the victim.
    g_playerManagerGlobal = Sym("player_manager", false, err);
    g_offPlayerList = Off("playermanager_list", g_offPlayerList);
    g_offSoldier = Off("player_soldier", g_offSoldier);
    g_offId = Off("player_id", g_offId);

    if (uintptr_t dmg = Sym("give_damage", false, err)) {
        if (hooks::Detour(reinterpret_cast<void*>(dmg), reinterpret_cast<void*>(&Hook_GiveDamage),
                          reinterpret_cast<void**>(&g_giveDamage))) {
            eventFlags_[RMM_EV_PLAYER_DAMAGE] = RMM_EVF_SUPPORTED | RMM_EVF_BLOCKABLE | RMM_EVF_MUTABLE;
            eventFlags_[RMM_EV_VEHICLE_DAMAGE] = RMM_EVF_SUPPORTED | RMM_EVF_BLOCKABLE | RMM_EVF_MUTABLE;
            eventFlags_[RMM_EV_VEHICLE_DESTROYED] = RMM_EVF_SUPPORTED;
            eventFlags_[RMM_EV_OBJECT_DESTROYED] = RMM_EVF_SUPPORTED;
        } else {
            // players->damage() can still call it directly.
            g_giveDamage = reinterpret_cast<GiveDamageFn>(dmg);
            core_->Log().Warn("%s: could not detour _giveDamage", shortName_.c_str());
        }
    }

    // ---- optional: kill, world objects, physics ---------------------------------------------
    g_killPlayer = reinterpret_cast<KillPlayerFn>(Sym("kill_player", false, err));
    g_getRootParent = reinterpret_cast<GetRootParentFn>(Sym("get_root_parent", false, err));
    g_objectManagerGlobal = Sym("object_manager", false, err);

    if (uintptr_t vt = Sym("objectmanager_vtable", false, err))
        g_objectManagerVptr = vt + Off("vtable_address_point", 8);

    g_offObjectId = Off("object_id", g_offObjectId);
    g_offOmObjects = Off("objectmanager_objects", g_offOmObjects);
    g_offOmPcoMap = Off("objectmanager_pco_map", g_offOmPcoMap);
    g_physicsSetSpeedCount = 0;

    for (const char* key : {"physicsnode_set_speed", "pointphysicsnode_set_speed", "staticphysicsnode_set_speed"})
        if (uintptr_t fn = Sym(key, false, err))
            g_physicsSetSpeed[g_physicsSetSpeedCount++] = fn;

    g_offScore = Off("player_score", g_offScore);
    g_offKills = Off("player_kills", g_offKills);
    g_offDeaths = Off("player_deaths", g_offDeaths);
    g_offNetServer = Off("gameserver_netserver", g_offNetServer);
    g_offCpTeam = Off("cp_team", g_offCpTeam);
    g_offCpTemplate = Off("cp_template", g_offCpTemplate);
    g_offKitMap = Off("gameserver_kit_map", g_offKitMap);
    g_offKitMapTeam[1] = Off("gameserver_kit_map_team1", 0);
    g_offKitMapTeam[2] = Off("gameserver_kit_map_team2", 0);
    g_vtNetServerGetPing = Off("vt_netserver_get_ping", g_vtNetServerGetPing);
    g_offKitIndex = Off("player_kit_index", g_offKitIndex);
    g_offCpRadius = Off("cp_template_radius", g_offCpRadius);
    g_offCpLoseTimer = Off("cp_lose_timer", g_offCpLoseTimer);
    g_offCpTakeTimer = Off("cp_take_timer", g_offCpTakeTimer);
    g_offCpTakingTeam = Off("cp_taking_team", g_offCpTakingTeam);
    g_offCpState = Off("cp_state", g_offCpState);
    g_offCpTmplTakeTime = Off("cp_template_take_time", g_offCpTmplTakeTime);
    g_offCpTmplLoseTime = Off("cp_template_lose_time", g_offCpTmplLoseTime);
    g_offCpTmplFixed = Off("cp_template_fixed", g_offCpTmplFixed);
    g_offCpTmplOnlyTeam = Off("cp_template_only_team", g_offCpTmplOnlyTeam);

    if (uintptr_t cid = Sym("cp_template_cid", false, err))
        elf::SafeReadT(cid, g_cpClassId);

    // The defaults are BF1942's: games whose gamedata doesn't list the layout get no control point API.
    g_cpLayoutKnown = data_.Has(std::string(Plat()) + "_offsets", "cp_state");

    if (uintptr_t frame = Sym("cp_frame_update", false, err)) {
        if (!hooks::Detour(reinterpret_cast<void*>(frame), reinterpret_cast<void*>(&Hook_CpFrameUpdate),
                           reinterpret_cast<void**>(&g_cpFrameUpdate))) {
            g_cpFrameUpdate = nullptr;
            core_->Log().Warn("%s: could not detour ControlPoint::handleFrameUpdate; control point locks off",
                              shortName_.c_str());
        }
    }

    if (uintptr_t cid = Sym("soldier_template_cid", false, err))
        elf::SafeReadT(cid, g_soldierClassId);

    // World objects, spawn/remove, rotation, terrain
    g_offOmGrid = Off("objectmanager_grid", g_offOmGrid);
    g_offOmGridNoCollision = Off("objectmanager_grid_nocollision", g_offOmGridNoCollision);
    g_offNetworkable = Off("object_networkable", g_offNetworkable);
    g_getTemplateByName = reinterpret_cast<GetTemplateByNameFn>(Sym("template_by_name", false, err));
    g_createObject = reinterpret_cast<CreateObjectFn>(Sym("create_object_on_all_clients", false, err));
    g_destroyObject = reinterpret_cast<DestroyObjectFn>(Sym("destroy_object", false, err));
    g_setRotation = reinterpret_cast<SetRotationFn>(Sym("matrix_set_rotation", false, err));
    g_setTransformation = reinterpret_cast<SetTransformationFn>(Sym("set_transformation", false, err));
#ifdef _WIN32
    g_objectSetRotation = reinterpret_cast<ObjectSetRotationFn>(Sym("object_set_rotation", false, err));
#endif
    g_terrainGlobal = Sym("terrain", false, err);

    if (uintptr_t vt = Sym("patchterrain_vtable", false, err))
        g_patchTerrainVptr = vt + Off("vtable_address_point", 8);

    g_vtTerrainGetHeight = Off("vt_terrain_get_height", g_vtTerrainGetHeight);

    if (uintptr_t vt = Sym("netserver_vtable", false, err))
        g_netServerVptr = vt + Off("vtable_address_point", 8);

    // Kits, round end, score, tickets
    g_offSoldierKitId = Off("soldier_kit_id", g_offSoldierKitId);
    g_offTeamScoreTickets = Off("teamscore_tickets", g_offTeamScoreTickets);
    g_offScoreTotal = Off("player_score_total", g_offScoreTotal);
    g_endGameStatus = static_cast<int32_t>(Off("gamestatus_endgame", static_cast<uint32_t>(g_endGameStatus)));

    if (uintptr_t cid = Sym("kit_template_cid", false, err))
        elf::SafeReadT(cid, g_kitClassId);

    g_handlePickupAddr = Sym("handle_pickup", false, err);
    g_handlePickup = reinterpret_cast<HandlePickupFn>(g_handlePickupAddr);  // replaced by the trampoline if hooked
    g_selectBestWeapon = reinterpret_cast<SoldierVoidFn>(Sym("select_best_weapon", false, err));
    g_setGameStatus = reinterpret_cast<SetGameStatusFn>(Sym("set_game_status", false, err));
    g_scoreManagerGlobal = Sym("score_manager", false, err);
    g_scoreSetWinner = reinterpret_cast<ScoreSetFn>(Sym("score_set_winner", false, err));
    g_scoreSetVictoryType = reinterpret_cast<ScoreSetFn>(Sym("score_set_victory_type", false, err));
    g_scoreGetWinner = reinterpret_cast<ScoreGetFn>(Sym("score_get_winner", false, err));
    g_scoreGetVictoryType = reinterpret_cast<ScoreGetFn>(Sym("score_get_victory_type", false, err));
    g_getTeamScore = reinterpret_cast<GetTeamScoreFn>(Sym("score_team_score", false, err));
    g_setCurrentTickets = reinterpret_cast<SetTicketsFn>(Sym("set_current_tickets", false, err));

    for (auto& h : eventHooks) {
        uintptr_t target = Sym(h.key, false, err);

        if (!target)
            continue;

        // Linux: setTeam already reports every change; gotControl/lostControl are
        // only called directly (set_control_point).
        if (g_cpSetTeam && (h.original == reinterpret_cast<void**>(&g_cpGotControl) ||
                            h.original == reinterpret_cast<void**>(&g_cpLostControl))) {
            *h.original = reinterpret_cast<void*>(target);
            continue;
        }

        if (!hooks::Detour(reinterpret_cast<void*>(target), h.detour, h.original)) {
            core_->Log().Warn("%s: could not detour '%s'", shortName_.c_str(), h.key);
            continue;
        }

        for (int32_t ev : h.events)
            eventFlags_[ev] = RMM_EVF_SUPPORTED;  // observe-only
    }

    // These two run their PRE handlers before the engine acts.
    if (eventFlags_.count(RMM_EV_VEHICLE_ENTER))
        eventFlags_[RMM_EV_VEHICLE_ENTER] |= RMM_EVF_BLOCKABLE;

    if (eventFlags_.count(RMM_EV_PLAYER_KIT_PICKUP))
        eventFlags_[RMM_EV_PLAYER_KIT_PICKUP] |= RMM_EVF_BLOCKABLE;

    // Where an enter/exit comes from. The input handler takes the
    // PlayerInput by value, so the hook's copy must be exactly its size.
    g_enterVehicleAddr = Sym("enter_vehicle", false, err);

    if (uint32_t inSize = Off("playerinput_size", g_inputSize); inSize != sizeof(PlayerInputBlob)) {
        core_->Log().Warn("%s: playerinput_size 0x%x != 0x%x; vehicle exit blocking and seat-switch detection off",
                          shortName_.c_str(), inSize, static_cast<unsigned>(sizeof(PlayerInputBlob)));
    } else if (uintptr_t triggers = Sym("check_player_triggers", false, err)) {
        if (hooks::Detour(reinterpret_cast<void*>(triggers), reinterpret_cast<void*>(&Hook_CheckPlayerTriggers),
                          reinterpret_cast<void**>(&g_checkTriggers))) {
            uintptr_t toggle = Sym("toggle_entry_point", false, err);

            if (toggle &&
                hooks::Detour(reinterpret_cast<void*>(toggle), reinterpret_cast<void*>(&Hook_ToggleEntryPoint),
                              reinterpret_cast<void**>(&g_toggleEntry))) {
                g_triggersHooked = true;
            } else {
                // Hook_CheckPlayerTriggers then passes straight through.
                core_->Log().Warn("%s: could not detour toggleEntryPoint; vehicle exit blocking off",
                                  shortName_.c_str());
            }
        } else {
            core_->Log().Warn("%s: could not detour checkPlayerTriggers; vehicle exit blocking off",
                              shortName_.c_str());
        }
    }

    if (g_triggersHooked && eventFlags_.count(RMM_EV_VEHICLE_EXIT))
        eventFlags_[RMM_EV_VEHICLE_EXIT] |= RMM_EVF_BLOCKABLE;

    // Items. Both enable functions must hook, or switches slip past.
    g_offSoldierItemIds = Off("soldier_item_ids", g_offSoldierItemIds);
    g_offSoldierActiveItem = Off("soldier_active_item", g_offSoldierActiveItem);
    g_offSoldierTemplateSlots = Off("soldiertemplate_item_slots", g_offSoldierTemplateSlots);
    g_getItem = reinterpret_cast<GetItemFn>(Sym("soldier_get_item", false, err));
    g_removeItem = reinterpret_cast<RemoveItemFn>(Sym("soldier_remove_item", false, err));
    g_enableItemAddr = Sym("soldier_enable_item", false, err);

    if (uintptr_t net = Sym("soldier_enable_item_net", false, err); net && g_enableItemAddr && g_getItem) {
        bool a = hooks::Detour(reinterpret_cast<void*>(g_enableItemAddr), reinterpret_cast<void*>(&Hook_EnableItem),
                               reinterpret_cast<void**>(&g_enableItem));
        bool b = a && hooks::Detour(reinterpret_cast<void*>(net), reinterpret_cast<void*>(&Hook_EnableItemNet),
                                    reinterpret_cast<void**>(&g_enableItemNet));

        if (b) {
            g_itemsHooked = true;
            eventFlags_[RMM_EV_PLAYER_WEAPON_SWITCH] = RMM_EVF_SUPPORTED | RMM_EVF_BLOCKABLE;

            if (uintptr_t msg = Sym("soldier_handle_message", false, err)) {
                if (!hooks::Detour(reinterpret_cast<void*>(msg), reinterpret_cast<void*>(&Hook_SoldierMessage),
                                   reinterpret_cast<void**>(&g_soldierMessage)))
                    core_->Log().Warn("%s: could not detour BFSoldier::handleMessage; every weapon switch will be "
                                      "reported as the game's (not blockable)",
                                      shortName_.c_str());
            }
        } else {
            // A half-installed pair would block one path and not the other:
            // the hook that did install passes straight through (g_itemsHooked).
            core_->Log().Warn("%s: could not detour BFSoldier::enableItem/enableItemForceNetwork; weapon switch "
                              "events off",
                              shortName_.c_str());
        }
    }

    // Map spawners. Both detours or neither: the GameServer hook only
    // acts inside the spawner's call.
    g_offSpawnerTimer = Off("objectspawner_timer", g_offSpawnerTimer);
    g_offSpawnerTeam = Off("objectspawner_team", g_offSpawnerTeam);
    g_offSpawnWaves = Off("gameserver_spawn_waves", g_offSpawnWaves);
    g_offSpawnDelay = Off("gameserver_spawn_delay", g_offSpawnDelay);
    g_offSpawnWaveTime = Off("gameserver_spawn_wave_time", g_offSpawnWaveTime);
    g_spawnerCalcDelay = reinterpret_cast<SpawnerCalcDelayFn>(Sym("object_spawner_calc_delay", false, err));

    if (uintptr_t sp = Sym("object_spawner_spawn", false, err)) {
        uintptr_t gsSpawn = Sym("gameserver_spawn_object", false, err);

        if (gsSpawn && hooks::Detour(reinterpret_cast<void*>(gsSpawn), reinterpret_cast<void*>(&Hook_ServerSpawnObject),
                                     reinterpret_cast<void**>(&g_serverSpawnObject))) {
            if (hooks::Detour(reinterpret_cast<void*>(sp), reinterpret_cast<void*>(&Hook_SpawnerSpawn),
                              reinterpret_cast<void**>(&g_spawnerSpawn)))
                eventFlags_[RMM_EV_SPAWNER_SPAWN] = RMM_EVF_SUPPORTED | RMM_EVF_BLOCKABLE;
            else
                core_->Log().Warn("%s: could not detour ObjectSpawner::spawnObject; spawner events off",
                                  shortName_.c_str());
        } else {
            core_->Log().Warn("%s: could not detour GameServer::spawnObject; spawner events off", shortName_.c_str());
        }
    }

    // ---- optional: bots (removal, input control) ---------------------------------------------
    g_exitVehicleAddr = Sym("exit_vehicle", false, err);
    g_destroyPlayerAddr = Sym("destroy_player", false, err);
    g_offDefaultCamera = Off("player_default_camera", g_offDefaultCamera);
    g_offPlayerCount = Off("gameserver_player_count", g_offPlayerCount);
    g_botSlotsHeld = 0;
    g_botManagerGlobal = Sym("bot_manager", false, err);
    g_offBotPidTable = Off("botmanager_pid_table", g_offBotPidTable);
    g_inputSlots = Off("playerinput_slots", g_inputSlots);
    g_offInputMask = Off("playerinput_mask", g_offInputMask);
    g_inputSize = Off("playerinput_size", g_inputSize);

    if (g_inputSlots == 0 || g_inputSlots > RMM_BOT_INPUT_SLOTS || g_offInputMask < g_inputSlots * 4 ||
        g_offInputMask + 8 > g_inputSize || g_inputSize > 0x200) {
        core_->Log().Warn("%s: implausible playerinput_* offsets in gamedata; bot input control off",
                          shortName_.c_str());
    } else if (uintptr_t addInput = Sym("ai_add_input", false, err)) {
        if (!hooks::Detour(reinterpret_cast<void*>(addInput), reinterpret_cast<void*>(&Hook_AddInput),
                           reinterpret_cast<void**>(&g_addInput))) {
            g_addInput = nullptr;
            core_->Log().Warn("%s: could not detour AIPlayer::addInput; bot input control unavailable",
                              shortName_.c_str());
        }
    }

    // Bot brain reset after a live kit swap (see g_botFromPlayerId).
    g_botFromPlayerId = reinterpret_cast<BotFromPlayerIdFn>(
        data_.Has(Plat(), "bot_from_player_id") ? Sym("bot_from_player_id", false, err) : 0);
    g_botGetBot = reinterpret_cast<BotGetBotFn>(data_.Has(Plat(), "bot_get_bot") ? Sym("bot_get_bot", false, err) : 0);
    g_botMainReset = Sym("bot_main_reset", false, err);

    if (!HaveBotLookup() || !g_botMainReset)
        core_->Log().Warn("%s: no bot_from_player_id/bot_main_reset in gamedata; set_kit refuses living bots",
                          shortName_.c_str());

    // Bot weapon switch vs. live kit swaps (see Hook_ChangeWeaponExec).
    g_aiInfoManager = Sym("ai_information_manager", false, err);

    if (uintptr_t cw = g_aiInfoManager ? Sym("ai_change_weapon_exec", false, err) : 0) {
        if (!hooks::Detour(reinterpret_cast<void*>(cw), reinterpret_cast<void*>(&Hook_ChangeWeaponExec),
                           reinterpret_cast<void**>(&g_changeWeaponExec))) {
            g_changeWeaponExec = nullptr;
            core_->Log().Warn("%s: could not detour BAPAChangeWeapon::execute; set_kit on a living bot may crash",
                              shortName_.c_str());
        }
    }

    // ---- optional: map / round / game status -------------------------------------------
    // Current level: Setup::getCurrentLevel() = setup + 0x23c, a CampaignEntry
    // {std::string level; GamePlayMode gpm; std::string mod}.
    g_setupGlobal = Sym("setup", false, err);
    g_offSetupLevel = Off("setup_current_level", g_offSetupLevel);
    g_offSetupGpm = Off("setup_current_gpm", 0);
    g_offSetupMod = Off("setup_current_mod", 0);
    SetGpmNames(data_.Get(Plat(), "gpm_names"));
    const EventHook statusHooks[] = {
        {"status_pregame",
         reinterpret_cast<void*>(&Hook_StatusPreGame),
         reinterpret_cast<void**>(&g_statusPreGame),
         {RMM_EV_GAME_STATUS}},
        {"status_first_playing",
         reinterpret_cast<void*>(&Hook_StatusFirstPlaying),
         reinterpret_cast<void**>(&g_statusFirstPlaying),
         {RMM_EV_GAME_STATUS, RMM_EV_ROUND_START}},
        {"status_playing",
         reinterpret_cast<void*>(&Hook_StatusPlaying),
         reinterpret_cast<void**>(&g_statusPlaying),
         {RMM_EV_GAME_STATUS, RMM_EV_ROUND_START}},
        {"status_first_endgame",
         reinterpret_cast<void*>(&Hook_StatusFirstEndGame),
         reinterpret_cast<void**>(&g_statusFirstEndGame),
         {RMM_EV_GAME_STATUS, RMM_EV_ROUND_END}},
    };

    for (auto& h : statusHooks) {
        uintptr_t target = Sym(h.key, false, err);

        if (!target)
            continue;

        if (!hooks::Detour(reinterpret_cast<void*>(target), h.detour, h.original)) {
            core_->Log().Warn("%s: could not detour '%s'", shortName_.c_str(), h.key);
            continue;
        }

        for (int32_t ev : h.events)
            eventFlags_[ev] = RMM_EVF_SUPPORTED;
    }

    if (g_setupGlobal)
        eventFlags_[RMM_EV_MAP_LOAD] = RMM_EVF_SUPPORTED;

    // ---- optional: direct GameServer calls (need the instance, captured by the hooks) --
    g_say = reinterpret_cast<SayFn>(Sym("say", false, err));
    g_sayClient = reinterpret_cast<SayClientFn>(Sym("say_client", false, err));
    g_getIp = reinterpret_cast<GetIpFn>(Sym("get_player_ip", false, err));
    g_ipReversed = data_.Get(Plat(), "player_ip_reversed", "0") == "1";

    // ---- optional: player list ----------------------------------------------------------
    off_.list = Off("playermanager_list", off_.list);
    off_.name = Off("player_name", off_.name);
    off_.vehicle = Off("player_vehicle", off_.vehicle);
    off_.id = Off("player_id", off_.id);
    off_.soldier = Off("player_soldier", off_.soldier);
    off_.isAI = Off("player_is_ai", off_.isAI);
    off_.team = Off("player_team", off_.team);
    off_.vptr = Off("vtable_address_point", off_.vptr);
    g_offId = off_.id;
    g_offVehicle = off_.vehicle;
    g_offSoldier = off_.soldier;
    g_offTeam = off_.team;
    playerManagerGlobal_ = Sym("player_manager", false, err);

    if (uintptr_t vt = Sym("bfplayer_vtable", false, err))
        bfPlayerVptr_ = vt + off_.vptr;

    if (playerManagerGlobal_) {
        // Connect/disconnect/team are derived by diffing the player list each
        // poll, so they are observe-only (not blockable).
        eventFlags_[RMM_EV_PLAYER_CONNECT] = RMM_EVF_SUPPORTED;
        eventFlags_[RMM_EV_PLAYER_AUTH] = RMM_EVF_SUPPORTED;
        eventFlags_[RMM_EV_PLAYER_DISCONNECT] = RMM_EVF_SUPPORTED;
        eventFlags_[RMM_EV_PLAYER_TEAM] = RMM_EVF_SUPPORTED;
    }

    err.clear();
    core_->Log().Info("%s: native %s backend attached; waiting for the console", shortName_.c_str(), Plat());

    return true;
}

int R1Adapter::CapOverride(int32_t cap) const {
    GameServerNow();  // plugins can ask before the first engine frame refreshed it

    switch (cap) {
        case RMM_CAP_CONSOLE_EXEC:
            return MainConsole() && g_oldExecuteLine ? 1 : 0;
        case RMM_CAP_CONSOLE_OUTPUT:
            return MainConsole() && g_oldExecuteLine && outputHooked_ ? 1 : 0;
        case RMM_CAP_CONSOLE_HOOK:
            return consoleHooked_ ? 1 : 0;
        case RMM_CAP_AUTH_ID:
            return authHooked_ && g_getKeyHash ? 1 : 0;
        case RMM_CAP_PRIVATE_MESSAGE:
            return g_sayClient && g_gameServer ? 1 : 0;
        case RMM_CAP_PLAYER_POSITION:
            return playerManagerGlobal_ && g_objectModel ? 1 : 0;
        // Direct GameServer calls need the instance, captured by the first auth/chat/spawn.
        case RMM_CAP_KILL_PLAYER:
            return g_killPlayer && g_gameServer ? 1 : -1;  // else gamedata template
        case RMM_CAP_DAMAGE_PLAYER:
            return g_giveDamage && g_gameServer && playerManagerGlobal_ && g_objectModel ? 1 : 0;
        case RMM_CAP_PLAYER_HEALTH:
            return playerManagerGlobal_ && g_objectModel ? 1 : 0;
        case RMM_CAP_WORLD_OBJECTS:
        case RMM_CAP_OBJECT_MOVE:
            return ObjectManager() ? 1 : 0;
        case RMM_CAP_OBJECT_VELOCITY:
            return ObjectManager() && g_physicsSetSpeedCount ? 1 : 0;
        case RMM_CAP_PLAYER_PUSH:
            return playerManagerGlobal_ && g_physicsSetSpeedCount && g_objectModel ? 1 : 0;
        // set_rotation only: info.rotation is not decoded from the matrix yet (TODO(RE))
        case RMM_CAP_OBJECT_ROTATION:
            return ObjectManager() && CanRotate() ? 1 : 0;
        case RMM_CAP_TELEPORT:
            return playerManagerGlobal_ && ObjectManager() && g_objectModel ? 1 : -1;
        case RMM_CAP_OBJECT_QUERY:
            return ObjectManager() && g_getTemplateByName && g_templateManagerGlobal ? 1 : 0;
        case RMM_CAP_OBJECT_SPAWN:
            return g_createObject && g_getTemplateByName && g_templateManagerGlobal && g_gameServer && ObjectManager()
                       ? 1
                       : 0;
        case RMM_CAP_OBJECT_REMOVE:
            return g_destroyObject && g_gameServer && ObjectManager() ? 1 : 0;
        case RMM_CAP_GROUND_HEIGHT:
            return g_terrainGlobal && g_patchTerrainVptr ? 1 : 0;
        case RMM_CAP_BOT_INPUT:
            return g_addInput && playerManagerGlobal_ ? 1 : 0;
        case RMM_CAP_CONTROL_POINTS:
            return g_cpLayoutKnown && ObjectManager() ? 1 : 0;
        case RMM_CAP_CONTROL_POINT_SET:
            return g_cpLayoutKnown && ObjectManager() && g_cpGotControl && g_cpLostControl ? 1 : 0;
        case RMM_CAP_CONTROL_POINT_LOCK:
            return g_cpLayoutKnown && ObjectManager() && g_cpFrameUpdate ? 1 : 0;
        // -1 below: fall back to whatever the gamedata offers
        case RMM_CAP_SET_KIT:
            return g_createObject && g_handlePickupAddr && g_destroyObject && g_getTemplateByName &&
                           g_templateManagerGlobal && g_gameServer && ObjectManager() && playerManagerGlobal_
                       ? 1
                       : -1;
        case RMM_CAP_END_ROUND:
            return g_setGameStatus && g_scoreSetWinner && g_scoreSetVictoryType && g_gameServer && ScoreManager() ? 1
                                                                                                                  : -1;
        case RMM_CAP_SET_SCORE:
            return playerManagerGlobal_ && g_scoreFieldsOk ? 1 : -1;
        case RMM_CAP_TICKETS:
            return g_getTeamScore && g_setCurrentTickets && g_gameServer && ScoreManager() ? 1 : -1;
        case RMM_CAP_PUT_IN_VEHICLE:
            return g_enterVehicleAddr && g_exitVehicleAddr && g_gameServer && ObjectManager() && playerManagerGlobal_
                       ? 1
                       : 0;
        case RMM_CAP_OBJECT_DAMAGE:
            return g_giveDamage && g_gameServer && ObjectManager() ? 1 : 0;
        case RMM_CAP_ITEMS:
            return g_getItem && g_enableItemAddr && playerManagerGlobal_ ? 1 : 0;
        case RMM_CAP_SPAWN_TIMING:
            return (g_gameInstanceGlobal || g_gameServer) && g_spawnFieldsOk ? 1 : 0;
        case RMM_CAP_BOT_REMOVE:  // native teardown, else whatever the gamedata offers
            if (data_.Has("capabilities", "bot_remove"))
                return -1;  // explicit switch wins

            return NativeBotRemoveReady() ? 1 : -1;
        default:
            return -1;
    }
}

std::vector<IGameAdapter::BotRef> R1Adapter::Bots() {
    if (!g_botManagerGlobal || !playerManagerGlobal_)
        return AdapterBase::Bots();

    std::vector<BotRef> v;

    if (!BotsAvailable())
        return v;  // the co-op probe (ai.getBotCount) stays: no AI, no BotManager data

    uintptr_t bm = 0, begin = 0, end = 0;

    if (!elf::SafeReadT(g_botManagerGlobal, bm) || !bm || !elf::SafeReadT(bm + g_offBotPidTable, begin) ||
        !elf::SafeReadT(bm + g_offBotPidTable + 4, end) || end < begin || end - begin > 4096 * 4)
        return v;

    const size_t n = (end - begin) / 4;

    for (rmm_player_t id : PlayerIds()) {
        rmm_player_info p{};
        p.struct_size = sizeof p;
        int32_t botId = -1;

        if (id < 0 || static_cast<size_t>(id) >= n || !GetPlayer(id, p) || !p.is_bot ||
            !elf::SafeReadT(begin + static_cast<uintptr_t>(id) * 4, botId) || botId < 0)
            continue;

        v.push_back(BotRef{id, botId, p.name});
    }

    std::sort(v.begin(), v.end(), [](const BotRef& a, const BotRef& b) { return a.botId < b.botId; });

    return v;
}

bool R1Adapter::NativeBotRemoveReady() const {
    // g_botManagerGlobal: after a removal the bot list must not come from ai.getAllBots (see Bots()).
    return g_exitVehicleAddr && g_destroyPlayerAddr && g_destroyObject && g_gameServer && playerManagerGlobal_ &&
           g_botManagerGlobal && g_killPlayer && HasTemplate("bot_deactivate") && HasTemplate("bot_count") &&
           HasTemplate("bot_list");
}

// Remove a bot the way the engine tears one down (~BotMain ->
// AIPlayer::removePlayerFromGame), after detaching its AI with ai.deactivateBot
// (BotManager pool + SAI bookkeeping; the brain is reused by the next addBot).
// See docs/re/bf1942-linux.md "AI bots".
bool R1Adapter::BotRemove(const BotRef& bot) {
    if (!NativeBotRemoveReady())
        return AdapterBase::BotRemove(bot);

    uint8_t* player = PlayerById(bot.player);
    uint8_t isAI = 0;

    if (!player || !elf::SafeRead(player + off_.isAI, &isAI, 1) || !isAI)
        return false;  // never a human

    if (!PlayerAlive(player))
        return RemoveBotNow(bot);

    // BotManager::deactivateBotFromPlayerId only detaches *dead* bots (its
    // dead list +0x30, or a bot whose isDead() is set), like co-op dropping a
    // bot between lives for a joining human. So: kill it, finish next frames.
    for (auto& r : pendingBotRemovals_)
        if (r.bot.player == bot.player)
            return true;

    if (!KillPlayer(bot.player))
        return false;

    double wait = std::atof(data_.Get("bots", "remove_wait", "5").c_str());
    pendingBotRemovals_.push_back(PendingBotRemoval{bot, core_->Time() + wait});

    return true;
}

bool R1Adapter::RemoveBotNow(const BotRef& bot, bool quiet) {
    uint8_t* player = PlayerById(bot.player);

    if (!player)
        return false;

    std::string out;

    if (!RunTemplate(
            "bot_deactivate",
            {{"player", std::to_string(bot.player)}, {"bot", std::to_string(bot.botId)}, {"botname", bot.name}}, &out))
        return false;

    std::string o = Trim(out);

    if (StartsWith(o, "Error") || IContains(o, "Unknown object") || o == "0" || IEquals(o, "false")) {
        if (!quiet)
            core_->Log().Warn("%s: ai.deactivateBot refused bot %s (#%d): %s", shortName_.c_str(), bot.name.c_str(),
                              bot.player, o.c_str());

        return false;
    }

    player = PlayerById(bot.player);  // re-resolve after the console call

    if (!player)
        return true;  // already gone

    reinterpret_cast<ExitVehicleFn>(g_exitVehicleAddr)(g_gameServer, player, 0);
    uintptr_t camera = 0;

    if (elf::SafeReadT(reinterpret_cast<uintptr_t>(player) + g_offDefaultCamera, camera) && camera)
        g_destroyObject(g_gameServer, camera);

    reinterpret_cast<DestroyPlayerFn>(g_destroyPlayerAddr)(GsThis(g_thisDestroyPlayer), player);
    core_->BotInput().Forget(bot.player);

    // Keep the slot empty until the round ends ([bots] keep_removed = 0: let co-op refill it).
    if (data_.Get("bots", "keep_removed", "1") != "0")
        ++g_botSlotsHeld;

    core_->Log().Info("%s: removed bot %s (#%d, bot %d)%s", shortName_.c_str(), bot.name.c_str(), bot.player, bot.botId,
                      g_botSlotsHeld ? Format("; %d slot(s) kept empty this round", g_botSlotsHeld).c_str() : "");

    return true;
}

bool R1Adapter::KillPlayer(rmm_player_t id) {
    if (g_killPlayer && g_gameServer)
        return LowByte(g_killPlayer(g_gameServer, id));

    return AdapterBase::KillPlayer(id);
}

bool R1Adapter::DamagePlayer(rmm_player_t id, float amount, rmm_player_t attacker) {
    if (!g_giveDamage || !g_gameServer)
        return false;

    if (g_giveDamageDepth > 0) {
        // Inside the engine's damage code: apply at the end of this frame.
        uint8_t* p = PlayerById(id);

        if (!PlayerAlive(p))
            return false;

        g_pendingDamage.push_back(PendingDamage{id, amount, attacker});

        return true;
    }

    return ApplyDamage(id, amount, attacker);
}

bool R1Adapter::DamageObject(rmm_object_t obj, float amount, rmm_player_t attacker) {
    if (!g_giveDamage || !g_gameServer || !DamageableRoot(obj))
        return false;

    if (g_giveDamageDepth > 0) {  // inside the engine's damage/kill code: end of frame
        g_pendingObjectDamage.push_back(PendingObjectDamage{obj, amount, attacker});

        return true;
    }

    return ApplyObjectDamage(obj, amount, attacker);
}

bool R1Adapter::PlayerHealth(rmm_player_t id, float& health, float& maxHealth) {
    uint8_t* p = PlayerById(id);
    uintptr_t armor = PlayerAlive(p) ? FindArmor(PlayerSoldier(p)) : 0;

    if (!armor)
        return false;

    health = ArmorFloat(armor, g_vtArmorHitPoints);
    maxHealth = ArmorFloat(armor, g_vtArmorMaxHitPoints);

    return true;
}

std::vector<IGameAdapter::VehicleRef> R1Adapter::Vehicles() {
    std::vector<VehicleRef> v;
    uintptr_t om = ObjectManager();

    for (uintptr_t o : MapValues(om, g_offOmPcoMap)) {
        uintptr_t c = Composite(o);

        if (IsSoldier(c))
            continue;  // soldiers are player-control objects too

        v.push_back(VehicleRef{Handle(c), ObjectTemplateName(c), RootOf(c) == c});
    }

    return v;
}

bool R1Adapter::ObjectValid(rmm_object_t obj) { return Resolve(obj) != 0; }

bool R1Adapter::GetObjectInfo(rmm_object_t handle, rmm_object_info& out) {
    uintptr_t obj = Resolve(handle);

    if (!obj)
        return false;

    uintptr_t root = RootOf(obj);
    out.object = handle;
    out.root = Handle(root);
    CopyOut(ObjectTemplateName(obj), out.template_name, sizeof out.template_name);
    CopyOut(ObjectTemplateName(root), out.root_template, sizeof out.root_template);
    AbsolutePosition(obj, out.position);

    if (uintptr_t node = PhysicsNodeOf(root)) {
        ReadVec(node, kVtPhysGetSpeed, out.velocity);
        ReadVec(node, kVtPhysGetRotSpeed, out.angular_velocity);
    }

    if (uintptr_t armor = FindArmor(root)) {
        out.health = ArmorFloat(armor, g_vtArmorHitPoints);
        out.max_health = ArmorFloat(armor, g_vtArmorMaxHitPoints);
        out.is_destroyed = ArmorDestroyed(armor) ? RMM_TRUE : RMM_FALSE;
    }

    out.occupant_count = static_cast<int32_t>(OccupantsOfRoot(root).size());
    out.is_vehicle = FindPlayerControl(obj) ? RMM_TRUE : RMM_FALSE;
    out.is_root = root == obj ? RMM_TRUE : RMM_FALSE;
    uintptr_t net = 0;
    out.is_networked =
        (elf::SafeReadT(obj + g_offNetworkable, net) && net) || (elf::SafeReadT(root + g_offNetworkable, net) && net)
            ? RMM_TRUE
            : RMM_FALSE;

    // A seat = an object in the PCO map (not a soldier).
    if (out.is_vehicle && !IsSoldier(obj) && MapFind(ObjectManager(), g_offOmPcoMap, ObjectId(obj))) {
        out.seat = SeatOf(obj);
        int who = SeatOccupant(obj);
        out.seat_occupant = who >= 0 ? who : RMM_PLAYER_NONE;
    }

    return true;
}

std::vector<rmm_player_t> R1Adapter::ObjectOccupants(rmm_object_t handle) {
    uintptr_t obj = Resolve(handle);

    return obj ? OccupantsOfRoot(RootOf(obj)) : std::vector<rmm_player_t>{};
}

// BCompositeObject::setAbsolutePosition (vtable +0x3c) on the root; this is
// also what PhysicsNode::setAbsolutePosition forwards to. VERIFY live that
// clients follow the jump without a desync.
bool R1Adapter::SetObjectPosition(rmm_object_t handle, const rmm_vec3& pos) {
    uintptr_t obj = Resolve(handle), root = obj ? RootOf(obj) : 0;
    using Fn = void(R1_THIS*)(uintptr_t self, const float* pos3);
    auto fn = root ? VirtualAt<Fn>(root, kVtSetAbsolutePosition) : nullptr;

    if (!fn)
        return false;

    const float p[3] = {pos.x, pos.y, pos.z};
    fn(root, p);

    return true;
}

// IPhysicsNode::setPositionalSpeed / setRotationalSpeed on the root's physics
// component; setIsAwake so a sleeping body picks up a non-zero velocity.
bool R1Adapter::SetObjectVelocity(rmm_object_t handle, const rmm_vec3& linear, const rmm_vec3& angular) {
    uintptr_t obj = Resolve(handle), root = obj ? RootOf(obj) : 0;
    uintptr_t node = root ? PhysicsNodeOf(root) : 0;

    if (!node)
        return false;

    using SetFn = void(R1_THIS*)(uintptr_t self, const float* vec3);
    using WakeFn = void(R1_THIS*)(uintptr_t self);
    auto setSpeed = VirtualAt<SetFn>(node, kVtPhysSetSpeed);
    auto setRot = VirtualAt<SetFn>(node, kVtPhysSetRotSpeed);

    if (!setSpeed || !setRot)
        return false;

    const float lin[3] = {linear.x, linear.y, linear.z}, ang[3] = {angular.x, angular.y, angular.z};
    setSpeed(node, lin);
    setRot(node, ang);
    bool moving = lin[0] != 0 || lin[1] != 0 || lin[2] != 0 || ang[0] != 0 || ang[1] != 0 || ang[2] != 0;

    if (moving)
        if (auto wake = VirtualAt<WakeFn>(node, kVtPhysSetAwake))
            wake(node);

    return true;
}

// The BFSoldier constructor gives every soldier its own PointPhysicsNode
// (setComponent(IID_IPhysicsNode)), and BFSoldier::handleUpdate only reads its
// speed (falling/explosion animations), so a speed added here is integrated
// by the engine like an explosion's knock-back.
// In a vehicle the soldier isn't what physics moves, so the speed is added to
// the vehicle's root instead (its PhysicsNode, the same component
// SetObjectVelocity drives): the whole vehicle, with everyone in it, goes up.
bool R1Adapter::PushPlayer(rmm_player_t id, const rmm_vec3& velocity) {
    uint8_t* p = PlayerById(id);

    if (!PlayerAlive(p))
        return false;

    uintptr_t body = 0;

    if (uintptr_t vehicle = PlayerVehicle(p)) {
        body = RootOf(vehicle);
    } else {
        body = Composite(PlayerSoldier(p));

        if (body && RootOf(body) != body)
            return false;
    }

    uintptr_t node = body ? PhysicsNodeOf(body) : 0;

    if (!node)
        return false;

    using SetFn = void(R1_THIS*)(uintptr_t self, const float* vec3);
    using WakeFn = void(R1_THIS*)(uintptr_t self);
    auto setSpeed = VirtualAt<SetFn>(node, kVtPhysSetSpeed);

    if (!setSpeed)
        return false;

    rmm_vec3 cur{0, 0, 0};
    ReadVec(node, kVtPhysGetSpeed, cur);
    const float v[3] = {cur.x + velocity.x, cur.y + velocity.y, cur.z + velocity.z};
    setSpeed(node, v);

    if (auto wake = VirtualAt<WakeFn>(node, kVtPhysSetAwake))
        wake(node);

    return true;
}

// The body that moves when a player is relocated: the root of their vehicle,
// or their soldier on foot. 0 for a dead player.
static uintptr_t PlayerBody(uint8_t* p) {
    if (!PlayerAlive(p))
        return 0;

    if (uintptr_t vehicle = PlayerVehicle(p))
        return RootOf(vehicle);

    uintptr_t soldier = Composite(PlayerSoldier(p));

    return soldier && RootOf(soldier) == soldier ? soldier : 0;
}

// Same setter as SetObjectPosition, then any speed is cleared so a falling
// or driving player doesn't carry it to the new place. VERIFY live that the
// player's own client follows (on-foot movement is client-predicted).
bool R1Adapter::Teleport(rmm_player_t id, const rmm_vec3& pos) {
    if (!g_objectModel)
        return AdapterBase::Teleport(id, pos);

    uintptr_t body = PlayerBody(PlayerById(id));
    using Fn = void(R1_THIS*)(uintptr_t self, const float* pos3);
    auto setPos = body ? VirtualAt<Fn>(body, kVtSetAbsolutePosition) : nullptr;

    if (!setPos)
        return AdapterBase::Teleport(id, pos);

    const float p[3] = {pos.x, pos.y, pos.z};
    setPos(body, p);

    if (uintptr_t node = PhysicsNodeOf(body)) {
        using SetFn = void(R1_THIS*)(uintptr_t self, const float* vec3);
        const float zero[3] = {0, 0, 0};

        if (auto setSpeed = VirtualAt<SetFn>(node, kVtPhysSetSpeed))
            setSpeed(node, zero);
    }

    return true;
}

// Exactly what createObjectOnAllClients does for a new object: copy the
// current absolute matrix (keeps the position row), setRotation(ypr in
// degrees), setTransformation. VERIFY live that clients follow.
bool R1Adapter::SetObjectRotation(rmm_object_t handle, const rmm_vec3& ypr) {
    uintptr_t obj = Resolve(handle), root = obj ? RootOf(obj) : 0;
#ifdef _WIN32
    if (root && g_objectSetRotation) {
        const float v[3] = {ypr.x, ypr.y, ypr.z};
        g_objectSetRotation(root, v);

        return true;
    }
#endif
    if (!root || !g_setRotation || !g_setTransformation)
        return false;

    using GetMatFn = const float*(R1_THIS*)(uintptr_t self);
    auto getMat = VirtualAt<GetMatFn>(root, kVtGetTransformation);
    const float* cur = getMat ? getMat(root) : nullptr;
    float m[16];

    if (!cur || !elf::SafeRead(cur, m, sizeof m))
        return false;

    const float v[3] = {ypr.x, ypr.y, ypr.z};
    g_setRotation(m, v);
    g_setTransformation(root, m);

    return true;
}

std::vector<IGameAdapter::ObjectRef> R1Adapter::WorldObjects() {
    std::vector<ObjectRef> out;
    uintptr_t om = ObjectManager();

    if (!om)
        return out;

    // Roots that are, or carry, a player-control object: vehicles (and soldiers).
    std::set<uintptr_t> pcoRoots;

    for (uintptr_t o : MapValues(om, g_offOmPcoMap))
        pcoRoots.insert(RootOf(o));

    std::set<uintptr_t> seen;

    for (uint32_t off : {g_offOmGrid, g_offOmGridNoCollision}) {
        for (uintptr_t o : MapValues(om, off)) {
            uintptr_t c = Composite(o);

            if (!c || !seen.insert(c).second)
                continue;

            ObjectRef r{Handle(c), ObjectTemplateName(c), rmm_vec3{0, 0, 0}, false, IsSoldier(c)};

            if (!r.handle)
                continue;

            AbsolutePosition(c, r.position);
            r.isVehicle = !r.isSoldier && pcoRoots.count(c) != 0;
            out.push_back(std::move(r));
        }
    }

    return out;
}

/// The engine's template of that name, or 0.
static uintptr_t TemplateByName(const std::string& name) {
    uintptr_t mgr = 0;

    if (name.empty() || !g_getTemplateByName || !g_templateManagerGlobal ||
        !elf::SafeReadT(g_templateManagerGlobal, mgr) || !mgr)
        return 0;

    es::Scoped s(name.c_str());

    return g_getTemplateByName(mgr, s.get());
}

bool R1Adapter::TemplateExists(const std::string& tmpl) { return TemplateByName(tmpl) != 0; }

// ---- kits, round end, score, tickets --------------------------------------------

/// The player's soldier if they are alive and on foot (changeKit only acts
/// when getVehicle, BFPlayer+0x4c, is of class CID_BFSoldierTemplate), else 0.
static uintptr_t OnFootSoldier(uint8_t* p) {
    uintptr_t controlled = 0;

    if (!PlayerAlive(p) || !elf::SafeReadT(reinterpret_cast<uintptr_t>(p) + g_offVehicle, controlled) || !controlled ||
        controlled != PlayerSoldier(p) || !IsSoldier(controlled))
        return 0;

    return controlled;
}

/// A kit template the engine knows, or 0.
static uintptr_t KitTemplate(const std::string& name) {
    uintptr_t tmpl = TemplateByName(name);

    return tmpl && TemplateClassId(tmpl) == g_kitClassId ? tmpl : 0;
}

bool R1Adapter::SetKit(rmm_player_t id, const std::string& kitTemplate) {
    uint8_t* p = PlayerById(id);

    if (!p || !g_createObject || !g_handlePickupAddr || !g_destroyObject || !g_gameServer)
        return AdapterBase::SetKit(id, kitTemplate);

    if (!OnFootSoldier(p))
        return false;

    if (!KitTemplate(kitTemplate)) {
        core_->Log().Debug("set_kit: '%s' is not a kit template", kitTemplate.c_str());

        return false;
    }

    uint8_t isAI = 0;
    elf::SafeRead(p + off_.isAI, &isAI, 1);

    if (isAI && (!HaveBotLookup() || !g_botMainReset || !g_botManagerGlobal)) {
        core_->Log().Debug("set_kit: #%d is a bot and its brain can't be reset (gamedata); refused", id);

        return false;
    }

    // Inside the engine's damage/kill code, or a bot (whose brain reset must not
    // run inside AIMain::action): at RMM's next frame.
    if (g_giveDamageDepth > 0 || isAI) {
        for (auto& k : g_pendingKits)
            if (k.player == id) {
                k.kit = kitTemplate;

                return true;
            }  // latest wins

        g_pendingKits.push_back(PendingKit{id, kitTemplate});

        return true;
    }

    return ApplyKit(id, kitTemplate);
}

// See g_botFromPlayerId. The BotMain of a bot player whose vtable +0x2c is
// BotMain::reset, or 0.
static uintptr_t BotBrain(rmm_player_t id) {
    uintptr_t bm = 0, begin = 0, end = 0, vt = 0, fn = 0;
    int32_t botId = -1;

    if (id < 0 || !HaveBotLookup() || !g_botMainReset || !g_botManagerGlobal ||
        !elf::SafeReadT(g_botManagerGlobal, bm) || !bm || !elf::SafeReadT(bm + g_offBotPidTable, begin) ||
        !elf::SafeReadT(bm + g_offBotPidTable + 4, end) || end < begin ||
        static_cast<uintptr_t>(id) >= (end - begin) / 4 ||  // getBotFromPlayerId doesn't range-check
        !elf::SafeReadT(begin + static_cast<uintptr_t>(id) * 4, botId) || botId < 0)
        return 0;

    uintptr_t bot = g_botFromPlayerId ? g_botFromPlayerId(bm, id) : g_botGetBot(bm, botId);

    if (!bot || !elf::SafeReadT(bot, vt) || !vt || !elf::SafeReadT(vt + kVtBotReset, fn) || fn != g_botMainReset)
        return 0;

    return bot;
}

void R1Adapter::FlushPendingKits() {
    if (!g_pendingKits.empty()) {
        auto batch = std::move(g_pendingKits);
        g_pendingKits.clear();

        for (auto& k : batch)
            if (!ApplyKit(k.player, k.kit))
                core_->Log().Debug("set_kit: deferred %s for #%d not applied (dead or in a vehicle by now)",
                                   k.kit.c_str(), k.player);
    }

    if (!g_pendingVehicleOps.empty()) {
        auto batch = std::move(g_pendingVehicleOps);
        g_pendingVehicleOps.clear();

        for (auto& op : batch)
            if (!(op.enter ? ApplyPutIn(op.player, op.vehicle, op.seat) : ApplyTakeOut(op.player)))
                core_->Log().Debug("%s: deferred call for #%d not applied", op.enter ? "enter_vehicle" : "exit_vehicle",
                                   op.player);
    }
}

// ---- items ------------------------------------------------------------------------

bool R1Adapter::GetItems(rmm_player_t id, std::vector<rmm_item_info>& out) {
    uintptr_t soldier = OnFootSoldier(PlayerById(id));

    if (!soldier || !g_getItem)
        return false;

    int active = ActiveItemSlot(soldier), n = ItemSlotCount(soldier);

    for (int slot = 1; slot < n; ++slot) {
        uintptr_t item = ItemAt(soldier, slot);

        if (!item)
            continue;

        rmm_item_info info{};
        info.slot = slot;
        CopyOut(ObjectTemplateName(item), info.weapon, sizeof info.weapon);
        info.active = slot == active;
        out.push_back(info);
    }

    return true;
}

// Through the hooked enableItem, so WEAPON_SWITCH is raised (reason PLUGIN).
// The held slot is replicated to everyone else; the player's own client
// (VERIFY live) may or may not follow - see docs/re "Weapon switching".
bool R1Adapter::SelectItem(rmm_player_t id, int32_t slot) {
    uintptr_t soldier = OnFootSoldier(PlayerById(id));

    if (!soldier || !g_enableItemAddr || !ItemAt(soldier, slot))
        return false;

    if (ActiveItemSlot(soldier) == slot)
        return true;

    {
        ItemContext ic(RMM_WEAPON_REASON_PLUGIN);
        reinterpret_cast<EnableItemFn>(g_enableItemAddr)(soldier, slot);
    }

    return ActiveItemSlot(soldier) == slot;
}

// ---- spawn timing ----------------------------------------------------------------

bool R1Adapter::GetSpawnTiming(rmm_spawn_timing& out) {
    uintptr_t gs = g_spawnFieldsOk ? GameServerNow() : 0;
    uint8_t waves = 0;

    if (!gs || !elf::SafeReadT(gs + g_offSpawnWaves, waves) || !elf::SafeReadT(gs + g_offSpawnDelay, out.delay) ||
        !elf::SafeReadT(gs + g_offSpawnWaveTime, out.wave_interval))
        return false;

    out.waves = waves ? RMM_TRUE : RMM_FALSE;

    return true;
}

bool R1Adapter::SetSpawnTiming(const rmm_spawn_timing& t) {
    uintptr_t gs = g_spawnFieldsOk ? GameServerNow() : 0;

    if (!gs)
        return false;

    rmm_spawn_timing check{sizeof check, 0, 0, 0};

    if (!GetSpawnTiming(check))
        return false;  // readable, so the fields are where we think

    *reinterpret_cast<uint8_t*>(gs + g_offSpawnWaves) = t.waves ? 1 : 0;
    *reinterpret_cast<float*>(gs + g_offSpawnDelay) = t.delay;
    *reinterpret_cast<float*>(gs + g_offSpawnWaveTime) = t.wave_interval;

    return true;
}

// ---- control points -----------------------------------------------------------------

/// Is @p o a ControlPoint whose fields are where the gamedata says? The grid
/// holds IObject*, which is the ControlPoint itself (handleFrameUpdate calls
/// IObject slots on `this`); its own ControlPointTemplate* must then be the
/// object's template. Anything else is refused rather than written to.
static bool IsControlPoint(uintptr_t o) {
    uintptr_t objTmpl = 0, cpTmpl = 0;

    return o && ObjectClassId(o) == g_cpClassId && elf::SafeReadT(o + g_offTemplate, objTmpl) &&
           elf::SafeReadT(o + g_offCpTemplate, cpTmpl) && objTmpl && objTmpl == cpTmpl;
}

/// Control points of the running map (ControlPoint*, the grid's IObject*), oldest first.
static std::vector<uintptr_t> ControlPointObjects() {
    std::vector<uintptr_t> out;
    uintptr_t om = ObjectManager();

    if (!om)
        return out;

    for (uint32_t off : {g_offOmGrid, g_offOmGridNoCollision})
        for (uintptr_t o : MapValues(om, off))
            if (std::find(out.begin(), out.end(), o) == out.end() && IsControlPoint(o))
                out.push_back(o);

    std::sort(out.begin(), out.end(), [](uintptr_t a, uintptr_t b) { return ObjectId(a) < ObjectId(b); });

    return out;
}

/// The ControlPoint behind a handle, or 0.
static uintptr_t ResolveControlPoint(rmm_object_t h) {
    uintptr_t live = Resolve(h);

    if (!live)
        return 0;

    if (Composite(live) == live && IsControlPoint(live))
        return live;

    for (uintptr_t cp : ControlPointObjects())
        if (Composite(cp) == live)
            return cp;

    return 0;
}

static uintptr_t CpTemplate(uintptr_t cp) {
    uintptr_t tmpl = 0;

    return cp && elf::SafeReadT(cp + g_offCpTemplate, tmpl) ? tmpl : 0;
}

/// Settle a point the way the engine does when nobody is at it: no team
/// taking it, both timers full, state controlled (or lost when neutral).
static void CpSettle(uintptr_t cp, uintptr_t tmpl) {
    auto* p = reinterpret_cast<uint8_t*>(cp);
    *reinterpret_cast<int32_t*>(p + g_offCpTakingTeam) = 0;
    *reinterpret_cast<float*>(p + g_offCpTakeTimer) = *reinterpret_cast<float*>(tmpl + g_offCpTmplTakeTime);
    *reinterpret_cast<float*>(p + g_offCpLoseTimer) = *reinterpret_cast<float*>(tmpl + g_offCpTmplLoseTime);
    *reinterpret_cast<int32_t*>(p + g_offCpState) = ReadInt(p, g_offCpTeam) != 0 ? 4 : 1;
}

std::vector<rmm_object_t> R1Adapter::ControlPoints() {
    std::vector<rmm_object_t> out;

    for (uintptr_t cp : ControlPointObjects())
        if (rmm_object_t h = Handle(cp))
            out.push_back(h);

    return out;
}

bool R1Adapter::GetControlPoint(rmm_object_t h, rmm_control_point_info& out) {
    uintptr_t cp = ResolveControlPoint(h), tmpl = CpTemplate(cp);

    if (!cp || !tmpl)
        return false;

    auto* p = reinterpret_cast<const uint8_t*>(cp);
    CopyOut(TemplateName(tmpl), out.name, sizeof out.name);
    AbsolutePosition(cp, out.position);
    elf::SafeReadT(tmpl + g_offCpRadius, out.radius);
    out.team = ReadInt(p, g_offCpTeam);
    int32_t state = ReadInt(p, g_offCpState);
    out.state = state == 2 ? RMM_CP_STATE_LOSING : state == 3 ? RMM_CP_STATE_TAKING : RMM_CP_STATE_HELD;
    out.capturing_team = state == 3 ? ReadInt(p, g_offCpTakingTeam) : 0;
    out.locked = CpLocked(cp) ? RMM_TRUE : RMM_FALSE;
    uint8_t fixed = 0;
    elf::SafeReadT(tmpl + g_offCpTmplFixed, fixed);
    out.fixed = fixed == 1 ? RMM_TRUE : RMM_FALSE;
    elf::SafeReadT(tmpl + g_offCpTmplOnlyTeam, out.only_team);

    return true;
}

// The engine's own transitions: lostControl (team 0, spawns and spawners off),
// then gotControl(team) (team, spawns and spawners on for it). Both refuse a
// team the template excludes and setTeam ignores fixed points, so the
// template's two fields are cleared around the calls.
bool R1Adapter::SetControlPoint(rmm_object_t h, int32_t team) {
    uintptr_t cp = ResolveControlPoint(h), tmpl = CpTemplate(cp);

    if (!cp || !tmpl || !g_cpGotControl || !g_cpLostControl || team < 0 || team > 2)
        return false;

    auto* p = reinterpret_cast<uint8_t*>(cp);
    int32_t before = ReadInt(p, g_offCpTeam);

    if (before == team)
        return true;

    auto* fixed = reinterpret_cast<uint8_t*>(tmpl + g_offCpTmplFixed);
    auto* onlyTeam = reinterpret_cast<int32_t*>(tmpl + g_offCpTmplOnlyTeam);
    uint8_t savedFixed = *fixed;
    int32_t savedOnly = *onlyTeam;
    *fixed = 0;
    *onlyTeam = 0;
    g_cpQuiet = true;

    if (before != 0)
        g_cpLostControl(p, 0);

    if (team != 0)
        g_cpGotControl(p, static_cast<unsigned>(team));

    g_cpQuiet = false;
    *fixed = savedFixed;
    *onlyTeam = savedOnly;
    CpSettle(cp, tmpl);
    int32_t after = ReadInt(p, g_offCpTeam);

    if (after != before && core_) {
        std::string name = TemplateName(tmpl);
        rmm_ev_control_point ev{Handle(cp), name.c_str(), before, after, 0, nullptr};
        core_->Dispatch(RMM_EV_CONTROL_POINT, &ev);
    }

    return after == team;
}

bool R1Adapter::LockControlPoint(rmm_object_t h, bool locked) {
    uintptr_t cp = ResolveControlPoint(h), tmpl = CpTemplate(cp);

    if (!cp || !tmpl || !g_cpFrameUpdate)
        return false;

    bool was = CpLocked(cp);

    if (locked && !was) {
        if (g_cpLockCount >= kMaxCpLocks)
            return false;

        g_cpLocks[g_cpLockCount++] = cp;
        CpSettle(cp, tmpl);  // cancel a capture in progress
    } else if (!locked && was) {
        for (int i = 0; i < g_cpLockCount; ++i)
            if (g_cpLocks[i] == cp)
                g_cpLocks[i--] = g_cpLocks[--g_cpLockCount];
    }

    return true;
}

/// A new level: its control points are new objects.
static void ClearControlPointLocks() { g_cpLockCount = 0; }

// Disabled: it crashes the server. The approach: switch away if held, BFSoldier::removeItem
// (detaches it, clears the slot), then GameServer::destroyObject, which tells every client.
// The owner's client still holds the item afterwards, and the server later fires the
// detached FireArms ("Fatal error: No physicsNodeComp!" from FireArms::fireBarrel) and exits.
// Blocking the switch works (the server corrects the owner's client), so nothing needs
// this; the code is kept for a future, safer removal path.
static constexpr bool kRemoveItemEnabled = false;

bool R1Adapter::RemoveItem(rmm_player_t id, int32_t slot) {
    if (!kRemoveItemEnabled) {
        core_->Log().Warn("remove_item is disabled on BF1942 (it crashed the server in a live test)");

        return false;
    }

    uintptr_t soldier = OnFootSoldier(PlayerById(id));

    if (!soldier || !g_removeItem || !g_destroyObject || !g_gameServer || !ItemAt(soldier, slot))
        return false;

    if (g_giveDamageDepth > 0)
        return false;  // never from inside damage code

    if (ActiveItemSlot(soldier) == slot && g_selectBestWeapon) {
        // Pick another item first (selectBestLoadedWeapon would choose this one again
        // if it is the best): the first other non-empty slot from the main weapon down.
        for (int s : {3, 2, 1, 4, 5, 6, 7, 8})
            if (s != slot && ItemAt(soldier, s)) {
                ItemContext ic(RMM_WEAPON_REASON_PLUGIN);
                reinterpret_cast<EnableItemFn>(g_enableItemAddr)(soldier, s);
                break;
            }
    }

    uintptr_t item = g_removeItem(soldier + g_thisItemContainer, slot);

    if (!item)
        return false;

    g_destroyObject(g_gameServer, item);
    core_->Log().Info("removed item %d from #%d (experimental)", slot, id);

    return true;
}

// ---- putting players in and out of vehicles ------------------------------------

bool R1Adapter::PutInVehicle(rmm_player_t id, rmm_object_t vehicle, int32_t seat) {
    if (!g_enterVehicleAddr || !g_exitVehicleAddr || !g_gameServer)
        return false;

    uint8_t* p = PlayerById(id);
    uintptr_t obj = Resolve(vehicle);

    if (!PlayerAlive(p) || !obj || !FindPlayerControl(obj))
        return false;

    if (g_giveDamageDepth > 0) {
        g_pendingVehicleOps.push_back(PendingVehicleOp{id, vehicle, seat, true});

        return true;
    }

    return ApplyPutIn(id, vehicle, seat);
}

// What checkPlayerTriggers does for F1-F8, from anywhere: exitVehicle(player,
// false) if they sit somewhere, then enterVehicle(player, seat composite,
// false, false). Through RMM's hooks, so the events are raised (reason PLUGIN).
bool R1Adapter::ApplyPutIn(rmm_player_t id, rmm_object_t vehicle, int32_t seat) {
    uint8_t* p = PlayerById(id);
    uintptr_t obj = Resolve(vehicle);

    if (!PlayerAlive(p) || !obj)
        return false;

    uintptr_t target = 0;

    for (auto& s : SeatsOf(RootOf(obj))) {
        if (seat >= 0 ? s.first == seat : SeatOccupant(s.second) < 0) {
            target = s.second;
            break;
        }
    }

    if (!target) {
        core_->Log().Debug("enter_vehicle: #%d: no seat %d (%zu seats)", id, seat, SeatsOf(RootOf(obj)).size());

        return false;
    }

    int taken = SeatOccupant(target);

    if (taken == id)
        return true;  // already there

    if (taken >= 0)
        return false;

    VehicleContext vc(RMM_VEHICLE_REASON_PLUGIN);

    if (PlayerVehicle(p)) {
        reinterpret_cast<ExitVehicleFn>(g_exitVehicleAddr)(g_gameServer, p, 0);
        p = PlayerById(id);

        if (!p || PlayerVehicle(p))
            return false;
    }

    reinterpret_cast<EnterVehicleFn>(g_enterVehicleAddr)(g_gameServer, p, reinterpret_cast<void*>(target), 0, 0);
    p = PlayerById(id);
    uintptr_t now = p ? PlayerVehicle(p) : 0;

    if (!(now && Composite(now) == target))
        core_->Log().Debug("enter_vehicle: #%d: engine left the player in %p (seat object %p)", id,
                           reinterpret_cast<void*>(now), reinterpret_cast<void*>(target));

    return now && Composite(now) == target;
}

bool R1Adapter::TakeOutOfVehicle(rmm_player_t id) {
    if (!g_exitVehicleAddr || !g_gameServer)
        return false;

    uint8_t* p = PlayerById(id);

    if (!PlayerAlive(p) || !PlayerVehicle(p))
        return false;

    if (g_giveDamageDepth > 0) {
        g_pendingVehicleOps.push_back(PendingVehicleOp{id, 0, -1, false});

        return true;
    }

    return ApplyTakeOut(id);
}

bool R1Adapter::ApplyTakeOut(rmm_player_t id) {
    uint8_t* p = PlayerById(id);

    if (!PlayerAlive(p) || !PlayerVehicle(p))
        return false;

    VehicleContext vc(RMM_VEHICLE_REASON_PLUGIN);
    reinterpret_cast<ExitVehicleFn>(g_exitVehicleAddr)(g_gameServer, p, 0);
    p = PlayerById(id);

    return p && !PlayerVehicle(p);
}

// GameServer::changeKit's recipe with any kit template instead of a spawn-menu
// slot: createObjectOnAllClients(kit, 0,0,0, 0,0,0, 1, 0), handlePickup(kit,
// player, soldier, true) - which tells every client and makes the soldier drop
// the old kit and take the new one - then destroyObject(old kit) and
// selectBestLoadedWeapon. The handlePickup call goes through RMM's hook, so
// KIT_DROP/KIT_PICKUP are raised (reason PLUGIN, not blockable). Bots also get
// their brain reset (see g_botFromPlayerId).
bool R1Adapter::ApplyKit(rmm_player_t id, const std::string& kitTemplate) {
    uint8_t* p = PlayerById(id);
    uintptr_t controlled = OnFootSoldier(p);
    uintptr_t tmpl = KitTemplate(kitTemplate);

    if (!controlled || !tmpl)
        return false;

    uint8_t isAI = 0;
    elf::SafeRead(p + off_.isAI, &isAI, 1);

    if (isAI && !BotBrain(id)) {  // a kit swap without the brain reset would crash the AI later
        core_->Log().Warn("set_kit: can't find the AI of bot #%d; kit not changed", id);

        return false;
    }

    uint32_t oldId = 0xffffffffu, nowId = 0xffffffffu;
    elf::SafeReadT(controlled + g_offSoldierKitId, oldId);
    uintptr_t oldKit = KitObjectById(oldId);
    uintptr_t kit = g_createObject(g_gameServer, tmpl, 0, 0, 0, 0, 0, 0, 1, 0);

    if (!kit)
        return false;

    uintptr_t item = Composite(kit);
    {
        KitContext kc(RMM_KIT_REASON_PLUGIN);
        reinterpret_cast<HandlePickupFn>(g_handlePickupAddr)(g_gameServer, item, p, controlled, 1);
    }

    p = PlayerById(id);  // event handlers ran in between

    if (!p)
        return false;

    // BFSoldier::pickupKit stores the new kit's id (kit+0x48) in soldier+0x408.
    if (!elf::SafeReadT(controlled + g_offSoldierKitId, nowId) || nowId == oldId) {
        core_->Log().Warn("set_kit: the soldier of #%d did not take %s; removing it", id, kitTemplate.c_str());
        g_destroyObject(g_gameServer, item);

        return false;
    }

    if (nowId != ObjectId(item))
        core_->Log().Debug("set_kit: soldier kit id %u, new kit object id %u (VERIFY)", nowId, ObjectId(item));

    // Keep the spawn-menu slot (BFPlayer+0x80, replicated: the scoreboard's kit
    // icon) in step when the kit is one of the team's own.
    int slot = KitSlot(ReadInt(p, g_offTeam), kitTemplate);

    if (slot >= 0)
        *reinterpret_cast<int32_t*>(p + g_offKitIndex) = slot;

    if (oldKit && oldKit != item)
        g_destroyObject(g_gameServer, oldKit);  // dropped by pickupKit: remove, as changeKit does

    if (g_selectBestWeapon)
        g_selectBestWeapon(controlled);

    if (isAI) {
        if (uintptr_t brain = BotBrain(id))
            reinterpret_cast<void(R1_THIS*)(uintptr_t)>(g_botMainReset)(brain);
        else
            core_->Log().Warn("set_kit: bot #%d lost its AI during the kit swap; not reset", id);
    }

    core_->Log().Info("gave %s to #%d", kitTemplate.c_str(), id);

    return true;
}

// BFPlayer+0xc0 is the score the scoreboard shows: BFPlayer::updateStateMask
// diffs it every update (bit 0x800), so a write reaches every client.
// ScoreManager::scoreEvent always moves +0x114 by the same amount.
bool R1Adapter::SetScore(rmm_player_t id, int32_t score) {
    uint8_t* p = g_scoreFieldsOk ? PlayerById(id) : nullptr;

    if (!p)
        return AdapterBase::SetScore(id, score);

    int32_t old = ReadInt(p, g_offScore);
    *reinterpret_cast<int32_t*>(p + g_offScore) = score;

    if (g_offScoreTotal)
        *reinterpret_cast<int32_t*>(p + g_offScoreTotal) += score - old;

    return true;
}

// Current tickets: ScoreManager::getTeamScore(team)->tickets (TeamScore+0x48),
// what gameStatusPlaying compares; GameServer::setCurrentNumberOfTickets sets
// them (Game::getNumberOfTickets is the round's *starting* count).
int32_t R1Adapter::GetTickets(int32_t team) {
    uintptr_t sm = ScoreManager();

    if (!sm || !g_getTeamScore)
        return AdapterBase::GetTickets(team);

    if (team != 1 && team != 2)
        return -1;

    uintptr_t ts = g_getTeamScore(sm, static_cast<unsigned>(team));
    int32_t t = -1;

    return ts && elf::SafeReadT(ts + g_offTeamScoreTickets, t) ? t : -1;
}

bool R1Adapter::SetTickets(int32_t team, int32_t tickets) {
    if (!g_setCurrentTickets || !g_gameServer)
        return AdapterBase::SetTickets(team, tickets);

    if (team != 1 && team != 2)
        return false;

    g_setCurrentTickets(g_gameServer, static_cast<unsigned>(team), static_cast<unsigned>(std::max(tickets, 0)));

    return true;
}

// Applied from the frame hook, where console commands (admin.*) also run,
// rather than from whatever engine callback the plugin was in.
bool R1Adapter::EndRoundNow(int32_t winner) {
    if (!g_setGameStatus || !g_scoreSetWinner || !g_scoreSetVictoryType || !g_gameServer || !ScoreManager())
        return AdapterBase::EndRoundNow(winner);

    if (status_ != RMM_STATUS_PLAYING || pendingEndRound_ >= 0)
        return false;

    pendingEndRound_ = winner;

    return true;
}

// Exactly what GameServer::handleScore does when a team reaches the score
// limit: setWinner(team), setVictoryType(1), setGameStatus(EndGame). A draw
// is winner 0 with victory type 0, as gameStatusPlaying reports one.
void R1Adapter::ProcessPendingEndRound() {
    if (pendingEndRound_ < 0)
        return;

    int32_t winner = pendingEndRound_;
    pendingEndRound_ = -1;
    uintptr_t sm = ScoreManager();

    if (!sm || !g_gameServer || status_ != RMM_STATUS_PLAYING)
        return;

    g_scoreSetWinner(sm, winner);
    g_scoreSetVictoryType(sm, winner ? 1 : 0);
    g_setGameStatus(g_gameServer, g_endGameStatus);
    core_->Log().Info("%s: round ended by RMM (%s)", shortName_.c_str(),
                      winner ? Format("team %d wins", winner).c_str() : "draw");
}

rmm_object_t R1Adapter::SpawnObject(const std::string& tmplName, const rmm_vec3& pos, const rmm_vec3& ypr) {
    if (!g_createObject || !g_gameServer || !ObjectManager())
        return 0;

    uintptr_t tmpl = TemplateByName(tmplName);

    if (!tmpl) {
        core_->Log().Debug("spawn: unknown template '%s'", tmplName.c_str());

        return 0;
    }

    // readyClientsOnly = 1, ghostId = 0: as GameServer::spawnObject.
    uintptr_t obj = g_createObject(g_gameServer, tmpl, pos.x, pos.y, pos.z, ypr.x, ypr.y, ypr.z, 1, 0);

    if (!obj)
        return 0;

    uintptr_t net = 0;
    elf::SafeReadT(obj + g_offNetworkable, net);
    core_->Log().Info("spawned %s at %.1f,%.1f,%.1f%s", tmplName.c_str(), pos.x, pos.y, pos.z,
                      net ? "" : " (not networked: server-only)");

    return Handle(obj);
}

bool R1Adapter::RemoveObject(rmm_object_t handle) {
    uintptr_t obj = Resolve(handle), root = obj ? RootOf(obj) : 0;

    if (!root || !g_destroyObject || !g_gameServer)
        return false;

    if (IsSoldier(root))
        return false;  // a player's soldier: use kill

    return LowByte(g_destroyObject(g_gameServer, root));
}

bool R1Adapter::GroundHeight(float x, float z, float& height) {
    uintptr_t terrain = 0, vptr = 0;

    if (!g_terrainGlobal || !elf::SafeReadT(g_terrainGlobal, terrain) || !terrain)
        return false;

    // Only the real terrain: NullTerrain (no map loaded) reports nonsense.
    if (!g_patchTerrainVptr || !elf::SafeReadT(terrain, vptr) || vptr != g_patchTerrainVptr)
        return false;

    using Fn = float(R1_THIS*)(uintptr_t self, float x, float z);  // float in st(0), as GCC 3 returns it
    auto getHeight = VirtualAt<Fn>(terrain, g_vtTerrainGetHeight);

    if (!getHeight)
        return false;

    float h = getHeight(terrain, x, z);

    if (!(h == h))
        return false;  // NaN

    height = h;

    return true;
}

bool R1Adapter::SayAll(const std::string& msg) {
    // GameServer::say(-1, msg, teamOnly=false, true) is exactly what
    // GameServer::serverMessage() does. Max 63 chars per message: split.
    if (!g_say || !GameServerNow())
        return AdapterBase::SayAll(msg);

    for (size_t pos = 0; pos < msg.size() || pos == 0; pos += 63) {
        es::Scoped s(msg.substr(pos, 63).c_str());
        g_say(GsThis(g_thisSay), -1, s.get(), 0, 1);

        if (msg.empty())
            break;
    }

    return true;
}

bool R1Adapter::PrivateMessage(rmm_player_t id, const std::string& msg) {
    if (!g_sayClient || !GameServerNow())
        return AdapterBase::PrivateMessage(id, msg);

    for (size_t pos = 0; pos < msg.size() || pos == 0; pos += 63) {
#ifdef _WIN32
        // The by-value argument: built with the engine's own constructor, then
        // owned (and destroyed) by sayClient.
        es::String copy{};
        es::Construct(&copy, msg.substr(pos, 63).c_str());
        g_sayClient(g_gameServer, id, copy, 0, 0);
#else
        es::Scoped copy(msg.substr(pos, 63).c_str());  // the by-value argument
        g_sayClient(g_gameServer, id, copy.get(), 0, 0);
#endif
        if (msg.empty())
            break;
    }

    return true;
}

std::string R1Adapter::PlayerAddress(rmm_player_t id) {
    if (!g_getIp || !GameServerNow())
        return {};
#ifdef _WIN32
    es::String ret{};
    g_getIp(GsThis(g_thisGetIp), &ret, id);
#else
    es::String ret = g_getIp(g_gameServer, id);
#endif
    std::string s = es::Read(&ret);
    es::Destroy(&ret);

    if (s == "bot" || s == "localhost")
        return {};

    return g_ipReversed ? UnreverseIp(s) : s;
}

void R1Adapter::NativeFrame() {
    GameServerNow();  // follow bf::game

    if (core_->Time() >= nextPoll_) {
        nextPoll_ = core_->Time() + 0.5;
        CheckMap();
        PollPlayers();
    }

    FlushPendingDamage();
    FlushPendingDeaths(*core_);
    FlushPendingKits();
    ProcessPendingBotRemovals();
    ProcessPendingEndRound();
}

// Living bots are killed first (only dead bots can be detached); finish the
// removal once the engine has them dead. Runs every frame while any wait.
void R1Adapter::ProcessPendingBotRemovals() {
    if (pendingBotRemovals_.empty())
        return;

    double now = core_->Time();

    for (size_t i = 0; i < pendingBotRemovals_.size();) {
        PendingBotRemoval& r = pendingBotRemovals_[i];
        uint8_t* p = PlayerById(r.bot.player);
        bool expired = now > r.deadline;

        // The AI's own isDead() may trail the engine's alive flag by a frame: retry until the deadline.
        if (p && !expired && (PlayerAlive(p) || !RemoveBotNow(r.bot, /*quiet=*/true))) {
            ++i;
            continue;
        }

        BotRef bot = r.bot;
        pendingBotRemovals_.erase(pendingBotRemovals_.begin() + static_cast<std::ptrdiff_t>(i));

        if (p && expired && PlayerById(bot.player))
            core_->Log().Warn("%s: bot %s (#%d) could not be detached from the AI in time; not removed",
                              shortName_.c_str(), bot.name.c_str(), bot.player);
    }
}

// Fires MAP_LOAD when Setup's current level differs from the last one seen
// (also the first time, so plugins learn the startup map). The level is set
// before the map loads; BF1942 has no map sizes, so size is 0.
void R1Adapter::CheckMap() {
    Core* core = ReadyCore();
    uintptr_t setup = 0;

    if (!core || !g_setupGlobal || !elf::SafeReadT(g_setupGlobal, setup) || !setup)
        return;

    const auto* level = reinterpret_cast<const es::String*>(setup + g_offSetupLevel);
    const auto* mod =
        reinterpret_cast<const es::String*>(setup + (g_offSetupMod ? g_offSetupMod : g_offSetupLevel + 8));
    int32_t gpm = -1;

    if (!elf::SafeReadT(setup + (g_offSetupGpm ? g_offSetupGpm : g_offSetupLevel + 4), gpm))
        return;

    std::string map = es::Read(level), modName = es::Read(mod);

    if (map.empty() || (map == currentMap_.map && gpm == lastGpm_ && modName == lastMod_))
        return;

    std::string mode = GpmName(gpm);  // "" while the level is still being set up
    lastGpm_ = gpm;
    lastMod_ = modName;

    if (!modName.empty())
        mod_ = modName;  // the running mod (bf1942, xpack1, DesertCombat...)

    NoteMap(map, mode, 0);
    core->OnMapLoad(map, mode, mod_, 0);
}

// Status transitions come from the game-status hooks; pre-game is reported by
// a per-frame function, so only changes are dispatched.
void R1Adapter::OnGameStatus(int32_t status) {
    Core* core = ReadyCore();

    if (!core || status == status_)
        return;

    CheckMap();  // a new level enters pre-game: MAP_LOAD first
    rmm_ev_game_status ev{status_, status};
    status_ = status;

    if (status == RMM_STATUS_PREGAME) {  // new round: the engine's normal bot count again
        g_botSlotsHeld = 0;
        pendingBotRemovals_.clear();
        ClearControlPointLocks();  // a new level's points may reuse old addresses
    }

    core->Dispatch(RMM_EV_GAME_STATUS, &ev);

    if (status == RMM_STATUS_PLAYING) {
        rmm_ev_map map{currentMap_.map, currentMap_.mode, mod_.c_str(), 0};
        core->Dispatch(RMM_EV_ROUND_START, &map);
    } else if (status == RMM_STATUS_ENDGAME) {
        // Whoever ended the round (handleScore's score limit, gameStatusPlaying's
        // tickets/time limit, or end_round) called ScoreManager::setWinner first.
        // Reason = the victory type (setVictoryType: 0 draw, 1..3; VERIFY live).
        rmm_ev_round_end end{RMM_TEAM_NONE, 0};

        if (uintptr_t sm = ScoreManager()) {
            int32_t w = g_scoreGetWinner ? static_cast<int32_t>(g_scoreGetWinner(sm) & 0xff) : 0;
            end.winner = (w == 1 || w == 2) ? w : RMM_TEAM_NONE;

            if (g_scoreGetVictoryType)
                end.reason = static_cast<int32_t>(g_scoreGetVictoryType(sm) & 0xff);
        }

        core->Dispatch(RMM_EV_ROUND_END, &end);
    }
}

// The engine prints to the server console (and its remote-console mirrors)
// with mainConsole->output(line) - OldConsole vtable +0x10, as
// handleChatMessage does. Validated against the OldConsole vtable first.
bool R1Adapter::NativeConsolePrint(const std::string& line) {
    if (!mainConsoleGlobal_ || !oldConsoleVptr_ || !es::Ready())
        return false;

    uintptr_t console = 0, vptr = 0;

    if (!elf::SafeReadT(mainConsoleGlobal_, console) || !console)
        return false;

    if (!elf::SafeReadT(console, vptr) || vptr != oldConsoleVptr_)
        return false;

    using OutputFn = void(R1_THIS*)(uintptr_t self, es::String* line);
    auto output = VirtualAt<OutputFn>(console, g_vtConsoleOutput);

    if (!output)
        return false;

    es::Scoped s((line + "\n").c_str());
    ++g_printBypass;  // RMM's own log lines are never swallowed by an Exec capture
    output(console, s.get());
    --g_printBypass;

    return true;
}

// mainConsole->executeLine(cmd, out, false x5): exactly how the engine runs
// lines itself (GameServer::handleGameEventManagerEvent,
// ObjectTemplateAdm::updateTemplate). Calls the original, never RMM's hook.
bool R1Adapter::NativeExec(const std::string& command, std::string* output) {
    void* console = MainConsole();

    if (!console || !g_oldExecuteLine)
        return false;

    // The trailing newline is required: executeLine reads lines with
    // istream::getline and drops one that ends at EOF (`testb $2` = eofbit
    // right after getline, 0x83f0c4b), so without it the command is silently
    // ignored.
    std::string line = command;

    if (line.empty() || line.back() != '\n')
        line += '\n';

    es::Scoped cmd(line.c_str());
    es::Scoped out("");
    // The engine prints results through OldConsole::output from inside
    // executeLine (`out` stays empty); capture them only if the caller wants
    // them, otherwise they reach the console as usual. Nested Execs (an event
    // handler running a command mid-exec) get their own buffer.
    std::string captured;
    std::string* prevCapture = g_captureOut;
    g_captureOut = output && outputHooked_ ? &captured : nullptr;
    ++g_rmmExecDepth;
    // Flags as the engine's own callers pass them (all false: no echo, no
    // print). The result comes back in `out`. A typed line uses (1,1,1,0,0): b3 echoes "> line", b4 prints the result.
    // Windows: the last flag selects full access (level 7 instead of 1); the
    // engine refuses getters such as game.serverMaxPlayers at level 1, even
    // typed at the console. Gamedata console_exec_flags (bit 0 = b3 .. bit 4 = b7).
    const uint32_t fl = g_execFlags;
    g_oldExecuteLine(console, cmd.get(), out.get(), fl & 1, (fl >> 1) & 1, (fl >> 2) & 1, (fl >> 3) & 1, (fl >> 4) & 1);
    --g_rmmExecDepth;
    g_captureOut = prevCapture;

    if (output) {
        std::string result = out.str();
        *output += result.empty() ? captured : result;  // anything printed instead (fallback)
    }

    return true;
}

// ---- players ------------------------------------------------------------------

std::vector<uintptr_t> R1Adapter::WalkPlayerList() {
    std::vector<uintptr_t> players;

    if (!playerManagerGlobal_ || listBroken_)
        return players;

    // playerManager is a global IPlayerManager*; PlayerManager::getPlayers()
    // returns the std::list at +0x0c. GCC 3 std::list holds a pointer to a
    // heap sentinel node; nodes are { next, prev, value }.
    uintptr_t pm = 0, sentinel = 0, node = 0;

    if (!elf::SafeReadT(playerManagerGlobal_, pm) || !pm)
        return players;

    if (!elf::SafeReadT(pm + off_.list + r1abi::kListHeader, sentinel) || !sentinel)
        return players;

    if (!elf::SafeReadT(sentinel, node))
        return players;

    for (int guard = 0; node && node != sentinel; ++guard) {
        uintptr_t player = 0;

        if (guard > 512 || !elf::SafeReadT(node + 2 * sizeof(uintptr_t), player) || !elf::SafeReadT(node, node)) {
            core_->Log().Error("%s: player list walk failed - disabling player support (check [%s_offsets])",
                               shortName_.c_str(), Plat());
            listBroken_ = true;

            return {};
        }

        if (player)
            players.push_back(player);
    }

    return players;
}

bool R1Adapter::ReadPlayerAt(uintptr_t p, rmm_player_info& out) {
    uintptr_t vptr = 0;

    if (!elf::SafeReadT(p, vptr))
        return false;

    if (bfPlayerVptr_ && vptr != bfPlayerVptr_)
        return false;  // not a BFPlayer: unknown layout

    int32_t id = -1, team = 0;
    es::String name{};
    uintptr_t vehicle = 0, soldier = 0;
    uint8_t ai = 0;

    if (!elf::SafeReadT(p + off_.id, id) || !elf::SafeReadT(p + off_.team, team) ||
        !elf::SafeReadT(p + off_.name, name) || !elf::SafeReadT(p + off_.vehicle, vehicle))
        return false;

    elf::SafeReadT(p + off_.soldier, soldier);
    elf::SafeReadT(p + off_.isAI, ai);

    uint32_t size = out.struct_size;
    out = rmm_player_info{};
    out.struct_size = size;
    out.id = id;
    CopyOut(es::Read(&name), out.name, sizeof out.name);
    out.team = team;
    out.is_bot = ai != 0;
    auto auth = g_authIds.find(id);

    if (auth != g_authIds.end())
        CopyOut(auth->second, out.auth_id, sizeof out.auth_id);

    if (!out.is_bot)
        CopyOut(PlayerAddress(id), out.address, sizeof out.address);

    const uint8_t* pb = reinterpret_cast<const uint8_t*>(p);
    out.score = ReadInt(pb, g_offScore);
    out.kills = ReadInt(pb, g_offKills);
    out.deaths = ReadInt(pb, g_offDeaths);
    out.ping = out.is_bot ? 0 : PlayerPing(id);
    CopyOut(PlayerKitName(pb), out.kit, sizeof out.kit);
    // BF1942 has no squads.
    out.is_alive = PlayerAlive(reinterpret_cast<const uint8_t*>(p));  // +0x79, set by spawnPlayer

    // Position of whatever the player controls (vehicle or soldier) while alive.
    if (out.is_alive) {
        uintptr_t controlled = vehicle ? vehicle : soldier;

        if (controlled)
            AbsolutePosition(controlled, out.position);
    }

    out.seat = -1;

    // A dead player "controls" their death camera (MultiPlayerFreeCamera):
    // only a living player is in a vehicle.
    if (out.is_alive && vehicle && vehicle != soldier) {
        out.vehicle = Handle(vehicle);
        CopyOut(ObjectTemplateName(vehicle), out.vehicle_template, sizeof out.vehicle_template);

        if (out.is_alive)
            out.seat = SeatOf(vehicle);
    }

    return true;
}

std::vector<rmm_player_t> R1Adapter::PlayerIds() {
    std::vector<rmm_player_t> ids;

    for (uintptr_t p : WalkPlayerList()) {
        int32_t id = -1;

        if (elf::SafeReadT(p + off_.id, id) && id >= 0)
            ids.push_back(id);
    }

    return ids;
}

bool R1Adapter::GetPlayer(rmm_player_t id, rmm_player_info& out) {
    for (uintptr_t p : WalkPlayerList()) {
        int32_t pid = -1;

        if (elf::SafeReadT(p + off_.id, pid) && pid == id)
            return ReadPlayerAt(p, out);
    }

    // A player being reported as disconnected is already gone from the engine.
    if (id == leaving_) {
        auto it = known_.find(id);

        if (it != known_.end()) {
            uint32_t size = out.struct_size;
            out = it->second;
            out.struct_size = size;

            return true;
        }
    }

    return false;
}

void R1Adapter::PollPlayers() {
    if (!playerManagerGlobal_ || listBroken_)
        return;

    std::map<rmm_player_t, rmm_player_info> now;

    for (uintptr_t p : WalkPlayerList()) {
        rmm_player_info info{};
        info.struct_size = sizeof info;

        if (ReadPlayerAt(p, info) && info.id >= 0)
            now[info.id] = info;
    }

    // Departures first so a reused slot id is reported as leave + join.
    std::vector<rmm_player_t> gone;

    for (auto& kv : known_)
        if (!now.count(kv.first) || now[kv.first].name != std::string(kv.second.name))
            gone.push_back(kv.first);

    for (rmm_player_t id : gone) {
        leaving_ = id;
        core_->OnPlayerDisconnect(id);
        leaving_ = RMM_PLAYER_NONE;
        known_.erase(id);
        authed_.erase(id);
        keyedAuth_.erase(id);
        joinedAt_.erase(id);

        // With the destroyPlayer hook the hash is dropped exactly when the slot
        // is freed. A "departure" seen here may also be a rename (same player,
        // new name), whose hash must survive.
        if (!destroyHooked_)
            g_authIds.erase(id);
    }

    // AUTH fires once the CD-key callback has run for the player (bots and
    // servers without the auth hook: immediately). Core then checks bans.
    // The callback may never run (LAN server, CD-key checks off), so after
    // auth_timeout seconds the player is authed without a CD-key hash; if the
    // hash turns up later, AUTH fires again so CD-key bans and admins apply.
    auto tryAuth = [this](rmm_player_t id, const rmm_player_info& info) {
        if (!known_.count(id))
            return;

        bool haveKey = g_authIds.count(id) && !g_authIds[id].empty();

        if (authed_.count(id)) {
            if (haveKey && !keyedAuth_.count(id)) {
                keyedAuth_.insert(id);
                core_->OnPlayerAuth(id);  // late CD-key: re-check bans/admins with it
            }

            return;
        }

        double waited = core_->Time() - (joinedAt_.count(id) ? joinedAt_[id] : core_->Time());
        bool timedOut = authTimeout_ > 0 && waited >= authTimeout_;

        if (info.is_bot || !authHooked_ || g_authIds.count(id) || timedOut) {
            // Say which path authenticated the player: on a LAN server (or with
            // CD-key checks off) there is no hash, and CD-key bans/admins can't match.
            if (info.is_bot) {
            } else if (!authHooked_) {
                core_->Log().Info("%s: %s authenticated without a CD-key (auth hook unavailable)", shortName_.c_str(),
                                  info.name);
            } else if (g_authIds.count(id)) {
                if (haveKey)
                    core_->Log().Info("%s: %s authenticated by CD-key", shortName_.c_str(), info.name);
                else
                    core_->Log().Info("%s: %s authenticated, but the CD-key check returned no hash "
                                      "(LAN server or CD-key checks off); CD-key bans and admins won't match",
                                      shortName_.c_str(), info.name);
            } else {
                core_->Log().Info("%s: no CD-key check for %s after %.0f s (LAN server?); authenticating without a "
                                  "CD-key hash",
                                  shortName_.c_str(), info.name, waited);
            }

            authed_.insert(id);

            if (haveKey)
                keyedAuth_.insert(id);

            core_->OnPlayerAuth(id);
        }
    };

    for (auto& kv : now) {
        auto it = known_.find(kv.first);

        if (it == known_.end()) {
            known_[kv.first] = kv.second;
            joinedAt_[kv.first] = core_->Time();
            std::string reject;

            // Not blockable (we observe after the fact): banned players are kicked by the core.
            if (core_->OnPlayerConnect(kv.first, kv.second.name, kv.second.address, reject))
                tryAuth(kv.first, kv.second);

            continue;
        }

        tryAuth(kv.first, kv.second);
        it = known_.find(kv.first);

        if (it == known_.end())
            continue;  // kicked during the auth event

        if (it->second.team != kv.second.team) {
            rmm_ev_player_change ev{kv.first, it->second.team, kv.second.team};
            it->second = kv.second;
            core_->Dispatch(RMM_EV_PLAYER_TEAM, &ev);
        } else {
            it->second = kv.second;
        }
    }
}

}  // namespace rmm
