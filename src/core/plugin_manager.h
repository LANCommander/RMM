#pragma once

#include "plugin.h"

#include <memory>
#include <string>
#include <vector>

namespace rmm {

class PluginManager {
public:
    void SetDirs(const std::string& pluginsDir, const std::string& cacheDir);

    /// Load every plugin in plugins/ (alphabetical), then call AllLoaded.
    void LoadAll();
    /// Unload everything (server shutdown). Ignores NO_UNLOAD.
    void UnloadAll();

    /// Immediate operations - only call when no plugin code is on the stack.
    Plugin* Load(const std::string& fileOrName, bool late, std::string& err);
    bool Unload(Plugin* p, bool force, std::string& err);
    bool Reload(Plugin* p, std::string& err);
    bool SetPaused(Plugin* p, bool paused, std::string& err);

    /// Deferred operations - safe from inside callbacks/commands. Executed by
    /// ProcessPending() at the end of the frame.
    void RequestLoad(const std::string& file);
    void RequestUnload(Plugin* p, const std::string& reason);
    void RequestReload(Plugin* p);

    /// A plugin crashed: stop calling it now, release its registrations soon.
    void MarkFailed(Plugin* p, const std::string& why);

    void ProcessPending();

    Plugin* Find(const std::string& nameIdOrFile) const;
    bool IsValid(const Plugin* p) const;

    const std::vector<std::unique_ptr<Plugin>>& All() const { return plugins_; }

private:
    std::string ResolvePath(const std::string& fileOrName) const;
    std::string ShadowCopy(const std::string& src, uint32_t id, std::string& err);
    void Remove(Plugin* p, bool keepMapped);

    struct Pending {
        enum Kind { kLoad, kUnload, kReload, kRelease } kind;

        std::string target;
        Plugin* p;
        std::string reason;
    };

    std::string pluginsDir_, cacheDir_;
    std::vector<std::unique_ptr<Plugin>> plugins_;
    std::vector<std::unique_ptr<Plugin>> graveyard_;  // faulted plugins: never unmapped
    std::vector<Pending> pending_;
    uint32_t nextId_ = 1;
    bool shuttingDown_ = false;
};

}  // namespace rmm
