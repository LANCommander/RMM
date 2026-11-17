#include "gcc3_string.h"

#include "../common/elf_symbols.h"

#include <cstdint>
#include <cstring>

#ifndef _WIN32
#    include <dlfcn.h>
#endif

namespace rmm::gcc3 {

namespace {

// Linux i386 member functions are plain cdecl with `this` first.
using CtorFn = void (*)(String* self, const char* text, const void* allocator);
using DtorFn = void (*)(String* self);
using AssignFn = String* (*)(String* self, const char* text, size_t len);

CtorFn g_ctor = nullptr;
DtorFn g_dtor = nullptr;
AssignFn g_assign = nullptr;

// Mangled names are identical in GCC 3.2+ (Itanium ABI).
constexpr const char* kCtor = "_ZNSsC1EPKcRKSaIcE";   // std::string::string(const char*, const allocator&)
constexpr const char* kDtor = "_ZNSsD1Ev";            // std::string::~string()
constexpr const char* kAssign = "_ZNSs6assignEPKcj";  // std::string::assign(const char*, size_t)

void* Lookup(void* lib, const char* name) {
    // 1) Statically linked server: the function lives in the executable.
    if (uintptr_t a = elf::Self().Find(name))
        return reinterpret_cast<void*>(a);
#ifndef _WIN32
    // 2) Dynamic server: libstdc++.so.5, explicitly the GCC 3 symbol version
    //    so we never pick up libstdc++.so.6's identically-named functions.
    if (lib) {
        if (void* p = dlvsym(lib, name, "GLIBCPP_3.2"))
            return p;

        return dlsym(lib, name);
    }
#else
    (void)lib;
#endif
    return nullptr;
}

}  // namespace

bool Init(std::string& error) {
    if (Ready())
        return true;

    void* lib = nullptr;
#ifndef _WIN32
    lib = dlopen("libstdc++.so.5", RTLD_NOW | RTLD_NOLOAD);
#endif
    g_ctor = reinterpret_cast<CtorFn>(Lookup(lib, kCtor));
    g_dtor = reinterpret_cast<DtorFn>(Lookup(lib, kDtor));
    g_assign = reinterpret_cast<AssignFn>(Lookup(lib, kAssign));

    if (!Ready()) {
        error = "GCC 3 std::string functions not found (libstdc++.so.5 not loaded and not in .symtab)";

        return false;
    }

    return true;
}

bool Ready() { return g_ctor && g_dtor && g_assign; }

DynamicCastFn DynamicCast() {
    // Resolved on first use (never from the LD_PRELOAD constructor); only a
    // successful lookup is cached.
    static DynamicCastFn fn = nullptr;

    if (fn)
        return fn;

    if (uintptr_t a = elf::Self().Find("__dynamic_cast"))
        return fn = reinterpret_cast<DynamicCastFn>(a);
#ifndef _WIN32
    // libstdc++.so.5 exports it as CXXABI_1.2; libstdc++.so.6 (ours) has no
    // such version, so this never picks up the modern runtime's copy.
    if (void* lib = dlopen("libstdc++.so.5", RTLD_NOW | RTLD_NOLOAD))
        if (void* p = dlvsym(lib, "__dynamic_cast", "CXXABI_1.2"))
            fn = reinterpret_cast<DynamicCastFn>(p);
#endif
    return fn;
}

void Construct(String* s, const char* text) {
    char allocator = 0;  // std::allocator<char> is empty; any address will do
    g_ctor(s, text ? text : "", &allocator);
}

void Destroy(String* s) { g_dtor(s); }

void Assign(String* s, const std::string& text) { g_assign(s, text.data(), text.size()); }

size_t Length(const String* s) {
    // _Rep { size_t length; size_t capacity; int refcount; } precedes the data.
    size_t len = 0;

    if (!s || !elf::SafeRead(s->data - 3 * sizeof(size_t), &len, sizeof len))
        return 0;

    return len;
}

std::string Read(const String* s) {
    if (!s || !s->data)
        return {};

    size_t len = Length(s);

    if (len > 1u << 20)
        return {};  // implausible: treat as corrupt

    std::string out(len, '\0');

    if (len && !elf::SafeRead(s->data, &out[0], len))
        return {};

    return out;
}

}  // namespace rmm::gcc3
