#include "safe_call.h"

#include <cstdio>
#include <exception>

#if defined(_MSC_VER)
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    include <windows.h>
#    include <malloc.h>  // _resetstkoflw
#endif

namespace rmm {

#if defined(_MSC_VER)

static const char* DescribeSeh(unsigned long code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:
            return "access violation";
        case EXCEPTION_STACK_OVERFLOW:
            return "stack overflow";
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
            return "integer divide by zero";
        case EXCEPTION_ILLEGAL_INSTRUCTION:
            return "illegal instruction";
        case EXCEPTION_PRIV_INSTRUCTION:
            return "privileged instruction";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
            return "array bounds exceeded";
        case EXCEPTION_DATATYPE_MISALIGNMENT:
            return "datatype misalignment";
        case EXCEPTION_IN_PAGE_ERROR:
            return "in-page error";
        case 0xE06D7363u:
            return "unhandled C++ exception";
        default:
            return "structured exception";
    }
}

// Must not contain objects with destructors (C2712), hence the split.
static int SehInvoke(void (*thunk)(void*), void* ctx, unsigned long* code, void** addr) {
    EXCEPTION_POINTERS* info = nullptr;
    __try {
        thunk(ctx);

        return 1;
    }

    __except (info = GetExceptionInformation(), EXCEPTION_EXECUTE_HANDLER) {
        *code = GetExceptionCode();
        *addr = info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionAddress : nullptr;

        return 0;
    }
}

bool GuardedCall(void (*thunk)(void*), void* ctx, std::string& fault) {
    unsigned long code = 0;
    void* addr = nullptr;

    if (SehInvoke(thunk, ctx, &code, &addr))
        return true;

    if (code == EXCEPTION_STACK_OVERFLOW)
        _resetstkoflw();

    char buf[160];
    std::snprintf(buf, sizeof buf, "%s (0x%08lX) at %p", DescribeSeh(code), code, addr);
    fault = buf;

    return false;
}

#else

bool GuardedCall(void (*thunk)(void*), void* ctx, std::string& fault) {
    try {
        thunk(ctx);

        return true;
    } catch (const std::exception& e) {
        fault = std::string("unhandled C++ exception: ") + e.what();
    } catch (...) {
        fault = "unhandled C++ exception";
    }

    return false;
}

#endif

}  // namespace rmm
