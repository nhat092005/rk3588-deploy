#ifndef BENCH_TIMING_H
#define BENCH_TIMING_H

#include <vector>

/** @brief Percentiles of a set of times, in the unit of the samples. */
struct Percentiles {
    double p50{0.0};
    double p95{0.0};
    double p99{0.0};
};

/**
 * @brief The nearest-rank percentiles of a set of samples.
 * @param samples Times to rank. Reordered by the call.
 * @return The 50th, 95th and 99th percentile, all 0 for an empty set.
 */
Percentiles percentilesOf(std::vector<double>& samples);

/**
 * @brief The monotonic clock, for the latency of one call.
 * @return Milliseconds from an arbitrary fixed start. Only differences mean something.
 */
double wallMs() noexcept;

/**
 * @brief The CPU time of this process, user and system, summed over all its threads
 *        (CLOCK_PROCESS_CPUTIME_ID). Time spent waiting is not counted.
 * @return Milliseconds from an arbitrary fixed start. Only differences mean something.
 */
double processCpuMs() noexcept;

#endif // BENCH_TIMING_H
