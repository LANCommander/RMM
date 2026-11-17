// Windows (rmm_launcher.exe): load rmm.dll into a suspended process and run
// RMM_Bootstrap in it. The winmm.dll proxy (src/loader) needs no injection.
#pragma once

#ifdef _WIN32

#    include <string>

namespace rmm::inject {

/// LoadLibraryA(@p dll) in @p process, then call its RMM_Bootstrap there.
/// The process may be suspended (CREATE_SUSPENDED): the remote threads run the
/// loader. Returns false with a reason; the caller decides whether to resume
/// the process without RMM.
bool LoadInto(void* process, const std::string& dll, std::string& err);

/// Full path of the module that contains this code (rmm.dll inside a server).
std::string ThisModulePath();

}  // namespace rmm::inject

#endif
