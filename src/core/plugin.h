// Internal representation of a loaded plugin. The public opaque handle
// rmm_plugin_t is a pointer to this struct.
#pragma once

#include <rmm/rmm_plugin.h>

#include <string>

struct rmm_plugin_s {
    enum class State { Loading, Running, Paused, Failed };

    uint32_t id = 0;
    std::string path;        // original path in plugins/
    std::string loadedPath;  // shadow copy actually mapped
    std::string file;        // file stem, e.g. "basecommands"
    void* lib = nullptr;

    std::string name, version, author, description, url, logTag;
    uint32_t games = 0, flags = 0;
    uint16_t apiMajor = 0, apiMinor = 0;

    rmm_query_fn fnQuery = nullptr;
    rmm_load_fn fnLoad = nullptr;
    rmm_all_loaded_fn fnAllLoaded = nullptr;
    rmm_pause_fn fnPause = nullptr;
    rmm_unload_fn fnUnload = nullptr;

    State state = State::Loading;
    std::string error;  // last load error or fault description
    bool late = false;
    double loadTime = 0;

    /// May RMM call into this plugin right now?
    bool Callable() const { return state == State::Running || state == State::Loading; }
};

namespace rmm {
using Plugin = rmm_plugin_s;
using PluginState = rmm_plugin_s::State;
const char* PluginStateName(PluginState s);
}  // namespace rmm
