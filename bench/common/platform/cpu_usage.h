#ifndef BENCH_CPU_USAGE_H
#define BENCH_CPU_USAGE_H

#include <cstdint>

/** @brief The CPU time of all the cores of the board since boot, in the ticks of /proc/stat. */
struct CpuTimes {
    /** Ticks in user, system and interrupt work: everything but idle and waiting for I/O. */
    std::uint64_t busy{0};
    /** Ticks of all kinds. */
    std::uint64_t total{0};
};

/**
 * @brief Reads the first line of /proc/stat (all cores added up).
 * @return The times, both 0 when the file cannot be read.
 */
CpuTimes readCpuTimes();

/**
 * @brief The share of the CPU that was busy between two readings, over all the cores.
 * @param before An earlier reading.
 * @param after A later reading.
 * @return 0 to 100 (100 is every core busy the whole time), or NaN when no tick passed or a
 *         reading failed.
 */
double busyPercent(const CpuTimes& before, const CpuTimes& after) noexcept;

#endif // BENCH_CPU_USAGE_H
