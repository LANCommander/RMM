// Resolve symbols from the running executable's full ELF symbol table.
//
// Old Refractor Linux servers (bf1942_lnxded) ship with .symtab intact but
// export almost nothing dynamically, so dlsym() cannot find engine functions.
// This reads /proc/self/exe once and indexes every defined FUNC/OBJECT symbol
// by its (mangled) name, so gamedata can say
//     console_parser = _ZN4dice4ref27console6Parser13handleCommandERKSsRSsNS0_10AccessTypeE
// and keep working across server builds without byte signatures.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

namespace rmm::elf {

class SymbolTable {
public:
    /// Parse the main executable. Returns false (with error) if it is not an
    /// ELF file, has no .symtab, or is not 32/64-bit little endian.
    bool LoadSelf(std::string& error);
    bool Load(const std::string& path, uintptr_t loadBias, std::string& error);

    /// Runtime address of a defined symbol, or 0.
    uintptr_t Find(const std::string& mangledName) const;

    size_t Size() const { return syms_.size(); }

private:
    std::unordered_map<std::string, uintptr_t> syms_;
};

/// Shared instance for the process; the adapter calls LoadSelf() on it once at attach.
SymbolTable& Self();

/// Read memory without crashing on a bad pointer (process_vm_readv on
/// Linux; ReadProcessMemory on Windows). Returns false if unreadable.
bool SafeRead(const void* address, void* out, size_t size);

template <class T> bool SafeReadT(uintptr_t address, T& out) {
    return SafeRead(reinterpret_cast<const void*>(address), &out, sizeof(T));
}

}  // namespace rmm::elf
