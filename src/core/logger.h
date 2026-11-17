#pragma once

#include <cstdio>
#include <functional>
#include <string>

namespace rmm {

class Logger {
public:
    ~Logger();
    void Open(const std::string& logsDir);
    void Close();

    void SetMinLevel(int lvl) { minLevel_ = lvl; }

    /// Extra sink, e.g. the engine console (set by the adapter).
    void SetConsoleSink(std::function<void(const std::string&)> sink) { sink_ = std::move(sink); }

    void Write(int level, const std::string& tag, const std::string& msg);

    void Debug(const char* fmt, ...);
    void Info(const char* fmt, ...);
    void Warn(const char* fmt, ...);
    void Error(const char* fmt, ...);

private:
    std::FILE* file_ = nullptr;
    std::string dir_, date_;
    int minLevel_ = 1;
    std::function<void(const std::string&)> sink_;
};

}  // namespace rmm
