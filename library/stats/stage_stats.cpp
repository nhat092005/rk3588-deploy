#include "stage_stats.h"

#include <algorithm>

StageStats::StageStats(std::size_t capacity) : limit{std::max<std::size_t>(capacity, 1)} {
    samples.reserve(limit);
}

void StageStats::add(double value) noexcept {
    if (samples.size() >= limit) {
        return;
    }
    samples.push_back(value);
    largest = std::max(largest, value);
}

double StageStats::median() noexcept {
    if (samples.empty()) {
        return 0.0;
    }
    const auto middle{samples.begin() + static_cast<std::ptrdiff_t>(samples.size() / 2)};
    std::nth_element(samples.begin(), middle, samples.end());
    return *middle;
}

void StageStats::clear() noexcept {
    samples.clear();
    largest = 0.0;
}
