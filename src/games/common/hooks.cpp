#include "hooks.h"

#include "../../core/util.h"

#include <cctype>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    include <windows.h>
#    ifdef RMM_WITH_MINHOOK
#        include <MinHook.h>
#    endif
#else
#    include <dlfcn.h>
#    include <link.h>
#    ifdef RMM_WITH_FUNCHOOK
#        include <funchook.h>
#    endif
#endif

namespace rmm::hooks {

#ifdef _WIN32

bool PatchImport(void* importingModule, const char* dll, const char* func, void* replacement, void** original) {
    auto* base = static_cast<uint8_t*>(importingModule ? importingModule : GetModuleHandleA(nullptr));
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);

    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return false;

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];

    if (!dir.VirtualAddress)
        return false;

    for (auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (_stricmp(reinterpret_cast<const char*>(base + imp->Name), dll) != 0)
            continue;

        auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
        auto* orig =
            imp->OriginalFirstThunk ? reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->OriginalFirstThunk) : thunk;

        for (; orig->u1.AddressOfData; ++orig, ++thunk) {
            if (IMAGE_SNAP_BY_ORDINAL(orig->u1.Ordinal))
                continue;

            auto* byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + orig->u1.AddressOfData);

            if (std::strcmp(reinterpret_cast<const char*>(byName->Name), func) != 0)
                continue;

            DWORD old;

            if (!VirtualProtect(&thunk->u1.Function, sizeof(void*), PAGE_READWRITE, &old))
                return false;

            if (original)
                *original = reinterpret_cast<void*>(thunk->u1.Function);

            thunk->u1.Function = reinterpret_cast<ULONG_PTR>(replacement);
            VirtualProtect(&thunk->u1.Function, sizeof(void*), old, &old);

            return true;
        }
    }

    return false;
}

bool ModuleRange(void* module, uint8_t*& base, size_t& size) {
    base = static_cast<uint8_t*>(module ? module : GetModuleHandleA(nullptr));
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);

    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return false;

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    size = nt->OptionalHeader.SizeOfImage;

    return true;
}

bool Detour(void* target, void* detour, void** trampoline) {
#    ifdef RMM_WITH_MINHOOK
    static bool init = MH_Initialize() == MH_OK;

    if (!init)
        return false;

    if (MH_CreateHook(target, detour, trampoline) != MH_OK)
        return false;

    return MH_EnableHook(target) == MH_OK;
#    else
    (void)target;
    (void)detour;
    (void)trampoline;

    return false;
#    endif
}

void RemoveAllDetours() {
#    ifdef RMM_WITH_MINHOOK
    MH_DisableHook(MH_ALL_HOOKS);
#    endif
}

const char* LastDetourError() {
#    ifdef RMM_WITH_MINHOOK
    return "MinHook failed";
#    else
    return "RMM was built without RMM_WITH_MINHOOK";
#    endif
}

#else  // POSIX

bool PatchImport(void*, const char*, const char*, void*, void**) { return false; }

struct RangeQuery {
    const char* name;
    uint8_t* base;
    size_t size;
};

bool ModuleRange(void* module, uint8_t*& base, size_t& size) {
    // module == nullptr: main executable (first object in dl_iterate_phdr).
    RangeQuery q{nullptr, nullptr, 0};

    if (module) {
        Dl_info info{};

        if (!dladdr(module, &info))
            return false;

        q.name = info.dli_fname;
    }

    dl_iterate_phdr(
        [](dl_phdr_info* info, size_t, void* data) -> int {
            auto* q = static_cast<RangeQuery*>(data);
            bool isMain = !q->name && (!info->dlpi_name || !*info->dlpi_name);
            bool isNamed = q->name && info->dlpi_name && std::strcmp(info->dlpi_name, q->name) == 0;

            if (!isMain && !isNamed)
                return 0;

            uint8_t* lo = nullptr;
            uint8_t* hi = nullptr;

            for (int i = 0; i < info->dlpi_phnum; ++i) {
                const auto& ph = info->dlpi_phdr[i];

                if (ph.p_type != PT_LOAD)
                    continue;

                auto* s = reinterpret_cast<uint8_t*>(info->dlpi_addr + ph.p_vaddr);

                if (!lo || s < lo)
                    lo = s;

                if (!hi || s + ph.p_memsz > hi)
                    hi = s + ph.p_memsz;
            }

            q->base = lo;
            q->size = static_cast<size_t>(hi - lo);

            return 1;
        },
        &q);
    base = q.base;
    size = q.size;

    return base != nullptr;
}

#    ifdef RMM_WITH_FUNCHOOK
// One funchook handle per detour: a handle installs all of its prepared hooks
// at once and can't take more afterwards, and hooks are added one at a time.
static std::vector<funchook_t*>& Installed() {
    static std::vector<funchook_t*> v;

    return v;
}

static std::string& LastError() {
    static std::string e;

    return e;
}

bool Detour(void* target, void* detour, void** trampoline) {
    funchook_t* fh = funchook_create();

    if (!fh) {
        LastError() = "funchook_create failed";

        return false;
    }

    // funchook_prepare relocates the overwritten prologue into a trampoline
    // and replaces `fn` with it, so the original stays callable.
    void* fn = target;

    if (funchook_prepare(fh, &fn, detour) != 0 || funchook_install(fh, 0) != 0) {
        LastError() = funchook_error_message(fh);
        funchook_destroy(fh);

        return false;
    }

    if (trampoline)
        *trampoline = fn;

    Installed().push_back(fh);

    return true;
}

void RemoveAllDetours() {
    for (funchook_t* fh : Installed()) {
        funchook_uninstall(fh, 0);
        funchook_destroy(fh);
    }

    Installed().clear();
}

const char* LastDetourError() { return LastError().c_str(); }
#    else
bool Detour(void*, void*, void**) { return false; }  // build with RMM_WITH_FUNCHOOK

void RemoveAllDetours() {}

const char* LastDetourError() { return "RMM was built without RMM_WITH_FUNCHOOK"; }
#    endif

#endif

bool ParsePattern(const std::string& text, std::vector<uint8_t>& bytes, std::vector<bool>& mask) {
    bytes.clear();
    mask.clear();

    for (auto& tok : SplitArgs(text)) {
        if (tok == "?" || tok == "??") {
            bytes.push_back(0);
            mask.push_back(false);
            continue;
        }

        if (tok.size() != 2 || !std::isxdigit(static_cast<unsigned char>(tok[0])) ||
            !std::isxdigit(static_cast<unsigned char>(tok[1])))
            return false;

        bytes.push_back(static_cast<uint8_t>(std::strtoul(tok.c_str(), nullptr, 16)));
        mask.push_back(true);
    }

    return !bytes.empty();
}

void* FindPattern(void* module, const std::string& pattern) {
    std::vector<uint8_t> bytes;
    std::vector<bool> mask;
    uint8_t* base;
    size_t size;

    if (!ParsePattern(pattern, bytes, mask) || !ModuleRange(module, base, size) || size < bytes.size())
        return nullptr;

    // Scans the whole image; fine for one-time startup scans.
    void* found = nullptr;

    for (size_t i = 0; i + bytes.size() <= size; ++i) {
        size_t j = 0;

        while (j < bytes.size() && (!mask[j] || base[i + j] == bytes[j]))
            ++j;

        if (j == bytes.size()) {
            if (found)
                return nullptr;  // ambiguous

            found = base + i;
        }
    }

    return found;
}

}  // namespace rmm::hooks
