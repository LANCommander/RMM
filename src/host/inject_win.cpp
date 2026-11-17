#include "inject_win.h"

#ifdef _WIN32

#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    include <windows.h>

#    include <cstdint>

namespace rmm::inject {

namespace {

std::string LastError() {
    char buf[256] = {0};
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, GetLastError(), 0, buf,
                   sizeof buf, nullptr);
    std::string s = buf;

    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
        s.pop_back();

    return s;
}

DWORD RunRemote(HANDLE proc, LPTHREAD_START_ROUTINE fn, void* arg, std::string& err) {
    HANDLE th = CreateRemoteThread(proc, nullptr, 0, fn, arg, 0, nullptr);

    if (!th) {
        err = "CreateRemoteThread: " + LastError();

        return 0xFFFFFFFF;
    }

    WaitForSingleObject(th, INFINITE);
    DWORD code = 0xFFFFFFFF;
    GetExitCodeThread(th, &code);
    CloseHandle(th);

    return code;
}

}  // namespace

std::string ThisModulePath() {
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&ThisModulePath), &self);
    char buf[MAX_PATH] = {0};
    GetModuleFileNameA(self, buf, MAX_PATH);

    return buf;
}

bool LoadInto(void* process, const std::string& dll, std::string& err) {
    HANDLE proc = static_cast<HANDLE>(process);
    // 1. LoadLibraryA(dll) in the target. kernel32 is mapped at the same
    //    address in every process of a session, so our LoadLibraryA is theirs.
    void* mem = VirtualAllocEx(proc, nullptr, dll.size() + 1, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

    if (!mem) {
        err = "VirtualAllocEx: " + LastError();

        return false;
    }

    WriteProcessMemory(proc, mem, dll.c_str(), dll.size() + 1, nullptr);
    auto loadLib = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryA")));
    DWORD remoteBase = RunRemote(proc, loadLib, mem, err);
    VirtualFreeEx(proc, mem, 0, MEM_RELEASE);

    if (remoteBase == 0 || remoteBase == 0xFFFFFFFF) {
        if (err.empty())
            err = "LoadLibrary failed inside the server (missing dependency or wrong architecture?)";

        return false;
    }

    // 2. Find RMM_Bootstrap's RVA from a local image of the DLL and call it remotely.
    HMODULE local = LoadLibraryExA(dll.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);

    if (!local) {
        err = "cannot map rmm.dll locally: " + LastError();

        return false;
    }

    FARPROC fn = GetProcAddress(local, "RMM_Bootstrap");

    if (!fn)
        fn = GetProcAddress(local, "_RMM_Bootstrap@4");

    uintptr_t rva = fn ? reinterpret_cast<uintptr_t>(fn) - reinterpret_cast<uintptr_t>(local) : 0;
    FreeLibrary(local);

    if (!rva) {
        err = "rmm.dll has no RMM_Bootstrap export";

        return false;
    }

    auto remoteFn = reinterpret_cast<LPTHREAD_START_ROUTINE>(static_cast<uintptr_t>(remoteBase) + rva);
    DWORD rc = RunRemote(proc, remoteFn, nullptr, err);

    if (rc != 0) {
        err = "RMM_Bootstrap returned " + std::to_string(rc) + " (see rmm/logs)";

        return false;
    }

    return true;
}

}  // namespace rmm::inject

#endif
