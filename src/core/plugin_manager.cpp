#include "plugin_manager.h"

#include "core.h"
#include "platform.h"
#include "util.h"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>

namespace fs = std::filesystem;

namespace rmm {

const char* PluginStateName(PluginState s) {
    switch (s) {
        case PluginState::Loading:
            return "loading";
        case PluginState::Running:
            return "running";
        case PluginState::Paused:
            return "paused";
        case PluginState::Failed:
            return "FAILED";
    }

    return "?";
}

void PluginManager::SetDirs(const std::string& pluginsDir, const std::string& cacheDir) {
    pluginsDir_ = pluginsDir;
    cacheDir_ = cacheDir;
    plat::MakeDirs(pluginsDir_);
    // Clear stale shadow copies from a previous run (locked files are skipped).
    std::error_code ec;

    for (auto& e : fs::directory_iterator(cacheDir_, ec))
        fs::remove(e.path(), ec);

    plat::MakeDirs(cacheDir_);
}

std::string PluginManager::ResolvePath(const std::string& fileOrName) const {
    if (plat::FileExists(fileOrName))
        return fileOrName;

    if (plat::FileExists(fileOrName + plat::LibExtension()))
        return fileOrName + plat::LibExtension();  // "dir/name"

    std::string candidate = plat::JoinPath(pluginsDir_, fileOrName);

    if (plat::FileExists(candidate))
        return candidate;

    candidate += plat::LibExtension();

    if (plat::FileExists(candidate))
        return candidate;

    return {};
}

std::string PluginManager::ShadowCopy(const std::string& src, uint32_t id, std::string& err) {
    // Loading a copy keeps the original file unlocked (Windows) so it can be
    // replaced and hot-reloaded, and gives every load a unique mapping even if
    // a faulted previous instance is still mapped.
    std::string dst = plat::JoinPath(cacheDir_, plat::FileStem(src) + "." + std::to_string(id) + plat::LibExtension());
    std::error_code ec;
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);

    if (ec) {
        err = "could not create shadow copy: " + ec.message();

        return {};
    }

    return dst;
}

bool PluginManager::IsValid(const Plugin* p) const {
    if (!p)
        return false;

    for (auto& up : plugins_)
        if (up.get() == p)
            return true;

    return false;
}

Plugin* PluginManager::Find(const std::string& key) const {
    if (key.empty())
        return nullptr;

    if (key[0] == '#') {
        uint32_t id = static_cast<uint32_t>(std::atoi(key.c_str() + 1));

        for (auto& p : plugins_)
            if (p->id == id)
                return p.get();

        return nullptr;
    }

    for (auto& p : plugins_)
        if (IEquals(p->file, key) || IEquals(p->name, key))
            return p.get();

    for (auto& p : plugins_)
        if (IEquals(p->file, plat::FileStem(key)))
            return p.get();

    return nullptr;
}

