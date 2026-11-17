// Crash isolation for calls into plugin code.
#pragma once

#include <string>
#include <type_traits>
#include <utility>

namespace rmm {

/// Invoke thunk(ctx). Returns false if it faulted, with a description in
/// `fault`. On MSVC this catches structured exceptions (access violations,
/// stack overflow, divide by zero, unhandled C++ exceptions). Elsewhere only
/// C++ exceptions are caught; a hard crash on Linux still takes the server
/// down (see docs/ARCHITECTURE.md, "Safety").
bool GuardedCall(void (*thunk)(void*), void* ctx, std::string& fault);

template <class F> bool Guard(F&& f, std::string& fault) {
    using Fn = std::remove_reference_t<F>;

    return GuardedCall([](void* p) { (*static_cast<Fn*>(p))(); }, &f, fault);
}

}  // namespace rmm
