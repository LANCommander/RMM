// Interop with the std::string of GCC 3.x's libstdc++.so.5, as used by the
// BF1942 Linux server (built with GCC 3.2.3).
//
// That std::string is the reference-counted copy-on-write implementation: the
// object is a single pointer to the character data, preceded in memory by a
// _Rep header { size_t length; size_t capacity; int refcount; }. RMM is built
// with a modern compiler whose std::string is unrelated, so engine strings
// must be created, assigned and destroyed with the *engine's* libstdc++
// functions, never ours. Reading is safe to do directly.
#pragma once

#include <cstddef>
#include <string>

namespace rmm::gcc3 {

/// Layout-compatible with GCC 3 std::string (one pointer).
struct String {
    char* data;
};

/// Resolve the libstdc++.so.5 entry points. Returns false with a reason if
/// they cannot be found (e.g. the static server build without symbols).
bool Init(std::string& error);
bool Ready();

/// Construct in place (placement-new equivalent) from a C string.
void Construct(String* s, const char* text);
/// Destroy (drops the reference / frees the rep).
void Destroy(String* s);
/// s = text
void Assign(String* s, const std::string& text);
/// Copy out (safe to call on any engine string).
std::string Read(const String* s);
size_t Length(const String* s);

/// The engine's own libstdc++ __dynamic_cast (Itanium ABI):
///   void* __dynamic_cast(const void* obj, const type_info* src, const type_info* dst, ptrdiff_t hint)
/// Used with the engine's typeinfo objects to repeat its RTTI casts (BFV
/// finds IPlayerControlObject this way). nullptr if unavailable.
using DynamicCastFn = void* (*)(const void* obj, const void* srcType, const void* dstType, ptrdiff_t hint);
DynamicCastFn DynamicCast();

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

}  // namespace rmm::gcc3
