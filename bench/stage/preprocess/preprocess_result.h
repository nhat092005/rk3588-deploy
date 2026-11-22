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

/** @brief How a paced window went (the FPS sweep). */
struct Pacing {
    /** Frames per second asked for, 0 for as fast as possible. */
    int targetFps{0};
    /** Frames done per second of the window. */
    double achievedFps{0.0};
    int frames{0};
    /** Frames whose latency was longer than the period (0 when there is no period). */
    int lateFrames{0};
};

/** @brief The inferences of the NPU background during a window. */
struct NpuMeasurement {
    int runs{0};
    double p50Ms{0.0};
};

/** @brief The timing of one method at one point. */
struct Measurement {
    int threads{1};
    Percentiles latency;
    /** CPU time of the process per frame, less the CPU time of the NPU background thread. */
    double cpuMsPerFrame{0.0};
    /** Share of all the cores busy in the window, 0 to 100 (NaN when /proc/stat gave nothing). */
    double systemCpuPercent{0.0};
    /** Set in a paced window. */
    std::optional<Pacing> pacing;
    /** Set when the NPU background runs. */
    std::optional<NpuMeasurement> npu;
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
    /** The pace of the FPS sweep, 0 for as fast as possible. Empty: calls back to back. */
    std::optional<int> fps;
    /** Not empty when the frame could not be made: then there are no methods. */
    std::string error;
    std::vector<MethodResult> methods;
};

/** @brief What is the same for every point of a run, and the board state around it. */
struct RunInfo {
    /** "none" (one point), "source", "output" or "fps". */
    std::string_view sweep;
    std::string_view pattern;
    std::string_view heap;
    /** "rest", or "npu" when a model runs on the NPU meanwhile. */
    std::string_view background;
    /** The model and the cores of the NPU background, empty when there is none. */
    std::string npuModel;
    std::string_view npuCores;
    /** The inferences of the NPU background with no preprocessing, over `seconds`: what the
     *  slowdown of a method is read against. Empty when there is no background. */
    std::optional<NpuMeasurement> npuAlone;
    int runs{0};
    int warmup{0};
    /** Length of a paced window, in seconds. */
    int seconds{0};
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
