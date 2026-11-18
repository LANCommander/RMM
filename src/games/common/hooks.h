// Low-level hooking helpers used by native adapters.
//
//  - IAT patching (Windows): redirect a module's import of DLL!Function.
//    Zero dependencies; ideal for hooking engine -> runtime-library calls
//    such as bf2_w32ded.exe -> dice_py.dll!Py_Initialize.
//  - Signature scanning: find engine functions by byte pattern, so hooks
//    survive minor patches (patterns live in gamedata [signatures]).
//  - Inline detours: MinHook on Windows (RMM_WITH_MINHOOK), funchook on Linux
//    (RMM_WITH_FUNCHOOK) - needed for hooking functions *inside* the engine.
//    Both relocate the target's prologue into a trampoline so the original
//    can still be called.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace rmm::hooks {

/// Patch @p importingModule's IAT entry for @p dll!@p func to @p replacement.
/// @p original receives the previous pointer. importingModule = nullptr means
/// the main executable. @p func "#<n>" matches an import by ordinal n.
/// Windows only; returns false elsewhere.
bool PatchImport(void* importingModule, const char* dll, const char* func, void* replacement, void** original);

/// Parse "55 8B EC ?? ?? 83 EC" (?? = wildcard) into bytes + mask.
bool ParsePattern(const std::string& text, std::vector<uint8_t>& bytes, std::vector<bool>& mask);

/// Scan the whole image of @p module (nullptr = main exe) for a
/// pattern. Returns nullptr if not found or if found more than once
/// (ambiguous signatures are treated as broken).
void* FindPattern(void* module, const std::string& pattern);

/// Image base and size of a loaded module.
bool ModuleRange(void* module, uint8_t*& base, size_t& size);

/// Install an inline detour (MinHook / funchook). Returns false if unavailable.
bool Detour(void* target, void* detour, void** trampoline);
void RemoveAllDetours();
/// Why the last Detour() failed (for logs).
const char* LastDetourError();

}  // namespace rmm::hooks
