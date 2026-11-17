#pragma once

#include "plugin.h"

#include <rmm/rmm_vars.h>

#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace rmm {

struct Var {
    rmm_handle_t h = 0;
    Plugin* owner = nullptr;  // nullptr = core
    std::string name;         // as registered
    std::string def, desc, value;
    uint32_t flags = 0;
    bool hasMin = false, hasMax = false;
    double min = 0, max = 0;
};

class VarRegistry {
public:
    void SetArchivePath(const std::string& path) { archivePath_ = path; }

    void LoadArchive();
    void SaveArchive();

    rmm_handle_t Create(Plugin* owner, const std::string& name, const std::string& def, const std::string& desc,
                        uint32_t flags, std::string* err = nullptr);
    Var* Get(rmm_handle_t h);
    Var* Find(const std::string& name);
    std::vector<const Var*> List() const;

    /// Validate and apply. `by` is the plugin making the change (nullptr =
    /// console/core). Fires change hooks and RMM_EV_VAR_CHANGED.
    bool Set(Plugin* by, Var* v, const std::string& value, std::string* err = nullptr);
    /// Remember a value for a var that does not exist yet (config/archive).
    void SetPending(const std::string& name, const std::string& value);

    rmm_handle_t HookChange(Plugin* owner, rmm_handle_t var, rmm_var_change_cb cb, void* user);
    bool UnhookChange(Plugin* owner, rmm_handle_t hook);

    void RemoveAll(Plugin* owner);

    /// Engine variable watches: polled, changes fire RMM_EV_VAR_CHANGED.
    void Watch(Plugin* owner, const std::string& engineVar);
    void PollWatches(double now);

private:
    struct ChangeHook {
        rmm_handle_t h;
        rmm_handle_t var;
        Plugin* owner;
        rmm_var_change_cb cb;
        void* user;
        bool dead;
    };

    struct WatchRec {
        std::string name;
        std::string last;
        bool primed;
        std::vector<Plugin*> owners;
    };

    std::map<rmm_handle_t, std::unique_ptr<Var>> vars_;
    std::unordered_map<std::string, rmm_handle_t> byName_;  // lower-case
    std::map<std::string, std::string> pending_;            // lower-case name -> value
    std::vector<ChangeHook> hooks_;
    std::vector<WatchRec> watches_;
    std::string archivePath_;
    rmm_handle_t nextHandle_ = 1;
    int firing_ = 0;
    bool dirty_ = false;
    double nextPoll_ = 0;
};

}  // namespace rmm
