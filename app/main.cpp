#include "logger.h"

static constexpr const char* TAG_APP = "APP";
static constexpr const char* TAG_NETWORK = "NETWORK";

int main() {
    Logger::setGlobalLevel(Logger::DEBUG);
    Logger::setLevel(TAG_NETWORK, Logger::WARN);

    LOGI(TAG_APP, "Application started");
    LOGD(TAG_APP, "Debug detail value=%d", 42);

    LOGI(TAG_NETWORK, "This INFO line is suppressed (tag level = WARN)");
    LOGW(TAG_NETWORK, "Packet loss detected");
    LOGE(TAG_NETWORK, "Connection failed");

    return 0;
}
