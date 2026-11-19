#include "clip_recorder.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace {
// Bytes of a buffer are the expected rate times the time it must hold, with room for the rate
// going over the target (key frames are several times a P frame).
constexpr double rateMargin{1.5};
constexpr double headroomSeconds{1.0};

constexpr int slotFree{0};
constexpr int slotRecording{1};
constexpr int slotFinished{2};
constexpr int slotHanded{3};

struct PacketEntry {
    std::size_t offset{0};
    std::size_t size{0};
    bool keyFrame{false};
    double seconds{0.0};
};

bool configValid(const ClipConfig& config) {
    return config.bytesPerSecond > 0 && config.preSeconds >= 0.0 && config.postSeconds >= 0.0 &&
           config.gopSeconds > 0.0 && config.framesPerSecond > 0 && config.clipCount > 0 &&
           std::isfinite(config.preSeconds) && std::isfinite(config.postSeconds);
}

std::size_t bytesFor(const ClipConfig& config, double seconds) {
    return static_cast<std::size_t>(
        std::ceil(static_cast<double>(config.bytesPerSecond) * seconds * rateMargin));
}
} // namespace

// One clip buffer. The phase is the only field that two threads touch.
struct ClipSlot {
    std::atomic<int> phase{slotFree};
    std::vector<std::uint8_t> bytes;
    std::size_t used{0};
    // The packets in the bytes, sized by create(): adding one allocates nothing.
    std::vector<PacketInfo> packets;
    std::size_t packetCount{0};
    std::int64_t eventId{0};
    double startSeconds{0.0};
    double eventSeconds{0.0};
};

struct ClipState {
    ClipState(const ClipConfig& configuration, Logger logger)
        : config{configuration}, log{logger},
          ring(bytesFor(configuration,
                        configuration.preSeconds + configuration.gopSeconds + headroomSeconds)),
          index(static_cast<std::size_t>(std::ceil(
                    (configuration.preSeconds + configuration.gopSeconds + headroomSeconds) *
                    configuration.framesPerSecond)) +
                2),
          slots(configuration.clipCount) {
        const std::size_t clipBytes{
            bytesFor(configuration, configuration.preSeconds + configuration.gopSeconds +
                                        configuration.postSeconds + headroomSeconds)};
        // Frames of a whole clip, with room for the packets that a rounding adds.
        const auto clipPackets{static_cast<std::size_t>(
                                   std::ceil((configuration.preSeconds + configuration.gopSeconds +
                                              configuration.postSeconds + headroomSeconds) *
                                             configuration.framesPerSecond)) +
                               2};
        for (ClipSlot& slot : slots) {
            slot.bytes.resize(clipBytes);
            slot.packets.resize(clipPackets);
        }
        recording.reserve(configuration.clipCount);
        waiting.reserve(configuration.clipCount);
    }
    ClipState(const ClipState&) = delete;
    ClipState& operator=(const ClipState&) = delete;
    ClipState(ClipState&&) = delete;
    ClipState& operator=(ClipState&&) = delete;
    ~ClipState() = default;

    void store(const std::uint8_t* data, std::size_t size, bool keyFrame, double seconds);
    void evictFor(std::size_t size);
    void copyFromRing(const PacketEntry& entry, ClipSlot& slot) const;
    static bool append(ClipSlot& slot, const std::uint8_t* data, std::size_t size, bool keyFrame,
                       double seconds);
    void finishAt(std::size_t position);

    ClipConfig config;
    Logger log;
    std::vector<std::uint8_t> ring;
    std::size_t head{0};
    std::size_t used{0};
    std::vector<PacketEntry> index;
    std::size_t indexFirst{0};
    std::size_t indexCount{0};
    std::vector<ClipSlot> slots;
    std::vector<std::size_t> recording;
    std::vector<std::size_t> waiting;
    double lastSeconds{0.0};
    std::size_t skipped{0};
    std::size_t oversized{0};
};

void ClipState::evictFor(std::size_t size) {
    while (indexCount > 0 && (used + size > ring.size() || indexCount >= index.size())) {
        used -= index.at(indexFirst).size;
        indexFirst = (indexFirst + 1) % index.size();
        --indexCount;
    }
}

void ClipState::store(const std::uint8_t* data, std::size_t size, bool keyFrame, double seconds) {
    if (size > ring.size()) {
        ++oversized;
        return;
    }
    evictFor(size);
    const std::size_t first{std::min(size, ring.size() - head)};
    std::memcpy(ring.data() + head, data, first);
    std::memcpy(ring.data(), data + first, size - first);
    index.at((indexFirst + indexCount) % index.size()) = PacketEntry{head, size, keyFrame, seconds};
    ++indexCount;
    head = (head + size) % ring.size();
    used += size;
}

void ClipState::copyFromRing(const PacketEntry& entry, ClipSlot& slot) const {
    const std::size_t first{std::min(entry.size, ring.size() - entry.offset)};
    std::memcpy(slot.bytes.data() + slot.used, ring.data() + entry.offset, first);
    std::memcpy(slot.bytes.data() + slot.used + first, ring.data(), entry.size - first);
    slot.used += entry.size;
    slot.packets.at(slot.packetCount) = PacketInfo{entry.size, entry.keyFrame, entry.seconds};
    ++slot.packetCount;
}

