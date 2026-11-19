#include "evidence_hub.h"

#include <utility>

std::unique_ptr<EvidenceHub> makeEvidenceHub(const EvidenceConfig& config, const Logger& log) {
    std::vector<std::unique_ptr<EvidenceSlot>> slots;
    slots.reserve(evidenceSlotCount);
    for (std::size_t index{0}; index < evidenceSlotCount; ++index) {
        std::optional<FrameSlot> original;
        std::optional<FrameSlot> marked;
        if (config.images) {
            original = makeFrameSlot(config.imageBytes, lowHeap, log);
            marked = makeFrameSlot(config.imageBytes, lowHeap, log);
            if (!original || !marked) {
                return nullptr;
            }
        }
        slots.push_back(std::make_unique<EvidenceSlot>(std::move(original), std::move(marked)));
    }
    std::optional<ClipRecorder> recorder;
    if (config.clip) {
        recorder = ClipRecorder::create(*config.clip, log);
        if (!recorder) {
            return nullptr;
        }
    }
    return std::make_unique<EvidenceHub>(std::move(slots), std::move(recorder));
}
