#ifndef LOGGER_H
#define LOGGER_H

#include <mutex>
#include <string>
#include <unordered_map>

class Logger {
public:
    enum Level { NONE = 0, ERROR, WARN, INFO, DEBUG, VERBOSE };

    static void setGlobalLevel(Level level);
    static void setLevel(const std::string& tag, Level level);
    static Level getLevel(const std::string& tag);
    static void log(Level level, const char* tag, const char* fmt, ...);

private:
    static std::string getTimestamp();
    static const char* levelColor(Level level);

    static Level m_globalLevel;
    static std::unordered_map<std::string, Level> m_tagLevels;
    static std::mutex m_mutex;

    static constexpr const char* COLOR_RESET = "\033[0m";

    static constexpr const char* COLOR_RED = "\033[31m";
    static constexpr const char* COLOR_YELLOW = "\033[33m";
    static constexpr const char* COLOR_GREEN = "\033[32m";
    static constexpr const char* COLOR_BLUE = "\033[34m";
    static constexpr const char* COLOR_WHITE = "\033[37m";
};

#define LOGE(tag, fmt, ...) Logger::log(Logger::ERROR, tag, fmt, ##__VA_ARGS__)

#define LOGW(tag, fmt, ...) Logger::log(Logger::WARN, tag, fmt, ##__VA_ARGS__)

#define LOGI(tag, fmt, ...) Logger::log(Logger::INFO, tag, fmt, ##__VA_ARGS__)

#define LOGD(tag, fmt, ...) Logger::log(Logger::DEBUG, tag, fmt, ##__VA_ARGS__)

#define LOGV(tag, fmt, ...) Logger::log(Logger::VERBOSE, tag, fmt, ##__VA_ARGS__)

// for overriding the log level for specific tags
#define logE(tag, fmt, ...) Logger::log(Logger::ERROR, tag, fmt, ##__VA_ARGS__)

#define logW(tag, fmt, ...) Logger::log(Logger::WARN, tag, fmt, ##__VA_ARGS__)

#define logI(tag, fmt, ...) Logger::log(Logger::INFO, tag, fmt, ##__VA_ARGS__)

#define logD(tag, fmt, ...) Logger::log(Logger::DEBUG, tag, fmt, ##__VA_ARGS__)

#define logV(tag, fmt, ...) Logger::log(Logger::VERBOSE, tag, fmt, ##__VA_ARGS__)

/*
#include "logger.h"

static constexpr const char* TAG_CAM = "CAMERA";
static constexpr const char* TAG_NET = "NETWORK";
static constexpr const char* TAG_UI  = "UI";

Logger::setGlobalLevel(Logger::WARN);

Logger::setLevel(TAG_CAM, Logger::DEBUG);
Logger::setLevel(TAG_UI, Logger::INFO);

LOGD(TAG_CAM, "Capture started");
LOGI(TAG_CAM, "Resolution %dx%d", 1920, 1080);

LOGI(TAG_NET, "Connected");
LOGW(TAG_NET, "Packet loss");

LOGI(TAG_UI, "Window shown");
*/

#endif
