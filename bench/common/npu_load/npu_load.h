#ifndef BENCH_NPU_LOAD_H
#define BENCH_NPU_LOAD_H

#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "dmabuf.h"
#include "logger.h"
#include "npu_infer.h"
#include "timing.h"

/** @brief The inferences that finished in a window. */
struct NpuWindow {
    std::size_t runs{0};
    /** Latency of one inference, in ms. */
    Percentiles latency;
};

/**
 * @brief The background of a "NPU running" measurement: one thread that runs a model back to back
 *        on the NPU, on a constant input, until the object is destroyed.
 *
 * Back to back is the heaviest load: a frame that is preprocessed always meets an inference in
 * progress. The time of an inference does not depend on the pixel values.
 */
class NpuLoad {
public:
    /**
     * @brief Loads the model and starts the thread.
     * @param config The model and the NPU cores.
     * @param log Logger that the load keeps.
     * @return The load, or empty when the buffer, the model or the input fails, which is logged.
     */
    static std::unique_ptr<NpuLoad> start(const NpuConfig& config, const Logger& log);

    NpuLoad(const NpuLoad&) = delete;
    NpuLoad& operator=(const NpuLoad&) = delete;
    NpuLoad(NpuLoad&&) = delete;
    NpuLoad& operator=(NpuLoad&&) = delete;
    /** @brief Stops the thread, then releases the model and the buffer. */
    ~NpuLoad();

    /**
     * @brief The inferences that finished since the last call (or since start).
     * @return Their count and latency.
     */
    NpuWindow takeWindow();

    /**
     * @brief The CPU time of the load thread, user and system.
     * @return Milliseconds from an arbitrary fixed start. Only differences mean something.
     */
    double cpuMs();

    /** @brief True when an inference failed: the thread has stopped. */
    bool failed() const noexcept {
        return failure.load();
    }

private:
    NpuLoad(DmaBuf&& input, NpuInfer&& model) noexcept
        : buffer{std::move(input)}, infer{std::move(model)} {}

    void loop(InputId input);

    // The model is released before the buffer it reads.
    DmaBuf buffer;
    NpuInfer infer;
    std::optional<InputId> inputId;
    std::atomic<bool> stopping{false};
    std::atomic<bool> failure{false};
    std::mutex guard;
    std::vector<double> samples;
    std::thread worker;
};

#endif // BENCH_NPU_LOAD_H
