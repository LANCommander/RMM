// Platform-independent part of the Refractor 1 adapter. The engine hooks for
// both server builds live in r1_native.cpp.
#include "r1_adapter.h"

#include "../../core/core.h"
#include "../../core/platform.h"
#include "../common/hooks.h"

#include <cstdlib>

namespace rmm {

R1Adapter* R1Adapter::instance_ = nullptr;

R1Adapter::R1Adapter(rmm_game game) : game_(game) { shortName_ = game == RMM_GAME_BFV ? "bfv" : "bf1942"; }

const char* R1Adapter::GameName() const { return game_ == RMM_GAME_BFV ? "Battlefield Vietnam" : "Battlefield 1942"; }

bool R1Adapter::Attach(Core& core, std::string& err) {
    core_ = &core;
    instance_ = this;

    if (!LoadGameData(core.BuildPath(RMM_PATH_GAMEDATA, ""), err)) {
        instance_ = nullptr;

        return false;
    }

    bool ok = AttachNative(err);

    if (!ok) {
        hooks::RemoveAllDetours();
        instance_ = nullptr;
    }

    return ok;
}

void R1Adapter::Detach() {
    hooks::RemoveAllDetours();
    instance_ = nullptr;
}

void R1Adapter::OnEngineFrame() {
    // Plugins load once the console is usable (main console found).
    if (!core_->Ready() && CapOverride(RMM_CAP_CONSOLE_EXEC) == 1)
        core_->OnGameReady();

    if (!core_->Ready())
        return;

    NativeFrame();
    core_->Frame();
}

uint32_t R1Adapter::EventFlags(int32_t ev) const {
    auto it = eventFlags_.find(ev);

    return it == eventFlags_.end() ? 0 : it->second;
}

bool R1Adapter::Exec(const std::string& command, std::string* output) { return NativeExec(command, output); }

void R1Adapter::ConsolePrint(const std::string& line) {
    // The engine's own console (mainConsole->output), so lines also reach the
    // console window and remote-console clients.
    if (NativeConsolePrint(line))
        return;

    plat::DebugOut(line);  // fallback: process stderr
}

int32_t R1Adapter::MaxPlayers() {
    std::string v;

    if (GetEngineVar(SettingVar(RMM_SETTING_MAX_PLAYERS), v) && std::atoi(v.c_str()) > 0)
        return std::atoi(v.c_str());

    return 64;
}

}  // namespace rmm
