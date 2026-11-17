// Entry point of the RMM core module (rmm.dll / librmm.so) inside a game
// server process. Detects the game, creates the right adapter and starts the
// core. How the module gets into the process:
//
//   Windows: rmm_launcher.exe starts the server suspended, injects rmm.dll and
//            calls the exported RMM_Bootstrap before the server's main thread
//            runs (so Py_Initialize etc. can still be hooked).
//   Linux:   LD_PRELOAD=/path/librmm.so ./bf2 ...   - a constructor runs
//            RMM_Bootstrap automatically.
#include "../core/core.h"
#include "../core/platform.h"
#include "../core/util.h"
#include "../games/refractor1/r1_adapter.h"
#include "../games/refractor2/r2_adapter.h"

#include <cstdlib>
#include <memory>

#ifdef _WIN32
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    include <windows.h>
#endif

namespace {

rmm_game DetectGame() {
    std::string forced = rmm::ToLower(rmm::plat::GetEnv("RMM_GAME"));

    if (!forced.empty()) {
        if (forced == "bf1942")
            return RMM_GAME_BF1942;

        if (forced == "bfv")
            return RMM_GAME_BFV;

        if (forced == "bf2")
            return RMM_GAME_BF2;

        if (forced == "bf2142")
            return RMM_GAME_BF2142;
    }

    // Executable names of the dedicated servers. TODO(verify) Linux names and
    // the exact BFV executable name.
    std::string exe = rmm::ToLower(rmm::plat::FileStem(rmm::plat::ExecutablePath()));

    if (exe.find("bf2142") != std::string::npos)
        return RMM_GAME_BF2142;

    if (exe.find("bf2") != std::string::npos)
        return RMM_GAME_BF2;

    if (exe.find("vietnam") != std::string::npos || exe.find("bfv") != std::string::npos)
        return RMM_GAME_BFV;

    if (exe.find("1942") != std::string::npos)
        return RMM_GAME_BF1942;

    return RMM_GAME_UNKNOWN;
}

std::unique_ptr<rmm::IGameAdapter> MakeAdapter(rmm_game g) {
    switch (g) {
        case RMM_GAME_BF1942:
        case RMM_GAME_BFV:
            return std::make_unique<rmm::R1Adapter>(g);
        case RMM_GAME_BF2:
        case RMM_GAME_BF2142:
            return std::make_unique<rmm::R2Adapter>(g);
        default:
            return nullptr;
    }
}

bool g_started = false;

void ShutdownAtExit() {
    if (g_started)
        rmm::Core::Get().Shutdown();
}

}  // namespace

#ifdef _WIN32
#    define RMM_BOOT_CALL __stdcall  // matches LPTHREAD_START_ROUTINE (CreateRemoteThread)
#else
#    define RMM_BOOT_CALL
#endif

/// Start RMM. Safe to call more than once. Returns 0 on success.
/// Note: on 32-bit Windows the export is decorated as _RMM_Bootstrap@4;
/// rmm_launcher looks up both spellings.
RMM_EXTERN_C RMM_DLLEXPORT unsigned long RMM_BOOT_CALL RMM_Bootstrap(void* /*unused*/) {
    if (g_started)
        return 0;

    std::string root = rmm::plat::GetEnv("RMM_ROOT");

    if (root.empty())
        root = rmm::plat::JoinPath(rmm::plat::CurrentDir(), "rmm");

    rmm_game game = DetectGame();
    auto adapter = MakeAdapter(game);

    if (!adapter) {
        rmm::plat::DebugOut("[RMM] Unrecognised server executable '" + rmm::plat::ExecutablePath() +
                            "'. Set RMM_GAME=bf1942|bfv|bf2|bf2142 to override.");

        return 1;
    }

    std::string err;

    if (!rmm::Core::Get().Start(std::move(adapter), root, err)) {
        rmm::plat::DebugOut("[RMM] Not started: " + err);

        return 2;
    }

    g_started = true;
    std::atexit(&ShutdownAtExit);

    return 0;
}

#ifdef _WIN32
BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    // Nothing here: loader-lock rules forbid real work. The launcher calls
    // RMM_Bootstrap explicitly.
    (void)reason;

    return TRUE;
}
#else
__attribute__((constructor)) static void RmmAutoStart() {
    if (rmm::plat::GetEnv("RMM_NO_AUTOSTART").empty())
        RMM_Bootstrap(nullptr);
}
#endif
