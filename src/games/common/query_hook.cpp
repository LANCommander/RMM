// winsock2.h has to come before anything that pulls in windows.h.
#ifdef _WIN32
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    include <winsock2.h>
#else
#    include <dlfcn.h>
#    include <sys/socket.h>
#    include <sys/types.h>
#endif

#include "query_hook.h"

#include "../../core/core.h"
#include "hooks.h"

#include <atomic>

namespace rmm::queryhook {
namespace {

std::atomic<bool> g_active{false};

// GameSpy replies are well under this; a longer packet is not one.
constexpr int kMaxReply = 2048;

int Rewrite(const void* buf, size_t len, char* out) {
    if (!g_active.load(std::memory_order_relaxed) || len >= static_cast<size_t>(kMaxReply))
        return 0;

    return Core::Get().Query().Rewrite(static_cast<const char*>(buf), static_cast<int>(len), out, kMaxReply);
}

#ifdef _WIN32
using SendToFn = int(WINAPI*)(SOCKET, const char*, int, int, const sockaddr*, int);
SendToFn g_real = nullptr;

int WINAPI HookSendTo(SOCKET s, const char* buf, int len, int flags, const sockaddr* to, int tolen) {
    char reply[kMaxReply];
    int n = len > 0 ? Rewrite(buf, static_cast<size_t>(len), reply) : 0;

    if (n > 0) {
        int sent = g_real(s, reply, n, flags, to, tolen);

        return sent == SOCKET_ERROR ? sent : len;  // the caller sent len bytes
    }

    return g_real(s, buf, len, flags, to, tolen);
}
#endif

}  // namespace

bool Install(std::string& err) {
#ifdef _WIN32
    if (!g_real) {
        void* orig = nullptr;
        // BF1942_w32ded.exe imports WSOCK32 by ordinal (sendto = 20).
        static const char* const kImports[][2] = {
            {"WSOCK32.dll", "#20"}, {"WSOCK32.dll", "sendto"}, {"WS2_32.dll", "#20"}, {"WS2_32.dll", "sendto"}};

        for (auto& imp : kImports)
            if (hooks::PatchImport(nullptr, imp[0], imp[1], reinterpret_cast<void*>(&HookSendTo), &orig))
                break;

        if (!orig) {
            err = "the server imports no sendto";

            return false;
        }

        g_real = reinterpret_cast<SendToFn>(orig);
    }
#else
    (void)err;
#endif
    g_active = true;

    return true;
}

void Remove() { g_active = false; }

}  // namespace rmm::queryhook

#ifndef _WIN32
// The server's UDP sends (librmm.so is preloaded, so this one is found first).
// force_align_arg_pointer: the old GCC that built the servers keeps only
// 4-byte stack alignment.
extern "C" __attribute__((visibility("default"), force_align_arg_pointer)) ssize_t sendto(int fd, const void* buf,
                                                                                          size_t len, int flags,
                                                                                          const struct sockaddr* to,
                                                                                          socklen_t tolen) {
    using SendToFn = ssize_t (*)(int, const void*, size_t, int, const struct sockaddr*, socklen_t);
    static SendToFn real = reinterpret_cast<SendToFn>(dlsym(RTLD_NEXT, "sendto"));
    char reply[rmm::queryhook::kMaxReply];
    int n = rmm::queryhook::Rewrite(buf, len, reply);

    if (n > 0) {
        ssize_t sent = real(fd, reply, static_cast<size_t>(n), flags, to, tolen);

        return sent < 0 ? sent : static_cast<ssize_t>(len);
    }

    return real(fd, buf, len, flags, to, tolen);
}
#endif
