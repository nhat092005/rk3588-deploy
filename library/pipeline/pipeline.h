#ifndef PIPELINE_H
#define PIPELINE_H

#include <memory>
#include <optional>

#include "app_options.h"
#include "handles.h"
#include "logger.h"

/** @brief The pipeline state: the devices, the work buffers and the queues. Defined in
 *         pipeline.cpp. */
struct PipelineState;

/**
 * @brief Camera, RGA letterbox, NPU, postprocess, tracker, violation rule, and the output branch
 *        (upright copy, boxes and labels, H.265 video, clips, JPEG evidence), on four threads.
 *        Move-only.
 *
 * - Camera thread (the caller of run(), CPU 6): takes a camera frame, the RGA letterboxes it into
 *   a model input, hands it over.
 * - NPU thread (CPU 5): runs the model, decodes, tracks the Person boxes, applies the violation
 *   rule.
 * - Writer thread (CPU 4): copies the camera frame upright with the RGA, gives the camera buffer
 *   back, draws the boxes and labels, encodes the video, keeps the packets in a ring and cuts the
 *   clip of a violation.
 * - Evidence thread (CPU 7): encodes the JPEGs of a violation and writes them, with its clip, to
 *   the output folder.
 *
 * A thread never waits for the one after it: when the next stage is busy the frame is dropped
 * for that stage (and counted), and the camera buffer goes back. The design is measured in
 * `probes/pipeline/probe_threads.cpp`. A line of statistics per thread is logged every
 * AppOptions::statsSeconds.
 */
class Pipeline {
public:
    /**
     * @brief Opens the camera and the model, allocates every buffer, creates the threads' objects.
     * @param options The command line, from parseOptions().
     * @param log Logger that the object keeps for the rest of its life.
     * @return The pipeline, or empty when a device, the model, the output folder or a buffer
     *         fails, which is logged.
     */
    static std::optional<Pipeline> create(const AppOptions& options, const Logger& log);

    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;
    Pipeline(Pipeline&& other) noexcept;
    Pipeline& operator=(Pipeline&& other) noexcept;
    ~Pipeline();

    /**
     * @brief Runs until the stop signal, AppOptions::frames frames, or a failure, then ends the
     *        threads in order and finishes the clips that are being recorded.
     * @param stop The signal that ends the run.
     * @return true when it ended by the signal or by the frame count and the camera closed
     *         cleanly, false on a failure (camera, RGA, NPU, encoder), which is logged. The camera
     *         is closed when run() returns, so call it once.
     */
    [[nodiscard]] bool run(const StopSignal& stop);

private:
    explicit Pipeline(std::unique_ptr<PipelineState> initial) noexcept;

    std::unique_ptr<PipelineState> state;
};

#endif // PIPELINE_H
