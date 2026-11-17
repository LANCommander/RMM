// ABI differences between the two Refractor 1 server builds, so the native
// backend (r1_native.cpp) is one source for both:
//
//                      Linux (GCC 3.2.3, i386)           Windows (VC++ 7.0, x86)
//   member functions   cdecl, `this` first on the stack  __thiscall: `this` in ECX
//   std::string        COW, one pointer (gcc3_string)     28 bytes, SSO (msvc7_string)
//   string by value    pointer to a caller-owned copy     the object itself; callee destroys it
//   std::map node      {color,parent,left,right,key,val}  {left,parent,right,key,val,color,isnil}
//   std::map / list    the member is the header pointer   {allocator, header pointer, size}
//   vptr               vtable symbol + 8                  vtable address
//
// Engine member functions are declared as plain function pointers with an
// explicit `self` first parameter: `R (R1_THIS*)(void* self, args...)`.
// Detours of member functions are written as
//     R R1_HOOK Hook_X(void* self R1_EDX, args...)
// which on Windows is __fastcall(self = ECX, unused EDX, stack args...): the
// same registers, stack layout and callee cleanup as __thiscall.
#pragma once

#include <cstdint>

#ifdef _WIN32
#    include "msvc7_string.h"
#    define R1_THIS    __thiscall
#    define R1_HOOK    __fastcall
#    define R1_EDX     , void* /*edx*/
#    define R1_EDX_ARG , nullptr
#else
#    include "gcc3_string.h"
#    define R1_THIS
#    define R1_HOOK
#    define R1_EDX
#    define R1_EDX_ARG
#endif

namespace rmm {

#ifdef _WIN32
namespace es = msvc7;  // the engine's std::string
#else
namespace es = gcc3;
#endif

namespace r1abi {

/// std::map<K, V> with a 4-byte key: where things are.
struct MapLayout {
    uint32_t header;  // offset of the header-node pointer inside the map object
    uint32_t root;    // header -> root
    uint32_t left, right, key, value;
};
#ifdef _WIN32
constexpr MapLayout kMap{4, 4, 0, 8, 0xc, 0x10};
/// std::list<T>: offset of the sentinel-node pointer inside the list object.
constexpr uint32_t kListHeader = 4;
#else
constexpr MapLayout kMap{0, 4, 8, 0xc, 0x10, 0x14};
constexpr uint32_t kListHeader = 0;
#endif

}  // namespace r1abi
}  // namespace rmm
