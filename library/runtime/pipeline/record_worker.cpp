#include "record_worker.h"

#include <chrono>
#include <ctime>
#include <utility>

#include "recording_retention.h"
#include "segment_writer.h"
#include "video_encoder.h"

namespace {
// The ring holds about this many seconds of the stream at the target bitrate: more than the
// longest stall measured on the board (2.2 s).
constexpr double ringSeconds{6.4};
constexpr std::size_t bytesPerKilobit{125};
// Room for the key frames, which are several times an ordinary frame.
constexpr std::size_t ringMargin{2};
constexpr std::size_t maxQueuedPackets{1024};
// How often the folder is read again and the limits are checked when nothing happens.
constexpr std::chrono::hours housekeepingPeriod{1};
} // namespace

struct RecordWorker::State {
    State(Logger logger, std::unique_ptr<PacketQueue> packets, SegmentWriter segments,
          RecordingRetention limits)
        : log{logger}, packetQueue{std::move(packets)}, writer{std::move(segments)},
          retention{std::move(limits)} {}

    void housekeeping(bool rescan);
    bool hasRoom();
    void collectClosed();

    Logger log;
    std::unique_ptr<PacketQueue> packetQueue;
    SegmentWriter writer;
    RecordingRetention retention;
    bool roomLeft{true};
    RecordCounts counts;
};

// Applies the limits (and reads the folder again when asked) and notes whether there is room.
void RecordWorker::State::housekeeping(bool rescan) {
    if (rescan) {
        retention.rescan();
    }
    const bool before{roomLeft};
    roomLeft = retention.enforce(std::time(nullptr));
    counts.segmentsRemoved.store(static_cast<std::int64_t>(retention.removedCount()));
    if (!roomLeft && before) {
        log.error("the volume has no room for recordings, no new segment is opened until it has");
    } else if (roomLeft && !before) {
        log.info("the volume has room for recordings again");
    }
}

// Whether a new segment may be opened: trims the folder first, so that the oldest segments make
// room for it.
bool RecordWorker::State::hasRoom() {
    housekeeping(false);
    return roomLeft;
}

// Counts the segments that SegmentWriter completed.
void RecordWorker::State::collectClosed() {
    for (const ClosedSegment& segment : writer.takeClosed()) {
        retention.add(segment.path);
        counts.segmentsSaved.fetch_add(1);
        counts.bytesSaved.fetch_add(static_cast<std::int64_t>(segment.bytes));
        log.info("recording saved ", segment.path, " (", segment.bytes, " bytes)");
    }
}

std::optional<RecordWorker> RecordWorker::create(const AppOptions& options, const Logger& log) {
    const std::size_t bytesPerSecond{static_cast<std::size_t>(options.videoKbps) * bytesPerKilobit};
    const auto ringBytes{
        static_cast<std::size_t>(static_cast<double>(bytesPerSecond) * ringSeconds) * ringMargin};
    std::unique_ptr<PacketQueue> queue{
        PacketQueue::create(ringBytes, maxQueuedPackets, defaultVideoCapacity)};
    std::optional<SegmentWriter> writer{SegmentWriter::create(
        SegmentConfig{options.archiveDir, options.archiveSegmentSeconds, defaultFileMode}, log)};
    std::optional<RecordingRetention> retention{RecordingRetention::create(
        RecordingRetentionConfig{
            options.archiveDir,
            static_cast<std::uint64_t>(options.archiveMaxStorageGb * bytesPerGigabyte),
            options.archiveKeepDays, defaultArchiveMinFreeBytes},
        log)};
    if (!queue || !writer || !retention) {
        return std::nullopt;
    }
    auto state{
        std::make_unique<State>(log, std::move(queue), std::move(*writer), std::move(*retention))};
    state->housekeeping(false);
    return std::optional<RecordWorker>{RecordWorker{std::move(state)}};
}

RecordWorker::RecordWorker(std::unique_ptr<State> initial) noexcept : state{std::move(initial)} {}
RecordWorker::RecordWorker(RecordWorker&& other) noexcept = default;
RecordWorker& RecordWorker::operator=(RecordWorker&& other) noexcept = default;
RecordWorker::~RecordWorker() = default;

PacketQueue& RecordWorker::queue() noexcept {
    return *state->packetQueue;
}

void RecordWorker::run() noexcept {
    State& worker{*state};
    QueuedPacket packet;
    packet.bytes.reserve(defaultVideoCapacity);
    while (true) {
        const PopResult result{worker.packetQueue->popFor(packet, housekeepingPeriod)};
        if (result == PopResult::Closed) {
            break;
        }
        if (result == PopResult::Timeout) {
            worker.housekeeping(true);
            continue;
        }
        // The limits are checked where they matter: before a segment is opened.
        bool allowNew{true};
        if (worker.writer.startsSegment(packet)) {
            allowNew = worker.hasRoom();
            if (!allowNew) {
                worker.counts.segmentsRefused.fetch_add(1);
            }
        }
        if (!worker.writer.consume(packet, std::time(nullptr), allowNew)) {
            worker.counts.writeErrors.fetch_add(1);
        }
        worker.collectClosed();
    }
    worker.writer.finish();
    worker.collectClosed();
    worker.housekeeping(false);
}

const RecordCounts& RecordWorker::counts() const noexcept {
    return state->counts;
}

std::uint64_t RecordWorker::storedBytes() const noexcept {
    return state->retention.totalBytes();
}
