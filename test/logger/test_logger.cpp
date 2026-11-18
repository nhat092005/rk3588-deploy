#include "logger.h"

namespace {
constexpr int sampleCount{-7};
constexpr unsigned int sampleMask{0xBEEF};
constexpr int maskWidth{8};
constexpr double sampleRatio{29.9401};
constexpr int ratioPrecision{3};
} // namespace

int main() {
    const Logger log{"TEST", LogLevel::Verbose};
    log.error("error level");
    log.warn("warn level");
    log.info("info level");
    log.debug("debug level");
    log.verbose("verbose level");
    log.info("int=", sampleCount, " hex=0x", Hex{sampleMask, maskWidth},
             " fixed=", Fixed{sampleRatio, ratioPrecision}, " bool=", true, " char=", 'c');

    const Logger quiet{"TEST", LogLevel::Error};
    quiet.info("this line is suppressed by the logger level");

    return 0;
}
