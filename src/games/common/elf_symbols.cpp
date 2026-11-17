#include "elf_symbols.h"

#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

#ifdef _WIN32
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    include <windows.h>
#else
#    include <link.h>
#    include <sys/uio.h>
#    include <unistd.h>
#endif

namespace rmm::elf {

namespace {

// Minimal ELF definitions (avoid depending on <elf.h> so this compiles on Windows too).
struct Ehdr32 {
    unsigned char ident[16];
    uint16_t type, machine;
    uint32_t version, entry, phoff, shoff, flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};

struct Shdr32 {
    uint32_t name, type, flags, addr, offset, size, link, info, addralign, entsize;
};

struct Sym32 {
    uint32_t name, value, size;
    unsigned char info, other;
    uint16_t shndx;
};

struct Ehdr64 {
    unsigned char ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};

struct Shdr64 {
    uint32_t name, type;
    uint64_t flags, addr, offset, size;
    uint32_t link, info;
    uint64_t addralign, entsize;
};

struct Sym64 {
    uint32_t name;
    unsigned char info, other;
    uint16_t shndx;
    uint64_t value, size;
};

// Prefixed: <link.h> pulls in <elf.h>, whose SHT_*/STT_* are macros.
constexpr uint32_t kShtSymtab = 2;
constexpr unsigned kSttObject = 1, kSttFunc = 2;

template <class Ehdr, class Shdr, class Sym>
bool Parse(const std::vector<char>& f, uintptr_t bias, std::unordered_map<std::string, uintptr_t>& out,
           std::string& err) {
    if (f.size() < sizeof(Ehdr)) {
        err = "truncated ELF header";

        return false;
    }

    Ehdr eh;
    std::memcpy(&eh, f.data(), sizeof eh);

    if (eh.shoff == 0 || eh.shentsize != sizeof(Shdr) || eh.shoff + uint64_t(eh.shnum) * sizeof(Shdr) > f.size()) {
        err = "no usable section headers";

        return false;
    }

    auto shdr = [&](size_t i) {
        Shdr s;
        std::memcpy(&s, f.data() + eh.shoff + i * sizeof(Shdr), sizeof s);

        return s;
    };

    for (size_t i = 0; i < eh.shnum; ++i) {
        Shdr sym = shdr(i);

        if (sym.type != kShtSymtab || sym.entsize != sizeof(Sym) || sym.link >= eh.shnum)
            continue;

        Shdr str = shdr(sym.link);

        if (sym.offset + sym.size > f.size() || str.offset + str.size > f.size()) {
            err = "symtab out of range";

            return false;
        }

        size_t n = static_cast<size_t>(sym.size / sizeof(Sym));

        for (size_t k = 0; k < n; ++k) {
            Sym s;
            std::memcpy(&s, f.data() + sym.offset + k * sizeof(Sym), sizeof s);
            unsigned type = s.info & 0xF;

            if (s.shndx == 0 || s.value == 0 || (type != kSttFunc && type != kSttObject))
                continue;  // undefined/irrelevant

            if (s.name >= str.size)
                continue;

            const char* name = f.data() + str.offset + s.name;
            size_t maxLen = static_cast<size_t>(str.size - s.name);
            std::string key(name, strnlen(name, maxLen));

            if (!key.empty())
                out.emplace(std::move(key), static_cast<uintptr_t>(s.value) + bias);
        }

        return true;
    }

    err = "executable has no .symtab (stripped)";

    return false;
}

}  // namespace

bool SymbolTable::Load(const std::string& path, uintptr_t bias, std::string& err) {
    std::ifstream in(path, std::ios::binary);

    if (!in) {
        err = "cannot open " + path;

        return false;
    }

    std::vector<char> f((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    if (f.size() < 16 || std::memcmp(f.data(),
                                     "\x7f"
                                     "ELF",
                                     4) != 0) {
        err = path + " is not an ELF file";

        return false;
    }

    if (f[5] != 1) {
        err = "big-endian ELF not supported";

        return false;
    }

    syms_.clear();

    if (f[4] == 1)
        return Parse<Ehdr32, Shdr32, Sym32>(f, bias, syms_, err);

    if (f[4] == 2)
        return Parse<Ehdr64, Shdr64, Sym64>(f, bias, syms_, err);

    err = "unknown ELF class";

    return false;
}

bool SymbolTable::LoadSelf(std::string& err) {
#ifdef _WIN32
    err = "ELF symbol tables are only available on Linux";

    return false;
#else
    // Non-PIE executables (all Refractor servers) have bias 0; PIE ones report
    // their load address as the first dl_iterate_phdr entry.
    uintptr_t bias = 0;
    dl_iterate_phdr(
        [](dl_phdr_info* info, size_t, void* data) -> int {
            *static_cast<uintptr_t*>(data) = static_cast<uintptr_t>(info->dlpi_addr);

            return 1;  // first entry is the main program
        },
        &bias);

    return Load("/proc/self/exe", bias, err);
#endif
}

uintptr_t SymbolTable::Find(const std::string& name) const {
    auto it = syms_.find(name);

    return it == syms_.end() ? 0 : it->second;
}

SymbolTable& Self() {
    static SymbolTable table;

    return table;
}

bool SafeRead(const void* address, void* out, size_t size) {
    if (!address)
        return false;
#ifdef _WIN32
    SIZE_T got = 0;

    return ReadProcessMemory(GetCurrentProcess(), address, out, size, &got) && got == size;
#else
    iovec local{out, size};
    iovec remote{const_cast<void*>(address), size};
    ssize_t n = process_vm_readv(getpid(), &local, 1, &remote, 1, 0);

    if (n == static_cast<ssize_t>(size))
        return true;

    // process_vm_readv can be blocked (seccomp/old kernels); fall back to a
    // pipe write, which also validates the source range without faulting.
    static int fds[2] = {-1, -1};

    if (fds[0] < 0 && pipe(fds) != 0)
        return false;

    if (write(fds[1], address, size) != static_cast<ssize_t>(size))
        return false;

    return read(fds[0], out, size) == static_cast<ssize_t>(size);
#endif
}

}  // namespace rmm::elf