Plugin* PluginManager::Load(const std::string& fileOrName, bool late, std::string& err) {
    Core& core = Core::Get();
    std::string path = ResolvePath(fileOrName);

    if (path.empty()) {
        err = "plugin file not found: " + fileOrName;

        return nullptr;
    }

    std::string stem = plat::FileStem(path);

    if (Plugin* existing = Find(stem)) {
        err = "already loaded as #" + std::to_string(existing->id) + " (" + PluginStateName(existing->state) + ")";

        return nullptr;
    }

    auto p = std::make_unique<Plugin>();
    p->id = nextId_++;
    p->path = path;
    p->file = stem;
    p->late = late;

    p->loadedPath = ShadowCopy(path, p->id, err);

    if (p->loadedPath.empty())
        return nullptr;

    p->lib = plat::LoadLib(p->loadedPath, err);

    if (!p->lib)
        return nullptr;

    auto fail = [&](const std::string& why) -> Plugin* {
        err = why;
        plat::FreeLib(p->lib);
        std::error_code ec;
        fs::remove(p->loadedPath, ec);

        return nullptr;
    };

    p->fnQuery = reinterpret_cast<rmm_query_fn>(plat::GetSym(p->lib, RMM_SYM_QUERY));
    p->fnLoad = reinterpret_cast<rmm_load_fn>(plat::GetSym(p->lib, RMM_SYM_LOAD));
    p->fnAllLoaded = reinterpret_cast<rmm_all_loaded_fn>(plat::GetSym(p->lib, RMM_SYM_ALL_LOADED));
    p->fnPause = reinterpret_cast<rmm_pause_fn>(plat::GetSym(p->lib, RMM_SYM_PAUSE));
    p->fnUnload = reinterpret_cast<rmm_unload_fn>(plat::GetSym(p->lib, RMM_SYM_UNLOAD));

    if (!p->fnQuery || !p->fnLoad)
        return fail("not an RMM plugin (missing RMM_Query/RMM_Load exports)");

    // Validate the RMM_Query info before running any plugin logic.
    const rmm_plugin_info* info = nullptr;
    std::string fault;

    if (!Guard([&] { info = p->fnQuery(); }, fault))
        return fail("crashed in RMM_Query: " + fault);

    if (!info)
        return fail("RMM_Query returned NULL");

    if (info->struct_size < offsetof(rmm_plugin_info, flags) + sizeof(info->flags))
        return fail("plugin info struct too small (built against an incompatible SDK?)");

    if (info->api_major != RMM_API_VERSION_MAJOR)
        return fail(Format("API major version mismatch: plugin %u, RMM %u", info->api_major, RMM_API_VERSION_MAJOR));

    if (info->api_minor > RMM_API_VERSION_MINOR)
        return fail(Format("plugin needs API %u.%u but RMM provides %u.%u - update RMM", info->api_major,
                           info->api_minor, RMM_API_VERSION_MAJOR, RMM_API_VERSION_MINOR));

    if (!(info->games & static_cast<uint32_t>(core.Game().GameId())))
        return fail(Format("plugin does not support %s", core.Game().GameName()));

    auto str = [](const char* s) { return std::string(s ? s : ""); };
    p->name = str(info->name).empty() ? stem : str(info->name);
    p->version = str(info->version);
    p->author = str(info->author);
    p->description = str(info->description);
    p->url = str(info->url);
    p->logTag = str(info->log_tag).empty() ? stem : str(info->log_tag);
    p->games = info->games;
    p->flags = info->flags;
    p->apiMajor = info->api_major;
    p->apiMinor = info->api_minor;
    p->state = PluginState::Loading;
    p->loadTime = core.Time();

    Plugin* raw = p.get();
    plugins_.push_back(std::move(p));  // handle must be valid during RMM_Load

    char errbuf[512] = {0};
    rmm_bool ok = RMM_FALSE;
    bool survived = core.CallPlugin(raw, "RMM_Load", [&] {
        ok = raw->fnLoad(core.Api(), raw, late ? RMM_TRUE : RMM_FALSE, errbuf, sizeof errbuf);
    });

    if (!survived || !ok) {
        errbuf[sizeof errbuf - 1] = '\0';
        err = survived ? (errbuf[0] ? errbuf : "RMM_Load returned false") : ("crashed in RMM_Load: " + raw->error);
        core.ReleaseResources(raw);
        Remove(raw, /*keepMapped=*/!survived);

        return nullptr;
    }

    raw->state = PluginState::Running;
    core.Log().Info("Loaded plugin #%u %s %s by %s", raw->id, raw->name.c_str(), raw->version.c_str(),
                    raw->author.c_str());

    // AutoExecConfig equivalent: rmm/configs/plugins/<file>.cfg
    std::string cfg = core.BuildPath(RMM_PATH_CONFIGS, "plugins/" + raw->file + ".cfg");

    if (plat::FileExists(cfg))
        core.ExecConfig(cfg);

    rmm_ev_plugin ev{raw, raw->name.c_str()};
    core.Dispatch(RMM_EV_PLUGIN_LOADED, &ev);

    if (late && raw->fnAllLoaded && raw->state == PluginState::Running)
        core.CallPlugin(raw, "RMM_AllLoaded", [&] { raw->fnAllLoaded(); });

    return raw;
}

void PluginManager::Remove(Plugin* p, bool keepMapped) {
    auto it = std::find_if(plugins_.begin(), plugins_.end(), [&](auto& up) { return up.get() == p; });

    if (it == plugins_.end())
        return;

    std::unique_ptr<Plugin> owned = std::move(*it);
    plugins_.erase(it);

    if (keepMapped) {
        graveyard_.push_back(std::move(owned));
    } else {
        plat::FreeLib(owned->lib);
        std::error_code ec;
        fs::remove(owned->loadedPath, ec);
    }
}

