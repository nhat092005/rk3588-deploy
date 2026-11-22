#include "timing.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>

#include <time.h>

namespace {
constexpr double millisPerSecond{1'000.0};
constexpr double nanosPerMilli{1'000'000.0};
constexpr double percent50{50.0};
constexpr double percent95{95.0};
constexpr double percent99{99.0};
constexpr double percentTotal{100.0};

// Milliseconds of a POSIX clock. An unsupported clock is a bug, not a runtime failure.
double clockMs(clockid_t clock) noexcept {
    timespec now{};
    if (clock_gettime(clock, &now) != 0) {
        std::abort();
    }
    return (static_cast<double>(now.tv_sec) * millisPerSecond) +
           (static_cast<double>(now.tv_nsec) / nanosPerMilli);
}

// The sample at the nearest rank of the percentile, in a sorted non-empty set.
double rankOf(const std::vector<double>& sorted, double percent) {
    const double rank{std::ceil((percent / percentTotal) * static_cast<double>(sorted.size()))};
    const std::size_t index{rank < 1.0 ? 0 : static_cast<std::size_t>(rank) - 1};
    return sorted.at(std::min(index, sorted.size() - 1));
}
} // namespace

Percentiles percentilesOf(std::vector<double>& samples) {
    if (samples.empty()) {
        return {};
    }
    std::sort(samples.begin(), samples.end());
    return {rankOf(samples, percent50), rankOf(samples, percent95), rankOf(samples, percent99)};
}

double wallMs() noexcept {
    return clockMs(CLOCK_MONOTONIC);
}

double processCpuMs() noexcept {
    return clockMs(CLOCK_PROCESS_CPUTIME_ID);
}
