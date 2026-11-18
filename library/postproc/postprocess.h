#ifndef POSTPROCESS_H
#define POSTPROCESS_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "logger.h"

/** @brief Default confidence threshold, `CONF` of rk3588_common.model_io. */
constexpr float defaultConfThreshold{0.25F};
/** @brief Default IoU threshold of the NMS, `IOU` of rk3588_common.model_io. */
constexpr float defaultIouThreshold{0.7F};
/** @brief Most detections returned, `MAX_DET` of rk3588_common.model_io. */
constexpr std::size_t maxDetections{300};

/** @brief One detected object, in pixels of the image that was letterboxed. */
struct Detection {
    float left{0.0F};
    float top{0.0F};
    float right{0.0F};
    float bottom{0.0F};
    /** Confidence of the class, from 0 to 1. */
    float confidence{0.0F};
    /** Index of the class, below PostprocessConfig::classCount. */
    int classId{0};
};

/** @brief Layout of the model output and the thresholds. */
struct PostprocessConfig {
    /** Number of classes: the output has 4 + classCount channels. */
    std::size_t classCount{0};
    /** Number of anchors of the output (8400 for a 640 input), at most 30000. */
    std::size_t anchorCount{0};
    /** Side of the model input in pixels, the factor of normalised boxes. Above 0, no default. */
    int modelSize{0};
    /** Whether the boxes are divided by the model input size (INT8 Ultralytics export). */
    bool normalizedBoxes{false};
    /** Candidates need a confidence above this. */
    float confThreshold{defaultConfThreshold};
    /** A box is suppressed when its IoU with a better box of the same class is above this. */
    float iouThreshold{defaultIouThreshold};
};

/** @brief How the letterboxed model input maps back to the image that was letterboxed. */
struct SourceMapping {
    /** Scale from the image to the model input (LetterboxLayout::gain). */
    double gain{1.0};
    /** Padding left of the image in the model input, in pixels. */
    int left{0};
    /** Padding above the image in the model input, in pixels. */
    int top{0};
    /** Width of the image in pixels, boxes are clipped to it. */
    int width{0};
    /** Height of the image in pixels, boxes are clipped to it. */
    int height{0};
};

/**
 * @brief Turns the raw output of a YOLO model into detections: confidence threshold, per class
 *        NMS, scaling back to the letterboxed image. The same numbers as
 *        rk3588_common.model_io.postprocess. All storage is allocated by create(): process()
 *        allocates nothing. Move-only through its members.
 */
class Postprocessor {
public:
    /**
     * @brief Checks the configuration and allocates the work buffers.
     * @param config Output layout and thresholds. classCount and anchorCount above 0, anchorCount
     *        at most 30000 (MAX_NMS), modelSize above 0.
     * @param log Logger that the object keeps for the rest of its life.
     * @return The postprocessor, or empty when the configuration is invalid, which is logged.
     */
    static std::optional<Postprocessor> create(const PostprocessConfig& config, const Logger& log);

    /**
     * @brief Decodes one output tensor.
     * @param output Floats laid out as [4 + classCount][anchorCount]: channels x, y, w, h, then one
     *        score per class, as NpuInfer::output(0) returns them.
     * @param mapping Letterbox of the image that was run through the model.
     * @return false when output has not exactly (4 + classCount) * anchorCount values, which is
     *         logged. The detections are then empty.
     *
     * At most maxDetections detections, best confidence first. Among equal confidences the
     * earlier anchor comes first. INT8 scores tie often, and numpy's argsort leaves the order of
     * ties open, so the Python result is only reproduced where ties do not decide the outcome.
     */
    [[nodiscard]] bool process(const std::vector<float>& output,
                               const SourceMapping& mapping) noexcept;

    /** @brief The detections of the last process(), valid until the next call. */
    const std::vector<Detection>& detections() const noexcept {
        return found;
    }

private:
    struct Candidate {
        float left{0.0F};
        float top{0.0F};
        float right{0.0F};
        float bottom{0.0F};
        float confidence{0.0F};
        int classId{0};
    };

    Postprocessor(const PostprocessConfig& config, Logger logger);

    void collectCandidates(const std::vector<float>& output) noexcept;
    void suppress() noexcept;
    void mapToImage(const SourceMapping& mapping) noexcept;

    PostprocessConfig settings;
    Logger log;
    std::vector<Candidate> candidates;
    // Candidate indexes in decreasing confidence, shrinking during the NMS.
    std::vector<std::uint32_t> order;
    std::vector<Detection> found;
};

#endif // POSTPROCESS_H
