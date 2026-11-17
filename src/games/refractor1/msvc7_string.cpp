#include "msvc7_string.h"

#ifdef _WIN32

#    include "../common/elf_symbols.h"

#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    include <windows.h>

namespace rmm::msvc7 {

namespace {

// bfcprt.dll exports (MSVC member functions: __thiscall).
using CtorFn = String*(__thiscall*)(String* self, const char* text);
using DtorFn = void(__thiscall*)(String* self);
using AssignFn = String*(__thiscall*)(String* self, const char* text, size_t len);

CtorFn g_ctor = nullptr;
DtorFn g_dtor = nullptr;
AssignFn g_assign = nullptr;

// std::basic_string<char>::basic_string(const char*)
constexpr const char* kCtor = "??0?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@QAE@PBD@Z";
// std::basic_string<char>::~basic_string()
constexpr const char* kDtor = "??1?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@QAE@XZ";
// std::basic_string<char>::assign(const char*, size_t)
constexpr const char* kAssign = "?assign@?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@QAEAAV12@PBDI@Z";

}  // namespace

bool Init(std::string& error) {
    if (Ready())
        return true;

    HMODULE lib = GetModuleHandleA("bfcprt.dll");

    if (!lib) {
        error = "bfcprt.dll (the server's C++ runtime) is not loaded";

        return false;
    }

    g_ctor = reinterpret_cast<CtorFn>(reinterpret_cast<void*>(GetProcAddress(lib, kCtor)));
    g_dtor = reinterpret_cast<DtorFn>(reinterpret_cast<void*>(GetProcAddress(lib, kDtor)));
    g_assign = reinterpret_cast<AssignFn>(reinterpret_cast<void*>(GetProcAddress(lib, kAssign)));

    if (!Ready()) {
        error = "bfcprt.dll does not export the VC7 std::string functions";

        return false;
    }

    return true;
}

bool Ready() { return g_ctor && g_dtor && g_assign; }

void Construct(String* s, const char* text) { g_ctor(s, text ? text : ""); }

void Destroy(String* s) { g_dtor(s); }

void Assign(String* s, const std::string& text) { g_assign(s, text.data(), text.size()); }

size_t Length(const String* s) {
    String copy{};

    if (!s || !elf::SafeRead(s, &copy, sizeof copy))
        return 0;

    return copy.size;
}

std::string Read(const String* s) {
    String copy{};

    if (!s || !elf::SafeRead(s, &copy, sizeof copy))
        return {};

    if (copy.size > (1u << 20) || copy.size > copy.capacity)
        return {};  // implausible: treat as corrupt

    if (copy.capacity < sizeof copy.bx.buf)
        return std::string(copy.bx.buf, copy.size);

    std::string out(copy.size, '\0');

    if (copy.size && !elf::SafeRead(copy.bx.ptr, &out[0], copy.size))
        return {};

    return out;
}

}  // namespace rmm::msvc7

#endif
