// FileSource check on a video file: opens it, takes every frame as the pipeline does and gives the
// buffer back after `hold` more frames, and prints what came out.
// Usage: probe_file_source <file> [hold] [consumer_ms]
//   hold        : frames kept before their buffer is given back (the pipeline holds up to 6),
//   default 0 consumer_ms : time that the consumer spends on each frame, default 0 (a slow
//   pipeline)
// Prints the format, the frames, the sequence gaps, the error flags, the time between frames
// (median and largest), the wall time against the pts span, and the mean luma of every 30th frame.
// Exit code 0 when the video ended (End), 1 on a failure or a stop.
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <thread>
#include <vector>

#include "file_source.h"
#include "handles.h"
#include "logger.h"

namespace {
constexpr std::size_t fileArg{1};
constexpr std::size_t holdArg{2};
constexpr std::size_t consumerArg{3};
constexpr std::int64_t lumaEvery{30};
constexpr std::size_t lumaStep{8};
constexpr double millisecondsPerSecond{1000.0};

// Mean of the luma plane at a sparse grid: the CPU reads the buffer between begin and end.
double meanLuma(const DmaBuf& buffer, const CameraFormat& format) {
    if (!buffer.beginCpuAccess()) {
        return -1.0;
    }
    const auto* bytes{static_cast<const std::uint8_t*>(buffer.data())};
    double sum{0.0};
    std::size_t count{0};
    for (int row{0}; row < format.height; row += static_cast<int>(lumaStep)) {
        for (int column{0}; column < format.width; column += static_cast<int>(lumaStep)) {
            sum += bytes[static_cast<std::size_t>(row) *
                             static_cast<std::size_t>(format.bytesPerLine) +
                         static_cast<std::size_t>(column)];
            ++count;
        }
    }
    (void)buffer.endCpuAccess();
    return sum / static_cast<double>(count);
}
} // namespace

int main(int argc, char** argv) {
    const Logger log{"PROBE_FILE", LogLevel::Info};
    if (argc <= static_cast<int>(fileArg)) {
        log.error("usage: probe_file_source <file> [hold] [consumer_ms]");
        return EXIT_FAILURE;
    }
    const std::size_t hold{
        argc > static_cast<int>(holdArg) ? static_cast<std::size_t>(std::atoi(argv[holdArg])) : 0};
    const int consumerMs{argc > static_cast<int>(consumerArg) ? std::atoi(argv[consumerArg]) : 0};
    const std::optional<StopSignal> stop{StopSignal::create(log)};
    if (!stop) {
        return EXIT_FAILURE;
    }
    const auto begin{std::chrono::steady_clock::now()};
    const std::unique_ptr<FileSource> source{FileSource::open(argv[fileArg], log)};
    if (!source) {
        log.error("FAIL: open");
        return EXIT_FAILURE;
    }
    const double openSeconds{
        std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count()};
    const CameraFormat& format{source->format()};
    log.info("open took ", openSeconds, " s: ", format.width, "x", format.height, " stride ",
             format.bytesPerLine, "x", format.verticalStride, " range ",
             format.fullRange ? "full" : "limited", " matrix ",
             format.matrix == ColorMatrix::Bt709 ? "BT.709" : "BT.601");
    std::deque<std::size_t> held;
    std::vector<double> gaps;
    std::int64_t frames{0};
    std::int64_t errors{0};
    std::int64_t missing{0};
    std::optional<std::int64_t> lastSequence;
    double firstSeconds{0.0};
    double lastSeconds{0.0};
    bool ended{false};
    const auto started{std::chrono::steady_clock::now()};
    while (true) {
        const WaitStatus waited{source->waitFrame(*stop)};
        if (waited == WaitStatus::End) {
            ended = true;
            break;
        }
        if (waited == WaitStatus::Timeout) {
            continue;
        }
        if (waited != WaitStatus::Ready) {
            log.error("waitFrame: ", static_cast<int>(waited));
            break;
        }
        const Dequeued dequeued{source->dequeue()};
        if (dequeued.status != DequeueStatus::Frame) {
            continue;
        }
        const Frame& frame{dequeued.frame};
        if (lastSequence && frame.sequence != *lastSequence + 1) {
            missing += frame.sequence - *lastSequence - 1;
        }
        lastSequence = frame.sequence;
        if (frames == 0) {
            firstSeconds = frame.captureSeconds;
        } else {
            gaps.push_back((frame.captureSeconds - lastSeconds) * millisecondsPerSecond);
        }
        lastSeconds = frame.captureSeconds;
        errors += frame.flaggedError ? 1 : 0;
        if (frames % lumaEvery == 0) {
            log.info("frame ", frames, " index ", frame.index, " mean luma ",
                     meanLuma(source->planeBuffer(frame.index, 0), format));
        }
        ++frames;
        held.push_back(frame.index);
        if (held.size() > hold) {
            if (!source->queue(held.front())) {
                log.error("queue failed");
                break;
            }
            held.pop_front();
        }
        if (consumerMs > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds{consumerMs});
        }
    }
    for (const std::size_t index : held) {
        (void)source->queue(index);
    }
    const double wall{
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count()};
    std::sort(gaps.begin(), gaps.end());
    log.info("frames ", frames, ", sequence gaps ", missing, ", error flags ", errors,
             ", span of the stamps ", lastSeconds - firstSeconds, " s, wall ", wall,
             " s, between frames ms median ", gaps.empty() ? 0.0 : gaps.at(gaps.size() / 2),
             " max ", gaps.empty() ? 0.0 : gaps.back());
    const bool closed{source->close()};
    log.info(ended && closed ? "PASS" : "FAIL", ": ended ", ended, ", closed ", closed);
    return ended && closed ? EXIT_SUCCESS : EXIT_FAILURE;
}
