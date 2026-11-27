#ifndef FRAME_DATA_H
#define FRAME_DATA_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "byte_tracker.h"
#include "postprocess.h"
#include "violation_rule.h"

/** @brief Frames in flight between the NPU thread and the writer thread. */
constexpr std::size_t frameDataCount{4};

/**
 * @brief What the NPU thread learned about one frame, for the writer thread to draw and store. The
 *        vectors are reserved by the constructor, so filling them allocates nothing.
 */
struct FrameData {
    FrameData() {
        detections.reserve(maxDetections);
        tracks.reserve(defaultTrackCapacity);
        events.reserve(defaultRuleCapacity);
        violators.reserve(defaultRuleCapacity);
    }

    /** All detections of the frame, in the pixels of the upright image. */
    std::vector<Detection> detections;
    /** The Person tracks of the frame. */
    std::vector<TrackedObject> tracks;
    /** The violations that reached their hold time in this frame. */
    std::vector<ViolationEvent> events;
    /** The identifiers of the Person tracks that have a reported violation now. */
    std::vector<int> violators;
    /** Set by the NPU thread when it hands the data over, cleared by the writer thread when done.
     */
    std::atomic<bool> busy{false};
};

/** @brief A frame between the camera thread and the NPU thread. */
struct InferJob {
    /** Index of the camera buffer, which the frame holds until the writer has copied it. */
    std::size_t cameraIndex{0};
    /** The model input buffer that the letterbox filled. */
    std::size_t inputSlot{0};
    double captureSeconds{0.0};
    std::int64_t number{0};
};

/** @brief A frame between the NPU thread and the writer thread. */
struct OutputJob {
    std::size_t cameraIndex{0};
    /** Index of the FrameData that holds the results. */
    std::size_t dataSlot{0};
    double captureSeconds{0.0};
    std::int64_t number{0};
};

#endif // FRAME_DATA_H
