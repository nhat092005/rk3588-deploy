#include "session_worker.h"

#include <chrono>
#include <ctime>
#include <utility>

#include "mp4_writer.h"
#include "session_writer.h"
#include "video_encoder.h"
#include "violation_retention.h"

namespace {
// The ring holds about this many seconds of the stream at the target bitrate: more than the
// longest stall measured on the board (2.2 s).
constexpr double ringSeconds{6.4};
constexpr std::size_t bytesPerKilobit{125};
// Room for the key frames, which are several times an ordinary frame.
constexpr std::size_t ringMargin{2};
constexpr std::size_t maxQueuedPackets{1024};
// How long the thread waits for a packet before it looks at the queue again.
constexpr std::chrono::seconds waitPeriod{1};
} // namespace

struct SessionWorker::State {
    State(std::unique_ptr<PacketQueue> packets, SessionWriter session)
        : packetQueue{std::move(packets)}, writer{std::move(session)} {}

    std::unique_ptr<PacketQueue> packetQueue;
    SessionWriter writer;
    SessionCounts counts;
};

std::optional<SessionWorker> SessionWorker::create(const AppOptions& options, const Logger& log) {
    const std::size_t bytesPerSecond{static_cast<std::size_t>(options.videoKbps) * bytesPerKilobit};
    const auto ringBytes{
        static_cast<std::size_t>(static_cast<double>(bytesPerSecond) * ringSeconds) * ringMargin};
    std::unique_ptr<PacketQueue> queue{
        PacketQueue::create(ringBytes, maxQueuedPackets, defaultVideoCapacity)};
    std::optional<SessionWriter> writer{SessionWriter::create(
        SessionConfig{options.sessionDir,
                      static_cast<std::uint64_t>(options.sessionMinFreeGb * bytesPerGigabyte),
                      defaultFileMode, nullptr, videoOutputOf(options.fileFormat)},
        log)};
    if (!queue || !writer) {
        return std::nullopt;
    }
    return std::optional<SessionWorker>{
        SessionWorker{std::make_unique<State>(std::move(queue), std::move(*writer))}};
}

SessionWorker::SessionWorker(std::unique_ptr<State> initial) noexcept : state{std::move(initial)} {}
SessionWorker::SessionWorker(SessionWorker&& other) noexcept = default;
SessionWorker& SessionWorker::operator=(SessionWorker&& other) noexcept = default;
SessionWorker::~SessionWorker() = default;

PacketQueue& SessionWorker::queue() noexcept {
    return *state->packetQueue;
}

void SessionWorker::run() noexcept {
    State& worker{*state};
    QueuedPacket packet;
    packet.bytes.reserve(defaultVideoCapacity);
    while (true) {
        const PopResult result{worker.packetQueue->popFor(packet, waitPeriod)};
        if (result == PopResult::Closed) {
            break;
        }
        if (result == PopResult::Timeout) {
            continue;
        }
        if (!worker.writer.consume(packet, std::time(nullptr))) {
            worker.counts.writeErrors.fetch_add(1);
        }
        worker.counts.bytesWritten.store(static_cast<std::int64_t>(worker.writer.bytesWritten()));
        worker.counts.gaps.store(static_cast<std::int64_t>(worker.writer.gapCount()));
        worker.counts.stopped.store(worker.writer.stopped());
    }
    worker.writer.finish();
    worker.counts.stopped.store(worker.writer.stopped());
}

const SessionCounts& SessionWorker::counts() const noexcept {
    return state->counts;
}

std::string SessionWorker::path() const {
    return state->writer.path();
}
