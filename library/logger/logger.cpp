#include "logger.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace {
constexpr int MILLIS_PER_SECOND = 1000;
} // namespace

Logger::Level Logger::globalLevel = Logger::INFO;
std::unordered_map<std::string, Logger::Level> Logger::tagLevels;
std::mutex Logger::logMutex;

void Logger::setGlobalLevel(Level level) {
    std::lock_guard<std::mutex> lock(logMutex);
    globalLevel = level;
}

void Logger::setLevel(const std::string& tag, Level level) {
    std::lock_guard<std::mutex> lock(logMutex);
    tagLevels[tag] = level;
}

Logger::Level Logger::getLevel(const std::string& tag) {
    auto found = tagLevels.find(tag);
    if (found != tagLevels.end()) {
        return found->second;
    }

    return globalLevel;
}

void Logger::log(Level level, const char* tag, const char* fmt, ...) {
    {
        std::lock_guard<std::mutex> lock(logMutex);
        if (level > globalLevel) {
            return;
        }
        auto found = tagLevels.find(tag);
        Level tagLevel = (found != tagLevels.end()) ? found->second : globalLevel;

        if (level > tagLevel) {
            return;
        }
    }
    static const char* levelStr[] = {"", "E", "W", "I", "D", "V"};
    va_list args;
    va_start(args, fmt);
    {
        std::lock_guard<std::mutex> lock(logMutex);
        printf("%s%c (%s) %s: ", levelColor(level), levelStr[level][0], getTimestamp().c_str(),
               tag);
        vprintf(fmt, args);
        printf("%s\n", COLOR_RESET);
    }
    va_end(args);
}

std::string Logger::getTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) %
                  std::chrono::milliseconds(MILLIS_PER_SECOND);

    auto timeT = std::chrono::system_clock::to_time_t(now);

    std::tm timeInfo;
    localtime_r(&timeT, &timeInfo);

    std::ostringstream oss;
    oss << std::put_time(&timeInfo, "%Y-%m-%d %H:%M:%S") << '.' << std::setw(3) << std::setfill('0')
        << millis.count();

    return oss.str();
}

const char* Logger::levelColor(Level level) {
    switch (level) {
    case ERROR:
        return COLOR_RED;

    case WARN:
        return COLOR_YELLOW;

    case INFO:
        return COLOR_GREEN;

    case DEBUG:
        return COLOR_BLUE;

    case VERBOSE:
        return COLOR_WHITE;

    default:
        return COLOR_RESET;
    }
}
