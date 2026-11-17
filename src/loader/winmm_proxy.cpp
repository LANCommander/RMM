// winmm.dll - loads Refractor MultiMod into a Windows dedicated server
// without any process injection.
//
// Copy it next to the server executable (BF1942_w32ded.exe imports winmm.dll,
// and the application directory is searched before the system directory).
// It is a full proxy: every export jumps to the real %SystemRoot% winmm.dll.
// While the server's imports are being resolved, DllMain points the
// executable's entry point at RmmEntry, which runs first on the main thread,
// outside the loader lock: it loads rmm\bin\rmm.dll and calls RMM_Bootstrap,
// restores the entry point and continues into the server's own startup. A
// server that restarts by starting a new copy of itself (BF1942 does, after a
// map change) gets RMM again the same way.
//
// Without rmm\bin\rmm.dll (or with RMM_DISABLE=1 in the environment) it only
// forwards. 32-bit only: the stubs are naked x86 jumps.
#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>
#include <cstring>

extern "C" {
// One slot per export (winmm_exports.inc), filled from the real DLL.
void* g_proxyTable[256] = {};
}

#define RMM_PROXY_EXPORT(i, ord, name)                                                                                 \
    extern "C" __declspec(naked) void rmm_proxy_##i() { __asm jmp dword ptr[g_proxyTable + (i) * 4] }
#include "winmm_exports.inc"
#undef RMM_PROXY_EXPORT

namespace {

struct Export {
    int ordinal;
    const char* name;
};

const Export kExports[] = {
#define RMM_PROXY_EXPORT(i, ord, name) {ord, name},
#include "winmm_exports.inc"
#undef RMM_PROXY_EXPORT
};

static_assert(sizeof kExports / sizeof kExports[0] <= sizeof g_proxyTable / sizeof g_proxyTable[0], "table too small");

HMODULE g_real = nullptr;
uint8_t* g_entry = nullptr;  // the executable's entry point
uint8_t g_saved[5] = {};     // its first bytes, replaced by a jump to RmmEntry
char g_rmmDll[MAX_PATH] = {};

void Say(const char* msg) {
    // The server has a console; stderr reaches it. Also visible in a debugger.
    OutputDebugStringA(msg);
    DWORD n = 0;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), msg, static_cast<DWORD>(std::strlen(msg)), &n, nullptr);
}

bool LoadReal() {
    char path[MAX_PATH] = {};
    UINT n = GetSystemDirectoryA(path, MAX_PATH);  // SysWOW64 for this 32-bit process (WOW64 redirection)

    if (n == 0 || n + 11 >= MAX_PATH)
        return false;

    std::strcat(path, "\\winmm.dll");
    g_real = LoadLibraryA(path);

    if (!g_real)
        return false;

    for (size_t i = 0; i < sizeof kExports / sizeof kExports[0]; ++i) {
        const char* what = kExports[i].name[0] ? kExports[i].name : MAKEINTRESOURCEA(kExports[i].ordinal);
        g_proxyTable[i] = reinterpret_cast<void*>(GetProcAddress(g_real, what));
    }

    return true;
}

/// rmm\bin\rmm.dll for this server: %RMM_ROOT%, else <exe dir>\rmm. Sets
/// RMM_ROOT for RMM (and for a restarted server) when it wasn't set.
bool FindRmm() {
    char root[MAX_PATH] = {};

    if (GetEnvironmentVariableA("RMM_DISABLE", root, MAX_PATH) && root[0] == '1')
        return false;

    if (!GetEnvironmentVariableA("RMM_ROOT", root, MAX_PATH)) {
        GetModuleFileNameA(nullptr, root, MAX_PATH);
        char* slash = std::strrchr(root, '\\');

        if (!slash)
            return false;

        std::strcpy(slash + 1, "rmm");
        SetEnvironmentVariableA("RMM_ROOT", root);
    }

    if (std::strlen(root) + 16 >= MAX_PATH)
        return false;

    std::strcpy(g_rmmDll, root);
    std::strcat(g_rmmDll, "\\bin\\rmm.dll");

    return GetFileAttributesA(g_rmmDll) != INVALID_FILE_ATTRIBUTES;
}

extern "C" void __cdecl RmmStart() {
    // Put the entry point back first: whatever happens below, the server starts.
    DWORD old = 0;

    if (VirtualProtect(g_entry, sizeof g_saved, PAGE_EXECUTE_READWRITE, &old)) {
        std::memcpy(g_entry, g_saved, sizeof g_saved);
        VirtualProtect(g_entry, sizeof g_saved, old, &old);
        FlushInstructionCache(GetCurrentProcess(), g_entry, sizeof g_saved);
    }

    HMODULE rmm = LoadLibraryA(g_rmmDll);

    if (!rmm) {
        Say("[RMM] winmm.dll: could not load rmm\\bin\\rmm.dll; the server runs without RMM\n");

        return;
    }

    using BootFn = unsigned long(__stdcall*)(void*);
    auto boot = reinterpret_cast<BootFn>(reinterpret_cast<void*>(GetProcAddress(rmm, "RMM_Bootstrap")));

    if (!boot)
        boot = reinterpret_cast<BootFn>(reinterpret_cast<void*>(GetProcAddress(rmm, "_RMM_Bootstrap@4")));

    if (!boot) {
        Say("[RMM] winmm.dll: rmm.dll has no RMM_Bootstrap; the server runs without RMM\n");

        return;
    }

    if (boot(nullptr) != 0)
        Say("[RMM] RMM did not start (see rmm\\logs); the server runs without RMM\n");
}

// Replaces the executable's entry point once. Registers and stack are kept as
// the loader left them, then the real entry point runs.
extern "C" __declspec(naked) void RmmEntry() {
    __asm {
        pushad
        pushfd
        call RmmStart
        popfd
        popad
        jmp dword ptr [g_entry]
    }
}

void HookEntry() {
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleA(nullptr));
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);

    if (nt->FileHeader.Characteristics & IMAGE_FILE_DLL)
        return;

    g_entry = base + nt->OptionalHeader.AddressOfEntryPoint;
    DWORD old = 0;

    if (!VirtualProtect(g_entry, sizeof g_saved, PAGE_EXECUTE_READWRITE, &old))
        return;

    std::memcpy(g_saved, g_entry, sizeof g_saved);
    int32_t rel = static_cast<int32_t>(reinterpret_cast<uint8_t*>(&RmmEntry) - (g_entry + 5));
    g_entry[0] = 0xE9;  // jmp rel32
    std::memcpy(g_entry + 1, &rel, sizeof rel);
    VirtualProtect(g_entry, sizeof g_saved, old, &old);
    FlushInstructionCache(GetCurrentProcess(), g_entry, sizeof g_saved);
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);

        // Loading a system DLL from DllMain is what every proxy has to do; the
        // real winmm.dll is already in the process in most cases.
        if (!LoadReal())
            return FALSE;

        if (FindRmm())
            HookEntry();
    }

    // The real winmm.dll stays loaded until the process ends.
    return TRUE;
}
