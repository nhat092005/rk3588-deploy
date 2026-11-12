#include "logger.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <sstream>

Logger::Level Logger::m_globalLevel = Logger::INFO;
std::unordered_map<std::string, Logger::Level> Logger::m_tagLevels;
std::mutex Logger::m_mutex;

void Logger::setGlobalLevel(Level level) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_globalLevel = level;
}

void Logger::setLevel(const std::string& tag, Level level) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_tagLevels[tag] = level;
}

Logger::Level Logger::getLevel(const std::string& tag) {
    auto it = m_tagLevels.find(tag);
    if (it != m_tagLevels.end())
        return it->second;

    return m_globalLevel;
}

void Logger::log(Level level, const char* tag, const char* fmt, ...) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (level > m_globalLevel)
            return;
        auto it = m_tagLevels.find(tag);
        Level tagLevel = (it != m_tagLevels.end()) ? it->second : m_globalLevel;

        if (level > tagLevel)
            return;
    }
    static const char* levelStr[] = {"", "E", "W", "I", "D", "V"};
    va_list args;
    va_start(args, fmt);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        printf("%s%c (%s) %s: ", levelColor(level), levelStr[level][0], getTimestamp().c_str(),
               tag);
        vprintf(fmt, args);
        printf("%s\n", COLOR_RESET);
    }
    va_end(args);
}

std::string Logger::getTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) %
              std::chrono::milliseconds(1000);

    auto tt = std::chrono::system_clock::to_time_t(now);

    std::tm tm;
    localtime_r(&tt, &tm);

    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << '.' << std::setw(3) << std::setfill('0')
        << ms.count();

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
