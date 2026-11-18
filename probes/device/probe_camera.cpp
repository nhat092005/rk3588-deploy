// V4L2 capture check for one video node (multi-planar, MMAP).
// Usage: probe_camera <node> [width height] [frames] [out.raw]
//   node    : capture node, e.g. /dev/videoN (name and format come from the board, not guessed)
//   width   : requested width, default 3840 (give width and height together)
//   height  : requested height, default 2160
//   frames  : number of frames to dequeue (default 90)
//   out.raw : write plane data of the first frame
// Format is NV12. The ISP is cropped, centred, to the aspect ratio of width:height (4224x2376 for
// 16:9) before the format is set, and the original crop is restored on exit.
// Exit code 0 only if every stage passes, no frame is dropped (gap in sequence),
// flagged with an error, or shorter than sizeimage. A frame rate below 90% of 30 FPS is a warning.
// Ctrl-C stops the capture, prints the summary and counts as FAIL. The log ends with PASS or FAIL.
// Environment PROBE_CAM_VERBOSE=1 logs every frame.
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <vector>

#include "arguments.h"
#include "camera.h"
#include "logger.h"

namespace {
constexpr int defaultWidth{3840};
constexpr int defaultHeight{2160};
constexpr std::int64_t defaultFrames{90};
constexpr std::int64_t progressInterval{30};
constexpr double expectedFps{30.0};
constexpr double fpsWarnRatio{0.9};
constexpr double percentScale{100.0};
constexpr int fpsPrecision{2};
constexpr int secondsPrecision{3};
constexpr double millisecondsPerSecond{1000.0};
constexpr int wholePrecision{0};
constexpr const char* verboseEnv{"PROBE_CAM_VERBOSE"};
constexpr int decimalBase{10};
// Positions of the command line arguments.
constexpr std::size_t nodeArg{1};
constexpr std::size_t widthArg{2};
constexpr std::size_t heightArg{3};
constexpr std::size_t framesArg{4};
constexpr std::size_t outPathArg{5};

struct Options {
    const char* node{nullptr};
    int width{defaultWidth};
    int height{defaultHeight};
    std::int64_t frames{defaultFrames};
    const char* outPath{nullptr}; // nullptr: no dump
};

struct CaptureStats {
    std::int64_t received{0};
    std::int64_t dropped{0};
    std::int64_t flagged{0};
    std::int64_t shortFrames{0};
    // Capture timestamps (Frame::captureSeconds): frames without one, steps that do not go
    // forward, and the shortest and longest step.
    std::int64_t noTimestamp{0};
    std::int64_t notForward{0};
    double minStep{std::numeric_limits<double>::max()};
    double maxStep{0.0};
};

// A positive decimal number that fits `limit`. Rejects empty text, trailing junk and zero.
std::optional<std::int64_t> parsePositive(const char* text, std::int64_t limit) noexcept {
    char* end{nullptr};
    errno = 0;
    const long long value{std::strtoll(text, &end, decimalBase)};
    if (errno != 0 || end == text || *end != '\0' || value <= 0 || value > limit) {
        return std::nullopt;
    }
    return value;
}

// Layout: program, node, then optionally width and height together, frames, out.raw.
std::optional<Options> parseArgs(const std::vector<const char*>& args, const Logger& log) {
    if (args.size() <= nodeArg || args.size() == heightArg) {
        log.error("usage: ", args.empty() ? "probe_camera" : args.at(0),
                  " <node> [width height] [frames] [out.raw]");
        return std::nullopt;
    }
    Options options{};
    options.node = args.at(nodeArg);
    if (args.size() > heightArg) {
        const std::int64_t sizeLimit{std::numeric_limits<int>::max()};
        const std::optional<std::int64_t> width{parsePositive(args.at(widthArg), sizeLimit)};
        const std::optional<std::int64_t> height{parsePositive(args.at(heightArg), sizeLimit)};
        if (!width || !height) {
            log.error("bad size: ", args.at(widthArg), " ", args.at(heightArg));
            return std::nullopt;
        }
        options.width = static_cast<int>(*width);
        options.height = static_cast<int>(*height);
    }
    if (args.size() > framesArg) {
        const std::optional<std::int64_t> frames{
            parsePositive(args.at(framesArg), std::numeric_limits<std::int64_t>::max())};
        if (!frames) {
            log.error("bad frames: ", args.at(framesArg));
            return std::nullopt;
        }
        options.frames = *frames;
    }
    if (args.size() > outPathArg) {
        options.outPath = args.at(outPathArg);
    }
    return options;
}

bool isClean(const CaptureStats& stats) noexcept {
    return stats.dropped == 0 && stats.flagged == 0 && stats.shortFrames == 0 &&
           stats.noTimestamp == 0 && stats.notForward == 0;
}

// Per-frame checks: sequence gap (dropped), error flag, bytes used against sizeimage. The first
// frame has no previous sequence to compare with.
void inspectFrame(const Camera& camera, const Frame& frame,
                  std::optional<std::int64_t> previousSequence, double previousCapture,
                  CaptureStats& stats, const Logger& log) noexcept {
    if (frame.captureSeconds <= 0.0) {
        ++stats.noTimestamp;
    } else if (previousSequence && previousCapture > 0.0) {
        const double step{frame.captureSeconds - previousCapture};
        stats.notForward += step > 0.0 ? 0 : 1;
        stats.minStep = std::min(stats.minStep, step);
        stats.maxStep = std::max(stats.maxStep, step);
    }
    if (previousSequence && frame.sequence > *previousSequence + 1) {
        const std::int64_t gap{frame.sequence - *previousSequence - 1};
        stats.dropped += gap;
        log.warn("dropped ", gap, " frame(s) before sequence=", frame.sequence);
    }
    if (frame.flaggedError) {
        ++stats.flagged;
        log.warn("frame sequence=", frame.sequence, " has V4L2_BUF_FLAG_ERROR");
    }
    bool shortFrame{false};
    for (std::size_t plane{0}; plane < camera.planeCount(); ++plane) {
        if (frame.bytesUsed.at(plane) != camera.planeSize(plane)) {
            log.warn("frame sequence=", frame.sequence, " plane ", plane,
                     " bytesused=", frame.bytesUsed.at(plane),
                     ", sizeimage=", camera.planeSize(plane));
            shortFrame = true;
        }
    }
    if (shortFrame) {
        ++stats.shortFrames;
    }
    log.verbose("frame ", stats.received, ": index=", frame.index, " sequence=", frame.sequence,
                " bytesused=", frame.bytesUsed.at(0));
    if ((stats.received + 1) % progressInterval == 0) {
        log.info("progress ", stats.received + 1, " frames, sequence=", frame.sequence);
    }
}

// FPS counts the intervals between frames, so it needs at least two frames.
void logSummary(const CaptureStats& stats, double seconds, const Logger& log) noexcept {
    const bool measurable{stats.received > 1 && seconds > 0.0};
    const double fps{measurable ? static_cast<double>(stats.received - 1) / seconds : 0.0};
    log.info("captured ", stats.received, " frames in ", Fixed{seconds, secondsPrecision},
             " s: ", Fixed{fps, fpsPrecision}, " FPS, dropped=", stats.dropped,
             ", flagged=", stats.flagged, ", short=", stats.shortFrames);
    log.info("capture timestamps: missing=", stats.noTimestamp, ", not forward=", stats.notForward,
             ", step min ", Fixed{stats.minStep * millisecondsPerSecond, fpsPrecision}, " ms, max ",
             Fixed{stats.maxStep * millisecondsPerSecond, fpsPrecision}, " ms");
    if (stats.received > 1 && fps < expectedFps * fpsWarnRatio) {
        log.warn("FPS ", Fixed{fps, fpsPrecision}, " is below ",
                 Fixed{fpsWarnRatio * percentScale, wholePrecision}, "% of ",
                 Fixed{expectedFps, wholePrecision}, " FPS");
    }
}

// Dequeues `options.frames` frames and queues them back. Checks every frame, dumps the first
// one when an output path is given. The summary is logged on every exit path of the loop.
bool runCapture(const Camera& camera, const StopSignal& stop, const Options& options,
                const Logger& log) {
    CaptureStats stats{};
    bool failed{false};
    bool dumpPending{options.outPath != nullptr};
    std::optional<std::int64_t> previousSequence{};
    double previousCapture{0.0};
    std::chrono::steady_clock::time_point firstTime{};
    std::chrono::steady_clock::time_point lastTime{};
    while (!failed && stats.received < options.frames) {
        const WaitStatus waited{camera.waitFrame(stop)};
        if (waited == WaitStatus::Timeout) {
            log.error("no frame after ", stats.received, ": poll timeout");
        } else if (waited == WaitStatus::Stopped) {
            log.warn("stopped by signal after ", stats.received, " frames");
        }
        if (waited != WaitStatus::Ready) {
            failed = true;
            continue;
        }

        const Dequeued dequeued{camera.dequeue()};
        if (dequeued.status != DequeueStatus::Frame) {
            failed = dequeued.status == DequeueStatus::Failed;
            continue;
        }
        const Frame& frame{dequeued.frame};

        lastTime = std::chrono::steady_clock::now();
        if (stats.received == 0) {
            firstTime = lastTime;
            log.info("first frame: index=", frame.index, " sequence=", frame.sequence,
                     " bytesused=", frame.bytesUsed.at(0));
        }
        inspectFrame(camera, frame, previousSequence, previousCapture, stats, log);
        previousSequence = frame.sequence;
        previousCapture = frame.captureSeconds;

        if (dumpPending) {
            dumpPending = false;
            if (!camera.writeFrame(frame, options.outPath)) {
                ++stats.flagged;
            }
        }
        failed = !camera.queue(frame.index);
        ++stats.received;
    }

    const std::chrono::duration<double> elapsed{lastTime - firstTime};
    logSummary(stats, elapsed.count(), log);
    return !failed && isClean(stats);
}
} // namespace

int main(int argc, char** argv) {
    const std::vector<const char*> args{commandLine(argc, argv)};
    const Logger log{"PROBE_CAM",
                     std::getenv(verboseEnv) != nullptr ? LogLevel::Verbose : LogLevel::Debug};
    const std::optional<Options> options{parseArgs(args, log)};
    if (!options) {
        return EXIT_FAILURE;
    }

    bool passed{false};
    const std::optional<StopSignal> stop{StopSignal::create(log)};
    if (stop) {
        std::optional<Camera> camera{
            Camera::open(CameraConfig{options->node, options->width, options->height}, log)};
        if (camera) {
            passed = runCapture(*camera, *stop, *options, log);
            const bool closed{camera->close()};
            log.info("cleanup ", closed ? "done" : "had errors");
            passed = passed && closed;
        }
    }

    if (passed) {
        log.info("PASS");
    } else {
        log.error("FAIL");
    }
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
