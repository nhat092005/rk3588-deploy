#include "postprocess.h"

#include <algorithm>

namespace {
constexpr std::size_t boxChannels{4};
constexpr std::size_t maxNmsCandidates{30'000};
// Added per class so that boxes of different classes never overlap (MAX_WH of model_io).
constexpr float classOffset{7680.0F};
constexpr float epsilon{1e-7F};
constexpr float half{2.0F};

float overlap(float low, float high, float otherLow, float otherHigh) noexcept {
    return std::max(std::min(high, otherHigh) - std::max(low, otherLow), 0.0F);
}
} // namespace

Postprocessor::Postprocessor(const PostprocessConfig& config, Logger logger)
    : settings{config}, log{logger}, candidates(config.anchorCount), order(config.anchorCount) {
    found.reserve(maxDetections);
}

std::optional<Postprocessor> Postprocessor::create(const PostprocessConfig& config,
                                                   const Logger& log) {
    if (config.classCount == 0 || config.anchorCount == 0 ||
        config.anchorCount > maxNmsCandidates || config.modelSize <= 0) {
        log.error("bad postprocess config: classes=", config.classCount,
                  " anchors=", config.anchorCount, " model size=", config.modelSize);
        return std::nullopt;
    }
    return std::optional<Postprocessor>{Postprocessor{config, log}};
}

bool Postprocessor::process(const std::vector<float>& output,
                            const SourceMapping& mapping) noexcept {
    found.clear();
    const std::size_t channels{boxChannels + settings.classCount};
    if (output.size() != channels * settings.anchorCount) {
        log.error("output has ", output.size(), " values, expected ",
                  channels * settings.anchorCount);
        return false;
    }
    collectCandidates(output);
    suppress();
    mapToImage(mapping);
    return true;
}

// Keeps the anchors whose best class score is above the threshold, with the box as left, top,
// right, bottom. `order` then holds their indexes, best confidence first.
void Postprocessor::collectCandidates(const std::vector<float>& output) noexcept {
    const std::size_t anchors{settings.anchorCount};
    const float scale{settings.normalizedBoxes ? static_cast<float>(settings.modelSize) : 1.0F};
    order.resize(anchors); // back to full size, within the capacity
    std::size_t count{0};
    for (std::size_t anchor{0}; anchor < anchors; ++anchor) {
        float best{output[boxChannels * anchors + anchor]};
        int bestClass{0};
        for (std::size_t cls{1}; cls < settings.classCount; ++cls) {
            const float score{output[(boxChannels + cls) * anchors + anchor]};
            if (score > best) {
                best = score;
                bestClass = static_cast<int>(cls);
            }
        }
        if (!(best > settings.confThreshold)) {
            continue;
        }
        const float centerX{output[anchor] * scale};
        const float centerY{output[anchors + anchor] * scale};
        const float width{output[2 * anchors + anchor] * scale};
        const float height{output[3 * anchors + anchor] * scale};
        Candidate& candidate{candidates[count]};
        candidate.left = centerX - width / half;
        candidate.top = centerY - height / half;
        candidate.right = centerX + width / half;
        candidate.bottom = centerY + height / half;
        candidate.confidence = best;
        candidate.classId = bestClass;
        order[count] = static_cast<std::uint32_t>(count);
        ++count;
    }
    order.resize(count);
    // Confidence falling, equal confidences by rising anchor index. Python leaves ties open
    // (argsort is not stable), this order matched it on the fixture at the 0.25 threshold.
    std::sort(order.begin(), order.end(), [this](std::uint32_t first, std::uint32_t second) {
        const float firstScore{candidates[first].confidence};
        const float secondScore{candidates[second].confidence};
        return firstScore != secondScore ? firstScore > secondScore : first < second;
    });
}

// Greedy NMS on the boxes moved by the class index, the way nms_xyxy of model_io does it. Stops
// at maxDetections kept boxes: the rest would be cut off anyway.
void Postprocessor::suppress() noexcept {
    std::size_t remaining{order.size()};
    std::size_t first{0};
    while (first < remaining && found.size() < maxDetections) {
        const Candidate& best{candidates[order[first]]};
        found.push_back(
            Detection{best.left, best.top, best.right, best.bottom, best.confidence, best.classId});
        const float shift{static_cast<float>(best.classId) * classOffset};
        const float bestLeft{best.left + shift};
        const float bestTop{best.top + shift};
        const float bestRight{best.right + shift};
        const float bestBottom{best.bottom + shift};
        const float bestArea{(bestRight - bestLeft) * (bestBottom - bestTop)};
        std::size_t kept{first};
        for (std::size_t slot{first + 1}; slot < remaining; ++slot) {
            const Candidate& other{candidates[order[slot]]};
            const float otherShift{static_cast<float>(other.classId) * classOffset};
            const float otherLeft{other.left + otherShift};
            const float otherTop{other.top + otherShift};
            const float otherRight{other.right + otherShift};
            const float otherBottom{other.bottom + otherShift};
            const float inter{overlap(bestLeft, bestRight, otherLeft, otherRight) *
                              overlap(bestTop, bestBottom, otherTop, otherBottom)};
            const float otherArea{(otherRight - otherLeft) * (otherBottom - otherTop)};
            const float iou{inter / (bestArea + otherArea - inter + epsilon)};
            if (iou <= settings.iouThreshold) {
                ++kept;
                order[kept] = order[slot];
            }
        }
        // Slots first + 1 .. kept now hold the survivors: the next best box is at first + 1.
        remaining = kept + 1;
        ++first;
    }
}

// Scales the kept boxes back to the letterboxed image: remove the padding, divide by the gain,
// clip to the image.
void Postprocessor::mapToImage(const SourceMapping& mapping) noexcept {
    const float gain{static_cast<float>(mapping.gain)};
    const float padLeft{static_cast<float>(mapping.left)};
    const float padTop{static_cast<float>(mapping.top)};
    const float maxX{static_cast<float>(mapping.width)};
    const float maxY{static_cast<float>(mapping.height)};
    for (Detection& detection : found) {
        detection.left = std::clamp((detection.left - padLeft) / gain, 0.0F, maxX);
        detection.right = std::clamp((detection.right - padLeft) / gain, 0.0F, maxX);
        detection.top = std::clamp((detection.top - padTop) / gain, 0.0F, maxY);
        detection.bottom = std::clamp((detection.bottom - padTop) / gain, 0.0F, maxY);
    }
}
