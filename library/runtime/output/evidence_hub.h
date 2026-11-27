#ifndef EVIDENCE_HUB_H
#define EVIDENCE_HUB_H

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "clip_recorder.h"
#include "frame_slot.h"
#include "logger.h"
#include "mailbox.h"
#include "violation_rule.h"

/** @brief Evidence waiting for the evidence thread at once, and room of its queue. */
constexpr std::size_t evidenceSlotCount{2};
constexpr std::size_t evidenceQueueCapacity{8};
/** @brief Most violations that one frame can bring. */
constexpr std::size_t maxEventsPerFrame{16};

/** @brief What the evidence thread is asked to do. */
enum class EvidenceKind : std::uint8_t { Images, Clip };

/**
 * @brief One request to the evidence thread: store the images of the violations in an
 *        EvidenceSlot, or add a finished clip to the directory of its violation.
 */
struct EvidenceJob {
    EvidenceKind kind{EvidenceKind::Images};
    /** Images: index of the EvidenceSlot. Clip: the ClipRef::slot to release afterwards. */
    std::size_t slot{0};
    /** Clip: the identifier of the violation and the bytes of the clip. */
    std::int64_t eventId{0};
    const std::uint8_t* data{nullptr};
    std::size_t size{0};
    /** Clip: the packets of the bytes, as ClipRef::packets. */
    const PacketInfo* packets{nullptr};
    std::size_t packetCount{0};
};

/**
 * @brief The violations of one frame and who they are, with the two images when the images are
 *        on (both empty otherwise, and the slot only carries the events).
 */
struct EvidenceSlot {
    EvidenceSlot(std::optional<FrameSlot> first, std::optional<FrameSlot> second)
        : original{std::move(first)}, marked{std::move(second)} {}

    std::optional<FrameSlot> original;
    std::optional<FrameSlot> marked;
    std::array<ViolationEvent, maxEventsPerFrame> events{};
    std::array<std::int64_t, maxEventsPerFrame> eventIds{};
    std::size_t eventCount{0};
    std::time_t wallTime{0};
    /** Set by the writer thread when it fills the slot, cleared by the evidence thread. */
    std::atomic<bool> busy{false};
};

/**
 * @brief What the writer thread and the evidence thread share: the evidence slots, the clip
 *        recorder and the queue between them. Built by makeEvidenceHub().
 */
struct EvidenceHub {
    EvidenceHub(std::vector<std::unique_ptr<EvidenceSlot>> evidence,
                std::optional<ClipRecorder> recorder)
        : slots{std::move(evidence)}, clips{std::move(recorder)} {}

    std::vector<std::unique_ptr<EvidenceSlot>> slots;
    /** Empty when clips are off (no time before and after). */
    std::optional<ClipRecorder> clips;
    Mailbox<EvidenceJob, evidenceQueueCapacity> queue;
};

/** @brief What makeEvidenceHub needs. */
struct EvidenceConfig {
    /** Bytes of one image: the stride x height x 3 / 2 of the upright NV12 frame. */
    std::size_t imageBytes{0};
    /** Whether the slots hold the two images. Off: no image buffer is allocated. */
    bool images{true};
    /** The clip settings, or empty for no clips. */
    std::optional<ClipConfig> clip;
};

/**
 * @brief Allocates the evidence slots (in the low heap, the RGA draws and copies there, unless the
 *        images are off) and the clip recorder.
 * @return The hub, or empty on failure, which is logged.
 */
std::unique_ptr<EvidenceHub> makeEvidenceHub(const EvidenceConfig& config, const Logger& log);

#endif // EVIDENCE_HUB_H
