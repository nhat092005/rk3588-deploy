// StageStats: median, maximum, window capacity and clear. Plain C++, runs on the PC. Exit code 0
// when every check passes.
#include <array>
#include <cstddef>
#include <cstdlib>

#include "logger.h"
#include "stage_stats.h"

namespace {
class Check {
public:
    explicit Check(const Logger& logger) : log{logger} {}

    void expect(bool condition, const char* what) {
        if (!condition) {
            log.error("FAIL: ", what);
            passed = false;
        }
    }

    bool ok() const {
        return passed;
    }

private:
    Logger log;
    bool passed{true};
};

constexpr std::size_t windowSize{8};
// Eight samples in no particular order: sorted 0.5 1 2 3 4 5 7 9.
constexpr std::array<double, windowSize> samples{5.0, 1.0, 9.0, 3.0, 0.5, 2.0, 7.0, 4.0};
constexpr double extraSample{100.0};
constexpr double singleSample{5.0};
constexpr double smallSample{2.0};
// The middle one of 5, 1, 9 sorted is 5. Of 5, 1, 9, 3 (sorted 1 3 5 9), the upper middle is 5.
constexpr double middleOfThree{5.0};
constexpr double upperMiddleOfFour{5.0};
constexpr double largestOfAll{9.0};

void checkStatistics(Check& check) {
    StageStats stats{windowSize};
    check.expect(stats.count() == 0 && stats.median() == 0.0 && stats.maximum() == 0.0,
                 "an empty window");
    stats.add(singleSample);
    check.expect(stats.count() == 1 && stats.median() == singleSample &&
                     stats.maximum() == singleSample,
                 "one sample");
    stats.add(samples.at(1));
    stats.add(samples.at(2));
    check.expect(stats.median() == middleOfThree, "the middle of three, in any order");
    check.expect(stats.maximum() == largestOfAll, "the largest");
    stats.add(samples.at(3));
    check.expect(stats.median() == upperMiddleOfFour, "an even count takes the upper middle");
    for (std::size_t index{4}; index < windowSize; ++index) {
        stats.add(samples.at(index));
    }
    check.expect(stats.count() == windowSize, "a full window");
    stats.add(extraSample);
    check.expect(stats.count() == windowSize && stats.maximum() == largestOfAll,
                 "a sample added to a full window");
    stats.clear();
    check.expect(stats.count() == 0 && stats.maximum() == 0.0 && stats.median() == 0.0,
                 "clear empties the window");
    stats.add(smallSample);
    check.expect(stats.maximum() == smallSample, "the maximum starts again after clear");
}

void checkCapacity(Check& check) {
    StageStats zero{0};
    zero.add(smallSample);
    zero.add(extraSample);
    check.expect(zero.count() == 1 && zero.maximum() == smallSample, "capacity 0 holds one sample");
}
} // namespace

int main() {
    const Logger log{"TEST_STATS", LogLevel::Debug};
    Check check{log};
    checkStatistics(check);
    checkCapacity(check);
    if (check.ok()) {
        log.info("PASS");
        return EXIT_SUCCESS;
    }
    log.error("FAIL");
    return EXIT_FAILURE;
}