bool ClipState::append(ClipSlot& slot, const std::uint8_t* data, std::size_t size, bool keyFrame,
                       double seconds) {
    if (slot.used + size > slot.bytes.size() || slot.packetCount >= slot.packets.size()) {
        return false;
    }
    std::memcpy(slot.bytes.data() + slot.used, data, size);
    slot.used += size;
    slot.packets.at(slot.packetCount) = PacketInfo{size, keyFrame, seconds};
    ++slot.packetCount;
    return true;
}

// Moves the clip at `position` of the recording list to the finished list.
void ClipState::finishAt(std::size_t position) {
    const std::size_t slotIndex{recording.at(position)};
    slots.at(slotIndex).phase.store(slotFinished, std::memory_order_release);
    recording.erase(recording.begin() + static_cast<std::ptrdiff_t>(position));
    waiting.push_back(slotIndex);
}

std::optional<ClipRecorder> ClipRecorder::create(const ClipConfig& config, const Logger& log) {
    if (!configValid(config)) {
        log.error("bad clip config: ", config.bytesPerSecond, " bytes/s, pre ", config.preSeconds,
                  " s, post ", config.postSeconds, " s, gop ", config.gopSeconds, " s, ",
                  config.framesPerSecond, " fps, ", config.clipCount, " clips");
        return std::nullopt;
    }
    return std::optional<ClipRecorder>{ClipRecorder{std::make_unique<ClipState>(config, log)}};
}

ClipRecorder::ClipRecorder(std::unique_ptr<ClipState> initial) noexcept
    : state{std::move(initial)} {}
ClipRecorder::ClipRecorder(ClipRecorder&& other) noexcept = default;
ClipRecorder& ClipRecorder::operator=(ClipRecorder&& other) noexcept = default;
ClipRecorder::~ClipRecorder() = default;

void ClipRecorder::addPacket(const std::uint8_t* data, std::size_t size, bool keyFrame,
                             double seconds) noexcept {
    ClipState& clips{*state};
    clips.lastSeconds = seconds;
    clips.store(data, size, keyFrame, seconds);
    // From the back, so that finishing a clip does not move the ones still to visit.
    for (std::size_t position{clips.recording.size()}; position-- > 0;) {
        ClipSlot& slot{clips.slots.at(clips.recording.at(position))};
        const bool kept{ClipState::append(slot, data, size, keyFrame, seconds)};
        if (!kept || seconds - slot.eventSeconds >= clips.config.postSeconds) {
            clips.finishAt(position);
        }
    }
}

bool ClipRecorder::start(std::int64_t eventId) noexcept {
    ClipState& clips{*state};
    const auto freeSlot{
        std::find_if(clips.slots.begin(), clips.slots.end(), [](const ClipSlot& slot) {
            return slot.phase.load(std::memory_order_acquire) == slotFree;
        })};
    // The newest key frame at least preSeconds old, or else the oldest key frame in the ring.
    std::optional<std::size_t> from;
    for (std::size_t offset{0}; offset < clips.indexCount; ++offset) {
        const std::size_t position{(clips.indexFirst + offset) % clips.index.size()};
        const PacketEntry& entry{clips.index.at(position)};
        if (!entry.keyFrame) {
            continue;
        }
        const bool oldEnough{entry.seconds <= clips.lastSeconds - clips.config.preSeconds};
        if (oldEnough || !from) {
            from = offset;
        }
    }
    if (freeSlot == clips.slots.end() || !from) {
        ++clips.skipped;
        return false;
    }
    ClipSlot& slot{*freeSlot};
    slot.used = 0;
    slot.packetCount = 0;
    for (std::size_t offset{*from}; offset < clips.indexCount; ++offset) {
        const PacketEntry& entry{clips.index.at((clips.indexFirst + offset) % clips.index.size())};
        // The clip buffer is larger than the ring, so the pre-roll always fits.
        clips.copyFromRing(entry, slot);
    }
    slot.eventId = eventId;
    slot.eventSeconds = clips.lastSeconds;
    slot.startSeconds = clips.index.at((clips.indexFirst + *from) % clips.index.size()).seconds;
    slot.phase.store(slotRecording, std::memory_order_release);
    const auto slotIndex{static_cast<std::size_t>(freeSlot - clips.slots.begin())};
    clips.recording.push_back(slotIndex);
    if (clips.config.postSeconds <= 0.0) {
        clips.finishAt(clips.recording.size() - 1);
    }
    return true;
}

void ClipRecorder::finishAll() noexcept {
    ClipState& clips{*state};
    while (!clips.recording.empty()) {
        clips.finishAt(clips.recording.size() - 1);
    }
}

std::optional<ClipRef> ClipRecorder::finished() noexcept {
    ClipState& clips{*state};
    if (clips.waiting.empty()) {
        return std::nullopt;
    }
    const std::size_t slotIndex{clips.waiting.front()};
    clips.waiting.erase(clips.waiting.begin());
    ClipSlot& slot{clips.slots.at(slotIndex)};
    slot.phase.store(slotHanded, std::memory_order_release);
    return ClipRef{slotIndex,         slot.bytes.data(), slot.used,           slot.eventId,
                   slot.startSeconds, slot.eventSeconds, slot.packets.data(), slot.packetCount};
}

void ClipRecorder::release(std::size_t slot) noexcept {
    state->slots.at(slot).phase.store(slotFree, std::memory_order_release);
}

std::size_t ClipRecorder::skippedCount() const noexcept {
    return state->skipped;
}

std::size_t ClipRecorder::oversizedCount() const noexcept {
    return state->oversized;
}
