#ifndef OUTPUT_BRANCH_H
#define OUTPUT_BRANCH_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

#include "app_options.h"
#include "camera.h"
#include "evidence_hub.h"
#include "frame_data.h"
#include "logger.h"
#include "packet_queue.h"
#include "rga.h"

/** @brief The state of the output branch. Defined in output_branch.cpp. */
struct BranchState;

/** @brief Times of the stages of one frame, in milliseconds. */
struct BranchTimes {
    double copy{0.0};
    double draw{0.0};
    double encode{0.0};
    /** The RGA copies of the images of a violation, 0 when there was none. */
    double evidence{0.0};
};

/** @brief Counts that the output branch keeps since its creation. */
struct BranchCounts {
    /** Violations that got no evidence slot, or whose evidence did not fit the queue. */
    std::size_t evidenceDropped{0};
    /** Violations whose clip was skipped (no buffer, no key frame) or lost in the queue. */
    std::size_t clipsSkipped{0};
    /** Boxes that did not fit the overlay or the labels. */
    std::size_t boxesDropped{0};
    /** Frames since creation. */
    std::int64_t frames{0};
};

/**
 * @brief The writer thread's work on one frame: copy the camera frame upright with the RGA, draw
 *        the boxes and labels, encode the video, keep the packet in the clip ring, and hand the
 *        evidence of a violation to the evidence thread. Move-only.
 *
 * One thread calls copy(), encode() and finish(). The frame goes through two calls so that the
 * caller can return the camera buffer between them.
 */
class OutputBranch {
public:
    /**
     * @brief Allocates the snapshot slot, the drawing, the video encoder, and the evidence hub.
     * @param options The command line: video bitrate, clip times, minimum confidence.
     * @param format The camera frame format.
     * @param recordQueue Where every encoded packet is pushed for the continuous recording, or
     *        null for no recording. It must outlive the branch.
     * @param log Logger that the object keeps for the rest of its life.
     * @return The branch, or empty on failure, which is logged.
     */
    static std::optional<OutputBranch> create(const AppOptions& options, const CameraFormat& format,
                                              PacketQueue* recordQueue, const Logger& log);

    OutputBranch(const OutputBranch&) = delete;
    OutputBranch& operator=(const OutputBranch&) = delete;
    OutputBranch(OutputBranch&& other) noexcept;
    OutputBranch& operator=(OutputBranch&& other) noexcept;
    ~OutputBranch();

    /** @brief Size of the upright frame, which is also the size of the images and the boxes. */
    ImageSize imageSize() const noexcept;

    /** @brief Bytes per line of the upright frame. */
    int imageStride() const noexcept;

    /** @brief The evidence slots, clip recorder and queue, for the evidence thread. Owned by the
     *         branch. */
    EvidenceHub& hub() noexcept;

    /**
     * @brief Copies the camera frame into the next snapshot slot, rotated upright. The camera
     *        buffer can go back to the camera as soon as this returns.
     * @param cameraFrame The RGA image of the camera buffer.
     * @return false when the RGA fails, which is logged.
     */
    [[nodiscard]] bool copy(const RgaImage& cameraFrame) noexcept;

    /**
     * @brief Draws, encodes and stores for the frame that copy() took.
     * @param data The detections, tracks and violations of the frame.
     * @param seconds Time the frame was captured.
     * @param times Receives the time of each stage.
     * @return false when the RGA or the encoder fails, which is logged.
     */
    [[nodiscard]] bool encode(const FrameData& data, double seconds, BranchTimes& times) noexcept;

    /** @brief Ends the clips that are still being recorded and hands them over. Call at the end
     *         of the stream. */
    void finish() noexcept;

    /** @brief The counts since creation. */
    BranchCounts counts() const noexcept;

private:
    explicit OutputBranch(std::unique_ptr<BranchState> initial) noexcept;

    std::unique_ptr<BranchState> state;
};

#endif // OUTPUT_BRANCH_H
