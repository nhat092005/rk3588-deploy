#ifndef BENCH_DRAW_RESULT_H
#define BENCH_DRAW_RESULT_H

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "draw_method.h"
#include "freq_state.h"
#include "letterbox_layout.h"
#include "timing.h"

/** @brief How the frame of one method differs from the frame of the reference method (R2). */
struct DrawDifference {
    /** Bytes of the NV12 frame that are not equal. */
    long long differing{0};
    /** Largest absolute difference of a byte. */
    int largest{0};
    /** Bytes that differ by more than 16. */
    long long overSixteen{0};
};

/** @brief The timing of one method at one point. */
struct DrawTiming {
    int threads{1};
    Percentiles latency;
    /** CPU time of the process per frame, all threads, waiting not counted. */
    double cpuMsPerFrame{0.0};
    /** Share of all the cores busy in the window, 0 to 100 (NaN when /proc/stat gave nothing). */
    double systemCpuPercent{0.0};
};

/** @brief What one method gave at one point. */
struct DrawMethodResult {
    DrawKind kind{DrawKind::Rga2};
    MethodStatus status{MethodStatus::Ok};
    /** The reason when the status is not Ok. */
    std::string message;
    /** Valid when the status is Ok. */
    DrawTiming timing;
    /** Empty for the reference method and when the reference did not run. */
    std::optional<DrawDifference> difference;
};

/** @brief One frame size and box count, with a result per method. */
struct DrawPointResult {
    ImageSize size;
    int boxes{0};
    /** Not empty when the point could not be set up: then there are no methods. */
    std::string error;
    std::vector<DrawMethodResult> methods;
};

/** @brief What is the same for every point of a run, and the board state around it. */
struct DrawRunInfo {
    /** "none" (one point), "boxes" or "source". */
    std::string_view sweep;
    std::string_view pattern;
    std::string_view heap;
    /** "reused" (drawn on again in place) or "fresh" (a new frame by device copy per run). */
    std::string_view frame;
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
std::string drawResultJson(const DrawRunInfo& info, const std::vector<DrawPointResult>& points);

#endif // BENCH_DRAW_RESULT_H
