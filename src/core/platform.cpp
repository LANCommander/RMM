#include "platform.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>

#ifdef _WIN32
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    include <windows.h>
#else
#    include <dlfcn.h>
#    include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace rmm::plat {

void* LoadLib(const std::string& path, std::string& error) {
#ifdef _WIN32
    // Make the plugin's own directory searchable for its dependencies.
    HMODULE h = LoadLibraryExA(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);

    if (!h) {
        DWORD code = GetLastError();
        char msg[256] = {0};
        FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0, msg, sizeof msg,
                       nullptr);
        error = "LoadLibrary failed (" + std::to_string(code) + "): " + msg;

        while (!error.empty() && (error.back() == '\n' || error.back() == '\r'))
            error.pop_back();
    }

    return h;
#else
    void* h = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);

    if (!h)
        error = dlerror();

    return h;
#endif
}

void* GetSym(void* lib, const char* name) {
#ifdef _WIN32
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(lib), name));
#else
    return dlsym(lib, name);
#endif
}

void FreeLib(void* lib) {
    if (!lib)
        return;
#ifdef _WIN32
    FreeLibrary(static_cast<HMODULE>(lib));
#else
    dlclose(lib);
#endif
}

const char* LibExtension() {
#ifdef _WIN32
    return ".dll";
#else
    return ".so";
#endif
}

std::string ExecutablePath() {
#ifdef _WIN32
    char buf[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, buf, MAX_PATH);

    return buf;
#else
    std::error_code ec;
    auto p = fs::read_symlink("/proc/self/exe", ec);

    return ec ? std::string() : p.string();
#endif
}

static void ModuleAnchor() {}

std::string ModuleDir() {
#ifdef _WIN32
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&ModuleAnchor), &self);
    char buf[MAX_PATH] = {0};
    GetModuleFileNameA(self, buf, MAX_PATH);

    return fs::path(buf).parent_path().string();
#else
    Dl_info info{};

    if (dladdr(reinterpret_cast<void*>(&ModuleAnchor), &info) && info.dli_fname)
        return fs::absolute(fs::path(info.dli_fname)).parent_path().string();

    return CurrentDir();
#endif
}

std::string CurrentDir() {
    std::error_code ec;

    return fs::current_path(ec).string();
}

std::vector<std::string> ListFiles(const std::string& dir, const std::string& ext) {
    std::vector<std::string> out;
    std::error_code ec;

    for (auto& e : fs::directory_iterator(dir, ec)) {
        if (!e.is_regular_file(ec))
            continue;

        if (ext.empty() || e.path().extension().string() == ext)
            out.push_back(e.path().string());
    }

    std::sort(out.begin(), out.end());

    return out;
}

bool MakeDirs(const std::string& dir) {
    std::error_code ec;
    fs::create_directories(dir, ec);

    return fs::is_directory(dir, ec);
}

bool FileExists(const std::string& path) {
    std::error_code ec;

    return fs::exists(path, ec);
}

std::string JoinPath(const std::string& a, const std::string& b) {
    if (b.empty())
        return a;

    return (fs::path(a) / fs::path(b)).string();
}

std::string FileStem(const std::string& path) { return fs::path(path).stem().string(); }

std::string GetEnv(const char* name) {
    const char* v = std::getenv(name);

    return v ? v : "";
}

double MonotonicSeconds() {
    using namespace std::chrono;
    static const auto start = steady_clock::now();

    return duration<double>(steady_clock::now() - start).count();
}

int64_t UnixTime() { return static_cast<int64_t>(std::time(nullptr)); }

static std::tm LocalTm() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    return tm;
}

std::string TimestampString() {
    std::tm tm = LocalTm();
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &tm);

    return buf;
}

std::string DateString() {
    std::tm tm = LocalTm();
    char buf[16];
    std::strftime(buf, sizeof buf, "%Y%m%d", &tm);

    return buf;
}

void DebugOut(const std::string& line) {
#ifdef _WIN32
    OutputDebugStringA((line + "\n").c_str());
#endif
    std::fprintf(stderr, "%s\n", line.c_str());
}

}  // namespace rmm::plat
