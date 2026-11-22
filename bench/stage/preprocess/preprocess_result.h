#ifndef BENCH_PREPROCESS_RESULT_H
#define BENCH_PREPROCESS_RESULT_H

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "freq_state.h"
#include "letterbox_layout.h"
#include "preprocess_method.h"
#include "timing.h"

/** @brief Pixel difference of one output to the reference output, over all channel values. */
struct Difference {
    double mean{0.0};
    double largest{0.0};
    /** Percent of the values that differ by more than 16 (BIG_DIFF of check_letterbox.py). */
    double bigShare{0.0};
};

/** @brief The timing of one method at one point. */
struct Measurement {
    int threads{1};
    Percentiles latency;
    double cpuMsPerFrame{0.0};
};

/** @brief What one method gave at one point. */
struct MethodResult {
    MethodKind kind{MethodKind::Cv1};
    MethodStatus status{MethodStatus::Ok};
    /** The reason when the status is not Ok. */
    std::string message;
    /** Valid when the status is Ok. */
    Measurement timing;
    /** Empty for the reference method and when the reference did not run. */
    std::optional<Difference> difference;
};

/** @brief One source size and target size, with a result per method. */
struct PointResult {
    ImageSize source;
    int target{0};
    /** Not empty when the frame could not be made: then there are no methods. */
    std::string error;
    std::vector<MethodResult> methods;
};

/** @brief What is the same for every point of a run, and the board state around it. */
struct RunInfo {
    /** "none" (one point), "source" or "output". */
    std::string_view sweep;
    std::string_view pattern;
    std::string_view heap;
    int runs{0};
    int warmup{0};
    std::string opencv;
    /** The lines of the OpenCV build report that decide the CPU side. */
    std::vector<std::string> build;
    FreqState before;
    FreqState after;
};

/**
 * @brief The result of a run as one JSON document.
 * @param info The run.
 * @param points The points in the order they were measured.
 * @return The JSON text.
 */
std::string resultJson(const RunInfo& info, const std::vector<PointResult>& points);

#endif // BENCH_PREPROCESS_RESULT_H
