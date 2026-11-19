// The output branch and the evidence thread on real camera frames with made-up detections: every
// frame is copied upright, boxes and labels are drawn, the video is encoded, and at the given
// frames a violation of a made-up person happens, so that its JPEGs and its clip are written to
// the output folder. tools/check/check_output.py checks the files.
// Usage: probe_output <node> <width> <height> <rotation> <output_dir> <frames> <event_frames>
//                     [max_storage_gb [keep_days [record_dir]]]
//   node          : camera capture node, for example /dev/video11
//   width, height : frame size in pixels, for example 3840 2160
//   rotation      : 0, 90, 180 or 270 degrees clockwise
//   output_dir    : an existing folder, for example /data/violations
//   frames        : frames to process, 2 or more
//   event_frames  : comma separated frame numbers (0 based) at which a violation is reported
//   max_storage_gb, keep_days : retention limits (defaults 4 and 30), to see old violations go
//   record_dir    : also record the video continuously there (10 s segments, 0.3 GB), to see that
//                   violations and clips still save while the recording thread writes
// Logs the stages, the violations and clips saved and removed. Exit code 0 when all of them were
// saved, 2 on bad arguments.
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "app_options.h"
#include "arguments.h"
#include "camera.h"
#include "evidence_worker.h"
#include "frame_data.h"
#include "logger.h"
#include "output_branch.h"
#include "record_worker.h"
#include "rga.h"

