// rmm_launcher.exe - start a Refractor dedicated server with RMM loaded.
//
//   rmm_launcher.exe [--rmm <path\rmm.dll>] [--strict] [--server <exe>] [-- server args...]
//
// The server is created suspended, rmm.dll is loaded into it and
// RMM_Bootstrap runs before the server's own code, then the server resumes.
// If anything fails the server is started *without* RMM (unless --strict),
// so a broken RMM install never keeps a server offline.
//
// BF1942 / BFV: prefer the winmm.dll proxy loader (dist\winmm.dll next to
// the server): no injection, and RMM survives the server restarting itself
// as a new process after a map change, which this launcher can't follow.
//
// Must be built for the same architecture as the server (x86 for all four
// games on Windows).
#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "../host/inject_win.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

const char* kKnownServers[] = {"bf2_w32ded.exe", "BF2142_w32ded.exe", "BF1942_w32ded.exe", "BfVietnam_w32ded.exe"};

std::string LastError() {
    char buf[256] = {0};
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, GetLastError(), 0, buf,
                   sizeof buf, nullptr);

    return buf;
}

std::string FullPath(const std::string& p) {
    char buf[MAX_PATH] = {0};
    GetFullPathNameA(p.c_str(), MAX_PATH, buf, nullptr);

    return buf;
}

bool FileExists(const std::string& p) { return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

std::string QuoteArg(const std::string& a) {
    if (!a.empty() && a.find_first_of(" \t\"") == std::string::npos)
        return a;

    std::string out = "\"";

    for (char c : a) {
        if (c == '"')
            out += '\\';

        out += c;
    }

    return out + "\"";
}

}  // namespace

int main(int argc, char** argv) {
    std::string rmmDll, server;
    bool strict = false;
    std::vector<std::string> serverArgs;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];

        if (a == "--rmm" && i + 1 < argc)
            rmmDll = argv[++i];
        else if (a == "--server" && i + 1 < argc)
            server = argv[++i];
        else if (a == "--strict")
            strict = true;
        else if (a == "--") {
            for (++i; i < argc; ++i)
                serverArgs.push_back(argv[i]);
        } else
            serverArgs.push_back(a);  // pass unknown args through to the server
    }

    if (server.empty())
        for (const char* s : kKnownServers)
            if (FileExists(s)) {
                server = s;
                break;
            }

    if (server.empty()) {
        std::fprintf(stderr, "rmm_launcher: no Refractor server executable found here; use --server <exe>\n");

        return 2;
    }

    if (rmmDll.empty())
        rmmDll = FileExists("rmm\\bin\\rmm.dll") ? "rmm\\bin\\rmm.dll" : "rmm.dll";

    rmmDll = FullPath(rmmDll);
    server = FullPath(server);

    std::string cmdline = QuoteArg(server);

    for (auto& a : serverArgs)
        cmdline += " " + QuoteArg(a);

    std::vector<char> cl(cmdline.begin(), cmdline.end());
    cl.push_back('\0');

    STARTUPINFOA si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};

    if (!CreateProcessA(server.c_str(), cl.data(), nullptr, nullptr, TRUE, CREATE_SUSPENDED, nullptr, nullptr, &si,
                        &pi)) {
        std::fprintf(stderr, "rmm_launcher: cannot start %s: %s\n", server.c_str(), LastError().c_str());

        return 3;
    }

    std::string err;

    if (!FileExists(rmmDll))
        err = "rmm.dll not found at " + rmmDll;

    bool ok = err.empty() && rmm::inject::LoadInto(pi.hProcess, rmmDll, err);

    if (!ok) {
        std::fprintf(stderr, "rmm_launcher: RMM not loaded: %s\n", err.c_str());

        if (strict) {
            TerminateProcess(pi.hProcess, 1);

            return 4;
        }

        std::fprintf(stderr, "rmm_launcher: starting server without RMM\n");
    } else {
        std::printf("rmm_launcher: Refractor MultiMod loaded into %s\n", server.c_str());
    }

    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);

    return static_cast<int>(code);
}
