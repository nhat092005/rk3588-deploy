#include "stream_worker.h"

#include <algorithm>
#include <chrono>
#include <utility>

#include "child_process.h"

namespace {
// About one second of the stream at the target bitrate.
constexpr double ringSeconds{1.0};
constexpr std::size_t bytesPerKilobit{125};
// Room for the key frames, which are several times an ordinary frame.
constexpr std::size_t ringMargin{4};
constexpr std::size_t maxQueuedPackets{256};
// How often the program is checked for an early end when no packet arrives.
constexpr std::chrono::milliseconds idleStep{200};
// How long the program may take to end after its input closed (it closes the RTSP session).
constexpr std::chrono::milliseconds endGrace{2'000};
} // namespace

struct StreamWorker::State {
    State(Logger logger, StreamConfig settings, std::unique_ptr<PacketQueue> packets)
        : log{logger}, config{std::move(settings)}, packetQueue{std::move(packets)} {}

    void giveUp(std::string_view reason);

    Logger log;
    StreamConfig config;
    std::unique_ptr<PacketQueue> packetQueue;
    std::atomic<bool> stopping{false};
    StreamCounts counts;
};

// One warning, then pushes are refused at once.
void StreamWorker::State::giveUp(std::string_view reason) {
    counts.failed.store(true);
    packetQueue->close();
    log.warn("stream stopped: ", reason, "; detection, recording and clips go on");
}

std::vector<std::string> ffmpegPushCommand(std::string_view url) {
    return {"ffmpeg",
            "-hide_banner",
            "-loglevel",
            "warning",
            "-fflags",
            "nobuffer",
            "-flags",
            "low_delay",
            "-use_wallclock_as_timestamps",
            "1",
            "-f",
            "hevc",
            "-i",
            "pipe:0",
            "-c",
            "copy",
            "-f",
            "rtsp",
            "-rtsp_transport",
            "tcp",
            std::string{url}};
}

std::optional<StreamWorker> StreamWorker::create(const StreamConfig& config, const Logger& log) {
    if (config.command.empty() || config.kbps <= 0 || config.maxPacketBytes == 0 ||
        config.stallMs <= 0) {
        log.error("the stream needs a command, a bitrate, a packet size and a stall time");
        return std::nullopt;
    }
    const std::size_t bytesPerSecond{static_cast<std::size_t>(config.kbps) * bytesPerKilobit};
    const std::size_t timed{
        static_cast<std::size_t>(static_cast<double>(bytesPerSecond) * ringSeconds) * ringMargin};
    std::unique_ptr<PacketQueue> queue{PacketQueue::create(
        std::max(timed, config.maxPacketBytes * 2), maxQueuedPackets, config.maxPacketBytes)};
    if (!queue) {
        log.error("the stream queue was not created");
        return std::nullopt;
    }
    auto state{std::make_unique<State>(log, config, std::move(queue))};
    return std::optional<StreamWorker>{StreamWorker{std::move(state)}};
}

StreamWorker::StreamWorker(std::unique_ptr<State> initial) noexcept : state{std::move(initial)} {}
StreamWorker::StreamWorker(StreamWorker&& other) noexcept = default;
StreamWorker& StreamWorker::operator=(StreamWorker&& other) noexcept = default;
StreamWorker::~StreamWorker() = default;

PacketQueue& StreamWorker::queue() noexcept {
    return *state->packetQueue;
}

void StreamWorker::stop() noexcept {
    state->stopping.store(true);
    state->packetQueue->close();
}

void StreamWorker::run() noexcept {
    State& worker{*state};
    std::optional<ChildProcess> child{ChildProcess::spawn(worker.config.command, worker.log)};
    if (!child) {
        worker.giveUp("the publishing program did not start");
        return;
    }
    const WriteLimits limits{std::chrono::milliseconds{worker.config.stallMs}, &worker.stopping};
    QueuedPacket packet;
    packet.bytes.reserve(worker.config.maxPacketBytes);
    while (!worker.stopping.load()) {
        const PopResult result{worker.packetQueue->popFor(packet, idleStep)};
        if (result == PopResult::Closed) {
            break;
        }
        if (result == PopResult::Timeout) {
            if (const std::optional<int> ended{child->pollExit()}; ended) {
                worker.giveUp("the publishing program ended (status " + std::to_string(*ended) +
                              ")");
                break;
            }
            continue;
        }
        const WriteResult written{child->write(packet.bytes, limits)};
        if (written == WriteResult::Done) {
            worker.counts.packetsSent.fetch_add(1);
            worker.counts.bytesSent.fetch_add(static_cast<std::int64_t>(packet.bytes.size()));
        } else if (written == WriteResult::Broken) {
            worker.giveUp("the publishing program ended (status " +
                          std::to_string(child->finish(endGrace)) + ")");
            break;
        } else if (written == WriteResult::Stalled) {
            worker.giveUp("the publishing program does not read");
            break;
        } else {
            break;
        }
    }
    (void)child->finish(endGrace);
}

const StreamCounts& StreamWorker::counts() const noexcept {
    return state->counts;
}