namespace {
constexpr int exitUsage{2};
constexpr int decimalBase{10};
constexpr long maxDimension{16'384};
constexpr long maxFrames{100'000};
constexpr std::size_t nodeArg{1};
constexpr std::size_t widthArg{2};
constexpr std::size_t heightArg{3};
constexpr std::size_t rotationArg{4};
constexpr std::size_t outputArg{5};
constexpr std::size_t framesArg{6};
constexpr std::size_t eventsArg{7};
constexpr std::size_t argsCount{8};
constexpr std::size_t storageArg{8};
constexpr std::size_t daysArg{9};
constexpr std::size_t recordArg{10};
constexpr std::size_t maxArgsCount{11};
constexpr double recordSegment{10.0};
constexpr double recordStorageGb{0.3};
constexpr int warmupFrames{5};
constexpr float personScore{0.9F};
constexpr float headScore{0.8F};
constexpr float quarter{4.0F};
constexpr float half{2.0F};
constexpr float tenth{10.0F};
constexpr int helmetlessTrack{7};
constexpr int otherTrack{3};

struct Options {
    const char* node{nullptr};
    int width{0};
    int height{0};
    Rotation rotation{Rotation::None};
    const char* outputDir{nullptr};
    int frames{0};
    std::vector<int> events;
    double maxStorageGb{defaultMaxStorageGb};
    int keepDays{defaultKeepDays};
    const char* recordDir{nullptr};
};

std::optional<long> parseNumber(const std::string& text, long low, long high) {
    char* end{nullptr};
    const long value{std::strtol(text.c_str(), &end, decimalBase)};
    if (end == text.c_str() || *end != '\0' || value < low || value > high) {
        return std::nullopt;
    }
    return value;
}

bool parseList(const char* text, std::vector<int>& list) {
    const std::string all{text};
    std::size_t start{0};
    while (start <= all.size()) {
        const std::size_t comma{all.find(',', start)};
        const std::string item{
            all.substr(start, comma == std::string::npos ? comma : comma - start)};
        const std::optional<long> number{parseNumber(item, 0, maxFrames)};
        if (!number) {
            return false;
        }
        list.push_back(static_cast<int>(*number));
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    return true;
}

std::optional<Options> parseArgs(const std::vector<const char*>& args, const Logger& log) {
    if (args.size() < argsCount || args.size() > maxArgsCount) {
        log.error("usage: ", args.empty() ? "probe_output" : args.at(0),
                  " <node> <width> <height> <rotation> <output_dir> <frames> <event_frames>"
                  " [max_storage_gb [keep_days]]");
        return std::nullopt;
    }
    Options options;
    options.node = args.at(nodeArg);
    options.outputDir = args.at(outputArg);
    const std::optional<long> width{parseNumber(args.at(widthArg), 2, maxDimension)};
    const std::optional<long> height{parseNumber(args.at(heightArg), 2, maxDimension)};
    const std::optional<Rotation> rotation{parseRotation(args.at(rotationArg))};
    const std::optional<long> frames{parseNumber(args.at(framesArg), 2, maxFrames)};
    if (!width || !height || !rotation || !frames ||
        !parseList(args.at(eventsArg), options.events)) {
        log.error("bad size, rotation, frames or event frames");
        return std::nullopt;
    }
    options.width = static_cast<int>(*width);
    options.height = static_cast<int>(*height);
    options.rotation = *rotation;
    options.frames = static_cast<int>(*frames);
    if (args.size() > storageArg) {
        char* end{nullptr};
        options.maxStorageGb = std::strtod(args.at(storageArg), &end);
        if (end == args.at(storageArg) || *end != '\0' || options.maxStorageGb <= 0.0) {
            log.error("bad max_storage_gb");
            return std::nullopt;
        }
    }
    if (args.size() > daysArg) {
        const std::optional<long> days{parseNumber(args.at(daysArg), 1, maxKeepDays)};
        if (!days) {
            log.error("bad keep_days");
            return std::nullopt;
        }
        options.keepDays = static_cast<int>(*days);
    }
    if (args.size() > recordArg) {
        options.recordDir = args.at(recordArg);
    }
    return options;
}

// Waits for a frame and takes it out of the camera.
std::optional<Frame> nextFrame(const Camera& camera, const StopSignal& stop) {
    while (true) {
        if (camera.waitFrame(stop) != WaitStatus::Ready) {
            return std::nullopt;
        }
        const Dequeued dequeued{camera.dequeue()};
        if (dequeued.status == DequeueStatus::Failed) {
            return std::nullopt;
        }
        if (dequeued.status == DequeueStatus::Frame) {
            return dequeued.frame;
        }
    }
}

// Two made-up persons and a head box on each, one of them with a violation reported at this
// frame when `event` is set.
void makeData(FrameData& data, const ImageSize& size, bool event, double seconds) {
    data.detections.clear();
    data.tracks.clear();
    data.events.clear();
    data.violators.clear();
    const auto width{static_cast<float>(size.width)};
    const auto height{static_cast<float>(size.height)};
    const TrackedObject violator{width / quarter,
                                 height / quarter,
                                 width / half,
                                 height - (height / tenth),
                                 helmetlessTrack,
                                 personScore,
                                 4,
                                 0};
    const TrackedObject other{width / half + (width / tenth),
                              height / quarter,
                              width - (width / quarter),
                              height - (height / tenth),
                              otherTrack,
                              personScore,
                              4,
                              1};
    data.tracks.push_back(violator);
    data.tracks.push_back(other);
    data.detections.push_back(Detection{violator.left + (width / tenth), violator.top,
                                        violator.right - (width / tenth),
                                        violator.top + (height / quarter), headScore, 1});
    data.detections.push_back(Detection{other.left + (width / tenth), other.top,
                                        other.right - (width / tenth),
                                        other.top + (height / quarter), headScore, 0});
    data.violators.push_back(helmetlessTrack);
    if (event) {
        ViolationEvent report;
        report.trackId = helmetlessTrack;
        report.noHelmet = true;
        report.startSeconds = seconds - defaultHoldSeconds;
        report.nowSeconds = seconds;
        report.left = violator.left;
        report.top = violator.top;
        report.right = violator.right;
        report.bottom = violator.bottom;
        data.events.push_back(report);
    }
}

bool run(const Camera& camera, const StopSignal& stop, const Options& options, const Logger& log) {
    AppOptions app;
    app.rotation = options.rotation;
    app.outputDir = options.outputDir;
    app.maxStorageGb = options.maxStorageGb;
    app.keepDays = options.keepDays;
    std::optional<RecordWorker> record;
    if (options.recordDir != nullptr) {
        app.record = true;
        app.recordDir = options.recordDir;
        app.recordSegmentSeconds = recordSegment;
        app.recordMaxStorageGb = recordStorageGb;
        record = RecordWorker::create(app, log);
        if (!record) {
            return false;
        }
    }
    const PacketSinks sinks{record ? &record->queue() : nullptr, nullptr};
    std::optional<OutputBranch> output{OutputBranch::create(app, camera.format(), sinks, log)};
    if (!output) {
        return false;
    }
    std::optional<EvidenceWorker> evidence{EvidenceWorker::create(
        app, output->imageSize(), output->imageStride(), camera.format().fullRange, log)};
    std::vector<RgaImage> cameraImages;
    for (std::size_t index{0}; index < camera.bufferCount(); ++index) {
        std::optional<RgaImage> image{RgaImage::import(camera.planeBuffer(index, 0), log)};
        if (!image) {
            return false;
        }
        cameraImages.push_back(std::move(*image));
    }
    if (!evidence) {
        return false;
    }
    for (int skipped{0}; skipped < warmupFrames; ++skipped) {
        const std::optional<Frame> frame{nextFrame(camera, stop)};
        if (!frame || !camera.queue(frame->index)) {
            return false;
        }
    }
    std::thread evidenceThread{[&] { evidence->run(output->hub()); }};
    std::thread recordThread;
    if (record) {
        recordThread = std::thread{[&] { record->run(); }};
    }
    FrameData data;
    bool succeeded{true};
    std::vector<double> encodeTimes;
    for (int number{0}; number < options.frames && succeeded; ++number) {
        const std::optional<Frame> frame{nextFrame(camera, stop)};
        if (!frame) {
            succeeded = false;
            break;
        }
        succeeded = output->copy(cameraImages.at(frame->index)) && camera.queue(frame->index);
        const bool event{std::find(options.events.begin(), options.events.end(), number) !=
                         options.events.end()};
        makeData(data, output->imageSize(), event, frame->captureSeconds);
        BranchTimes times;
        succeeded = succeeded && output->encode(data, frame->captureSeconds, times);
        encodeTimes.push_back(times.encode);
    }
    output->finish();
    // Gives the evidence thread time to finish the queue, then ends it.
    output->hub().queue.close();
    if (record) {
        record->queue().close();
        recordThread.join();
    }
    evidenceThread.join();
    const BranchCounts counts{output->counts()};
    const EvidenceCounts& saved{evidence->counts()};
    log.info("frames ", counts.frames, ", violations saved ", saved.violationsSaved.load(),
             " failed ", saved.violationsFailed.load(), ", clips saved ", saved.clipsSaved.load(),
             " failed ", saved.clipsFailed.load(), ", refused ", saved.violationsRefused.load(),
             ", old removed ", saved.violationsRemoved.load(), ", evidence dropped ",
             counts.evidenceDropped, ", clips skipped ", counts.clipsSkipped);
    if (record) {
        log.info("recording: segments ", record->counts().segmentsSaved.load(), ", bytes ",
                 record->counts().bytesSaved.load(), ", write errors ",
                 record->counts().writeErrors.load(), ", removed ",
                 record->counts().segmentsRemoved.load(), ", queue overflows ",
                 record->queue().overflowCount(), ", dropped ", record->queue().droppedCount());
    }
    const auto expected{static_cast<std::int64_t>(options.events.size())};
    return succeeded && saved.violationsSaved.load() == expected &&
           saved.clipsSaved.load() == expected && saved.violationsFailed.load() == 0 &&
           saved.clipsFailed.load() == 0;
}
} // namespace

int main(int argc, char** argv) {
    const std::vector<const char*> args{commandLine(argc, argv)};
    const Logger log{"PROBE_OUTPUT", LogLevel::Info};
    const std::optional<Options> options{parseArgs(args, log)};
    if (!options) {
        return exitUsage;
    }
    bool passed{false};
    const std::optional<StopSignal> stop{StopSignal::create(log)};
    if (stop) {
        std::optional<Camera> camera{Camera::open(
            CameraConfig{options->node, options->width, options->height, defaultHeap}, log)};
        if (camera) {
            passed = run(*camera, *stop, *options, log);
            passed = camera->close() && passed;
        }
    }
    if (passed) {
        log.info("PASS");
    } else {
        log.error("FAIL");
    }
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
