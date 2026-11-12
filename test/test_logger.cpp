#include "logger.h"

static constexpr const char* TAG_TEST = "TEST";

int main() {
    Logger::setGlobalLevel(Logger::VERBOSE);

    LOGE(TAG_TEST, "error level");
    LOGW(TAG_TEST, "warn level");
    LOGI(TAG_TEST, "info level");
    LOGD(TAG_TEST, "debug level");
    LOGV(TAG_TEST, "verbose level");

    Logger::setLevel(TAG_TEST, Logger::ERROR);
    LOGI(TAG_TEST, "this line is suppressed by tag override");

    return 0;
}