bool PluginManager::Unload(Plugin* p, bool force, std::string& err) {
    Core& core = Core::Get();

    if (!IsValid(p)) {
        err = "invalid plugin";

        return false;
    }

    if (!force && (p->flags & RMM_PLUGIN_FLAG_NO_UNLOAD) && p->state != PluginState::Failed) {
        err = p->name + " cannot be unloaded at runtime";

        return false;
    }

    std::string name = p->name;
    rmm_ev_plugin ev{p, name.c_str()};
    core.Dispatch(RMM_EV_PLUGIN_UNLOADED, &ev);

    bool faulted = p->state == PluginState::Failed;

    if (!faulted && p->fnUnload) {
        p->state = PluginState::Running;  // allow the final call even if paused

        if (!core.CallPlugin(p, "RMM_Unload", [&] { p->fnUnload(); }))
            faulted = true;
    }

    core.ReleaseResources(p);
    Remove(p, faulted);
    core.Log().Info("Unloaded plugin %s", name.c_str());

    return true;
}

bool PluginManager::Reload(Plugin* p, std::string& err) {
    if (!IsValid(p)) {
        err = "invalid plugin";

        return false;
    }

    std::string path = p->path;

    if (!Unload(p, false, err))
        return false;

    return Load(path, true, err) != nullptr;
}

bool PluginManager::SetPaused(Plugin* p, bool paused, std::string& err) {
    if (!IsValid(p)) {
        err = "invalid plugin";

        return false;
    }

    if (p->state == PluginState::Failed) {
        err = "plugin has failed; reload it";

        return false;
    }

    if (paused && (p->flags & RMM_PLUGIN_FLAG_NO_PAUSE)) {
        err = p->name + " cannot be paused";

        return false;
    }

    PluginState want = paused ? PluginState::Paused : PluginState::Running;

    if (p->state == want)
        return true;

    p->state = PluginState::Running;  // so the notification itself is delivered

    if (p->fnPause)
        Core::Get().CallPlugin(p, "RMM_Pause", [&] { p->fnPause(paused ? RMM_TRUE : RMM_FALSE); });

    if (p->state != PluginState::Failed)
        p->state = want;

    return true;
}

void PluginManager::MarkFailed(Plugin* p, const std::string& why) {
    if (!p || p->state == PluginState::Failed)
        return;

    p->state = PluginState::Failed;
    p->error = why;
    pending_.push_back(Pending{Pending::kRelease, {}, p, why});
}

void PluginManager::RequestLoad(const std::string& file) {
    pending_.push_back(Pending{Pending::kLoad, file, nullptr, {}});
}

void PluginManager::RequestUnload(Plugin* p, const std::string& reason) {
    pending_.push_back(Pending{Pending::kUnload, {}, p, reason});
}

void PluginManager::RequestReload(Plugin* p) { pending_.push_back(Pending{Pending::kReload, {}, p, {}}); }

void PluginManager::ProcessPending() {
    Core& core = Core::Get();

    if (core.CallDepth() > 0 || core.Events().Depth() > 0)
        return;

    while (!pending_.empty()) {
        Pending job = pending_.front();
        pending_.erase(pending_.begin());
        std::string err;

        switch (job.kind) {
            case Pending::kLoad:
                if (!Load(job.target, true, err))
                    core.Log().Error("Load of '%s' failed: %s", job.target.c_str(), err.c_str());

                break;
            case Pending::kUnload:
                if (IsValid(job.p)) {
                    if (!job.reason.empty())
                        core.Log().Info("Unloading %s: %s", job.p->name.c_str(), job.reason.c_str());

                    if (!Unload(job.p, false, err))
                        core.Log().Error("Unload failed: %s", err.c_str());
                }

                break;
            case Pending::kReload:
                if (IsValid(job.p) && !Reload(job.p, err))
                    core.Log().Error("Reload failed: %s", err.c_str());

                break;
            case Pending::kRelease:
                // Faulted plugin: drop registrations but keep it listed (and mapped).
                if (IsValid(job.p))
                    core.ReleaseResources(job.p);

                break;
        }
    }
}

void PluginManager::LoadAll() {
    Core& core = Core::Get();
    std::vector<Plugin*> batch;

    for (auto& path : plat::ListFiles(pluginsDir_, plat::LibExtension())) {
        std::string err;

        if (Plugin* p = Load(path, false, err))
            batch.push_back(p);
        else
            core.Log().Error("Failed to load %s: %s", plat::FileStem(path).c_str(), err.c_str());
    }

    for (Plugin* p : batch)
        if (IsValid(p) && p->state == PluginState::Running && p->fnAllLoaded)
            core.CallPlugin(p, "RMM_AllLoaded", [&] { p->fnAllLoaded(); });

    ProcessPending();
}

void PluginManager::UnloadAll() {
    shuttingDown_ = true;

    while (!plugins_.empty()) {
        std::string err;
        Unload(plugins_.back().get(), true, err);
    }

    pending_.clear();
}

}  // namespace rmm
