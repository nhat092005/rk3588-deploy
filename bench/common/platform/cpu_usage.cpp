#include "cpu_usage.h"

#include <array>
#include <fstream>
#include <limits>
#include <string>

namespace {
// The fields of the "cpu" line: user nice system idle iowait irq softirq steal. The guest time is
// already inside user and nice.
constexpr std::size_t fieldCount{8};
constexpr std::size_t idleField{3};
constexpr std::size_t iowaitField{4};
constexpr double percentOf{100.0};
} // namespace

CpuTimes readCpuTimes() {
    std::ifstream file{"/proc/stat"};
    std::string name;
    std::array<std::uint64_t, fieldCount> ticks{};
    file >> name;
    for (std::uint64_t& value : ticks) {
        file >> value;
    }
    if (!file || name != "cpu") {
        return CpuTimes{};
    }
    CpuTimes times{};
    for (const std::uint64_t value : ticks) {
        times.total += value;
    }
    times.busy = times.total - ticks.at(idleField) - ticks.at(iowaitField);
    return times;
}

double busyPercent(const CpuTimes& before, const CpuTimes& after) noexcept {
    if (after.total <= before.total || after.busy < before.busy) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return percentOf * static_cast<double>(after.busy - before.busy) /
           static_cast<double>(after.total - before.total);
}
