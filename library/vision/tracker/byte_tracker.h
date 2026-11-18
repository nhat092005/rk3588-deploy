#ifndef BYTE_TRACKER_H
#define BYTE_TRACKER_H

#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

#include "logger.h"
#include "postprocess.h"

/** @brief Detections at or above this start a track and take part in the first matching,
 *         `track_high_thresh` of bytetrack.yaml. */
constexpr float defaultTrackHighThreshold{0.25F};
/** @brief Detections above this and below the high threshold only take part in the second
 *         matching, `track_low_thresh`. */
constexpr float defaultTrackLowThreshold{0.1F};
/** @brief A detection needs at least this score to open a new track, `new_track_thresh`. */
constexpr float defaultNewTrackThreshold{0.25F};
/** @brief Frames a lost track is kept, `track_buffer`. */
constexpr int defaultTrackBuffer{30};
/** @brief Cost limit of the first matching, `match_thresh`. */
constexpr double defaultMatchThreshold{0.8};
/** @brief Most tracks (confirmed, unconfirmed and lost) kept at once. */
constexpr std::size_t defaultTrackCapacity{128};

/** @brief Thresholds of ByteTrack. The defaults are the values of Ultralytics bytetrack.yaml. */
struct ByteTrackerConfig {
    float highThreshold{defaultTrackHighThreshold};
    float lowThreshold{defaultTrackLowThreshold};
    float newTrackThreshold{defaultNewTrackThreshold};
    int trackBuffer{defaultTrackBuffer};
    double matchThreshold{defaultMatchThreshold};
    /** Multiply the IoU of the first matching by the score of the detection, `fuse_score`. */
    bool fuseScore{true};
    /** Most tracks at once. A detection that would open a track beyond it is ignored. */
    std::size_t trackCapacity{defaultTrackCapacity};
    /** Most detections per frame. The ones after it are ignored. */
    std::size_t detectionCapacity{maxDetections};
};

/** @brief One confirmed track of a frame, in the coordinates of the detections. */
struct TrackedObject {
    float left{0.0F};
    float top{0.0F};
    float right{0.0F};
    float bottom{0.0F};
    /** Track identifier, starting at 1 and never reused until reset(). */
    int id{0};
    /** Score of the detection that updated the track last. */
    float score{0.0F};
    /** Class of the detection that updated the track last. It can change between frames. */
    int classId{0};
    /** Index in the detections of the frame of the detection that updated the track last. */
    int detectionIndex{0};
};

/** @brief The tracks and work buffers. Defined in byte_tracker.cpp. */
struct TrackerState;

/**
 * @brief ByteTrack, a port of `BYTETracker` of Ultralytics 8.4.154 (AGPL-3.0) without ReID and
 *        camera motion compensation: a Kalman filter per track, a first matching of the tracks
 *        with the high score detections, a second matching of the remaining tracks with the low
 *        score ones, a third of the unconfirmed tracks, new tracks from what is left. Matching
 *        uses LapSolver, so the identifiers equal the Python ones. It does not look at the
 *        class of a detection. Move-only. All storage is allocated by create(): update()
 *        allocates nothing.
 *
 * A new track is confirmed, and returned, from its second matched frame (immediately on frame
 * 1), as in Ultralytics.
 */
class ByteTracker {
public:
    /**
     * @brief Checks the configuration and allocates the storage.
     * @param config Thresholds and capacities. trackBuffer at least 1, capacities above 0.
     * @param log Logger that the object keeps for the rest of its life.
     * @return The tracker, or empty when the configuration is invalid, which is logged.
     */
    static std::optional<ByteTracker> create(const ByteTrackerConfig& config, const Logger& log);

    ByteTracker(const ByteTracker&) = delete;
    ByteTracker& operator=(const ByteTracker&) = delete;
    ByteTracker(ByteTracker&& other) noexcept;
    ByteTracker& operator=(ByteTracker&& other) noexcept;
    ~ByteTracker();

    /**
     * @brief Processes one frame.
     * @param detections The detections of the frame, for example Postprocessor::detections() at
     *        a confidence threshold of defaultTrackLowThreshold. Boxes with no area and scores
     *        at or below the low threshold are ignored.
     * @return The confirmed tracks, in the order of Ultralytics, valid until the next call.
     *
     * Call it for every frame, also when there is no detection: lost tracks age by frame.
     */
    const std::vector<TrackedObject>& update(const std::vector<Detection>& detections) noexcept;

    /** @brief Forgets all tracks and restarts the identifiers at 1. */
    void reset() noexcept;

    /**
     * @brief Detections and new tracks dropped since create() because a capacity was full.
     *        Zero in normal use; a warning is logged the first time it is not.
     */
    std::size_t overflowCount() const noexcept;

private:
    explicit ByteTracker(std::unique_ptr<TrackerState> initial) noexcept;

    std::unique_ptr<TrackerState> state;
};

#endif // BYTE_TRACKER_H
