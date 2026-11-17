// OS abstraction: dynamic libraries, paths, time.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rmm::plat {

void* LoadLib(const std::string& path, std::string& error);
void* GetSym(void* lib, const char* name);
void FreeLib(void* lib);
const char* LibExtension();  // ".dll" / ".so"

std::string ExecutablePath();  // full path of the host process image
std::string ModuleDir();       // directory containing the RMM core module
std::string CurrentDir();
std::vector<std::string> ListFiles(const std::string& dir, const std::string& ext);
bool MakeDirs(const std::string& dir);
bool FileExists(const std::string& path);
std::string JoinPath(const std::string& a, const std::string& b);
std::string FileStem(const std::string& path);
std::string GetEnv(const char* name);

double MonotonicSeconds();
int64_t UnixTime();
std::string TimestampString();  // "2026-11-09 13:37:00"
std::string DateString();       // "20261109"

void DebugOut(const std::string& line);  // OutputDebugString / stderr

}  // namespace rmm::plat
