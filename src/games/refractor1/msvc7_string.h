// Interop with the std::string of Visual C++ 7.0, as used by the BF1942
// Windows server (BF1942_w32ded.exe).
//
// The server does not inline its string code: std::basic_string<char> is
// exported from bfcprt.dll (the server's C++ runtime, a renamed msvcp70), so
// RMM creates, assigns and destroys engine strings through those exports and
// the engine's own allocator. Layout (from bfcprt's c_str/size):
//   +0x00 allocator (empty, padded)
//   +0x04 union { char buf[16]; char* ptr; }   - ptr when capacity >= 16
//   +0x14 size_t size
//   +0x18 size_t capacity
// Same API as gcc3_string.h, so the Refractor 1 native code is shared.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace rmm::msvc7 {

/// Layout-compatible with VC7 std::string (28 bytes).
struct String {
    uint32_t alloc;

    union {
        char buf[16];
        char* ptr;
    } bx;

    uint32_t size;
    uint32_t capacity;
};

static_assert(sizeof(String) == 0x1c, "VC7 std::string is 28 bytes");

/// Resolve the bfcprt.dll entry points. Returns false with a reason if the
/// module or an export is missing.
bool Init(std::string& error);
bool Ready();

/// Construct in place from a C string.
void Construct(String* s, const char* text);
/// Destroy (frees the heap buffer through the engine's allocator).
void Destroy(String* s);
/// s = text
void Assign(String* s, const std::string& text);
/// Copy out (safe to call on any engine string).
std::string Read(const String* s);
size_t Length(const String* s);

/// No RTTI casts are needed on Windows (BF1942 walks interfaces); kept for API parity.
using DynamicCastFn = void* (*)(const void* obj, const void* srcType, const void* dstType, ptrdiff_t hint);

inline DynamicCastFn DynamicCast() { return nullptr; }

/// RAII holder for an engine string used as a call argument.
class Scoped {
public:
    explicit Scoped(const char* text = "") { Construct(&s_, text); }

    ~Scoped() { Destroy(&s_); }

    Scoped(const Scoped&) = delete;
    Scoped& operator=(const Scoped&) = delete;

    String* get() { return &s_; }

    std::string str() const { return Read(&s_); }

private:
    String s_;
};

}  // namespace rmm::msvc7
