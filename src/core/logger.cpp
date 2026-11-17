#include "logger.h"

#include "platform.h"
#include "util.h"

#include <cstdarg>

namespace rmm {

static const char* LevelName(int l) {
    switch (l) {
        case 0:
            return "DEBUG";
        case 1:
            return "INFO";
        case 2:
            return "WARN";
        default:
            return "ERROR";
    }
}

Logger::~Logger() { Close(); }

void Logger::Open(const std::string& logsDir) {
    Close();
    dir_ = logsDir;
    date_ = plat::DateString();
    plat::MakeDirs(dir_);
    file_ = std::fopen(plat::JoinPath(dir_, "rmm_" + date_ + ".log").c_str(), "a");
}

void Logger::Close() {
    if (file_) {
        std::fclose(file_);
        file_ = nullptr;
    }
}

void Logger::Write(int level, const std::string& tag, const std::string& msg) {
    if (level < minLevel_)
        return;

    // Roll over to a new file each day.
    if (!dir_.empty() && plat::DateString() != date_)
        Open(dir_);

    std::string line = "[" + plat::TimestampString() + "] [" + LevelName(level) + "] [" + tag + "] " + msg;

    if (file_) {
        std::fputs(line.c_str(), file_);
        std::fputc('\n', file_);
        std::fflush(file_);
    }

    std::string consoleLine = "[RMM] [" + tag + "] " + msg;

    if (sink_)
        sink_(consoleLine);
    else
        plat::DebugOut(consoleLine);
}

#define RMM_LOGGER_FN(Name, Lvl)                                                                                       \
    void Logger::Name(const char* fmt, ...) {                                                                          \
        va_list ap;                                                                                                    \
        va_start(ap, fmt);                                                                                             \
        std::string m = VFormat(fmt, ap);                                                                              \
        va_end(ap);                                                                                                    \
        Write(Lvl, "core", m);                                                                                         \
    }
RMM_LOGGER_FN(Debug, 0)
RMM_LOGGER_FN(Info, 1)
RMM_LOGGER_FN(Warn, 2)
RMM_LOGGER_FN(Error, 3)
#undef RMM_LOGGER_FN

}  // namespace rmm
