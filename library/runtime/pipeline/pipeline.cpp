#include "pipeline.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "byte_tracker.h"
#include "camera.h"
#include "cpu_affinity.h"
#include "dmabuf.h"
#include "evidence_worker.h"
#include "frame_data.h"
#include "mailbox.h"
#include "npu_infer.h"
#include "output_branch.h"
#include "postprocess.h"
#include "record_worker.h"
#include "stream_worker.h"
#include "rga.h"
#include "stage_stats.h"
#include "violation_retention.h"
#include "violation_rule.h"
#include "volume.h"

namespace {
using Clock = std::chrono::steady_clock;

// What the model is: five classes, 8400 anchors for a 640 input, boxes divided by the input size
// (the INT8 Ultralytics export). A model that differs is refused by the first postprocess.
constexpr std::size_t modelClassCount{5};
constexpr std::size_t modelAnchorCount{8400};
constexpr int rgbChannels{3};
// The postprocess keeps low confidence boxes for the tracker. The violation rule filters its own.
constexpr float detectionConfidence{0.1F};
constexpr std::size_t statsCapacity{4096};
constexpr double millisecondsPerSecond{1000.0};
constexpr double cameraFrameMs{1000.0 / 30.0};
constexpr int timePrecision{2};
constexpr int fpsPrecision{1};
// Model inputs: the camera thread fills one while the NPU thread reads the other.
constexpr std::size_t inputCount{2};
// Camera buffers: a frame can be held by the camera thread, the first mailbox, the NPU thread, the
// second mailbox and the writer thread at once (5), and the driver needs one to fill: 6.
constexpr std::size_t pipelineCameraBuffers{6};
// A writer frame slower than this is logged with where the time went.
constexpr double slowFrameMs{50.0};
// The big cores of the RK3588 (A76), one per thread (measured in
// probes/pipeline/probe_threads.cpp).
constexpr int cameraCpu{6};
constexpr int npuCpu{5};
constexpr int writerCpu{4};
constexpr int evidenceCpu{7};
// The recording thread mostly waits for the disk, so a small core will do.
constexpr int recordCpu{3};
// The stream thread waits for the network, and the ffmpeg it starts inherits this core: a small
// one, apart from the five threads above and the recording.
constexpr int streamCpu{2};

double millisecondsBetween(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

void pinCurrentThread(int cpu, const char* name, const Logger& log) {
    if (!pinThreadToCpu(cpu)) {
        log.warn("could not pin the ", name, " thread to CPU ", cpu);
    }
}

// A model input: the buffer the letterbox fills, its RGA image and its NPU id.
struct Input {
    DmaBuf buffer;
    RgaImage image;
    InputId inputId;
};

// Counts of the whole run, written by several threads.
struct Counters {
    std::atomic<std::int64_t> frames{0};
    std::atomic<std::int64_t> badFrames{0};
    std::atomic<std::int64_t> droppedCamera{0};
    std::atomic<std::int64_t> skippedInference{0};
    std::atomic<std::int64_t> droppedOutput{0};
    std::atomic<std::int64_t> violations{0};
    std::atomic<std::int64_t> lostViolations{0};
};

// The pieces made by create(), moved into the state.
struct Parts {
    Camera camera;
    RgaLetterbox letterbox;
    std::vector<RgaImage> cameraImages;
    std::vector<Input> inputs;
    NpuInfer npu;
    Postprocessor postprocessor;
    ByteTracker tracker;
    ViolationRule rule;
    SourceMapping mapping;
    std::optional<RecordWorker> record;
    std::optional<StreamWorker> stream;
    OutputBranch output;
    EvidenceWorker evidence;
};
} // namespace

// Members that threads touch are declared after the ones they use, so that the destruction order is
// the safe one.
struct PipelineState {
    PipelineState(const AppOptions& config, Logger logger, Parts parts)
        : options{config}, log{logger}, camera{std::move(parts.camera)},
          letterbox{std::move(parts.letterbox)}, cameraImages{std::move(parts.cameraImages)},
          inputs{std::move(parts.inputs)}, npu{std::move(parts.npu)},
          postprocessor{std::move(parts.postprocessor)}, tracker{std::move(parts.tracker)},
          rule{std::move(parts.rule)}, mapping{parts.mapping}, record{std::move(parts.record)},
          stream{std::move(parts.stream)}, output{std::move(parts.output)},
          evidence{std::move(parts.evidence)} {
        persons.reserve(maxDetections);
    }
    PipelineState(const PipelineState&) = delete;
    PipelineState& operator=(const PipelineState&) = delete;
    PipelineState(PipelineState&&) = delete;
    PipelineState& operator=(PipelineState&&) = delete;
    ~PipelineState() = default;

    AppOptions options;
    Logger log;
    Camera camera;
    RgaLetterbox letterbox;
    std::vector<RgaImage> cameraImages;
    std::vector<Input> inputs;
    NpuInfer npu;
    Postprocessor postprocessor;
    ByteTracker tracker;
    ViolationRule rule;
    SourceMapping mapping;
    // Declared before the output branch, which keeps a pointer to their queues.
    std::optional<RecordWorker> record;
    std::optional<StreamWorker> stream;
    OutputBranch output;
    EvidenceWorker evidence;
    std::vector<Detection> persons;
    std::array<FrameData, frameDataCount> frames;
    std::array<std::atomic<bool>, inputCount> inputBusy{};
    Mailbox<InferJob> toNpu;
    Mailbox<OutputJob> toWriter;
    Counters counters;
    std::atomic<bool> failed{false};
};

namespace {
std::optional<std::vector<RgaImage>> importCameraBuffers(const Camera& camera, const Logger& log) {
    std::vector<RgaImage> images;
    images.reserve(camera.bufferCount());
    for (std::size_t index{0}; index < camera.bufferCount(); ++index) {
        std::optional<RgaImage> image{RgaImage::import(camera.planeBuffer(index, 0), log)};
        if (!image) {
            return std::nullopt;
        }
        images.push_back(std::move(*image));
    }
    return images;
}

// The tracker, the rule and the decoder, which need no device.
struct Logic {
    Postprocessor postprocessor;
    ByteTracker tracker;
    ViolationRule rule;
};

std::optional<Logic> makeLogic(const AppOptions& options, const Logger& log) {
    PostprocessConfig post;
    post.classCount = modelClassCount;
    post.anchorCount = modelAnchorCount;
    post.modelSize = defaultModelSize;
    post.normalizedBoxes = true;
    post.confThreshold = detectionConfidence;
    ViolationConfig ruleConfig;
    ruleConfig.holdSeconds = options.holdSeconds;
    ruleConfig.gapSeconds = options.gapSeconds;
    ruleConfig.minConfidence = options.minConfidence;
    std::optional<Postprocessor> decoder{Postprocessor::create(post, log)};
    std::optional<ByteTracker> tracker{ByteTracker::create(ByteTrackerConfig{}, log)};
    std::optional<ViolationRule> rule{ViolationRule::create(ruleConfig, log)};
    if (!decoder || !tracker || !rule) {
        return std::nullopt;
    }
    return Logic{std::move(*decoder), std::move(*tracker), std::move(*rule)};
}

// The camera and the RGA side: the frame buffers, the letterbox and the model input buffers.
struct Capture {
    Camera camera;
    RgaLetterbox letterbox;
    std::vector<RgaImage> cameraImages;
    std::vector<Input> inputs;
};

std::optional<Capture> makeCapture(const AppOptions& options, NpuInfer& npu, const Logger& log) {
    const std::string node{options.cameraNode};
    std::optional<Camera> camera{
        Camera::open(CameraConfig{node.c_str(), options.width, options.height, defaultHeap,
                                  pipelineCameraBuffers},
                     log)};
    if (!camera) {
        return std::nullopt;
    }
    const CameraFormat& format{camera->format()};
    std::optional<RgaLetterbox> letterbox{RgaLetterbox::create(
        LetterboxConfig{ImageSize{format.width, format.height}, format.bytesPerLine,
                        defaultModelSize, options.rotation, format.fullRange},
        log)};
    std::optional<std::vector<RgaImage>> images{importCameraBuffers(*camera, log)};
    if (!letterbox || !images) {
        return std::nullopt;
    }
    std::vector<Input> inputs;
    inputs.reserve(inputCount);
    for (std::size_t index{0}; index < inputCount; ++index) {
        // fillPadding is a colour fill, which only the RGA2 does, and the RGA2 maps only memory
        // below 4 GB: a buffer of the "system" heap may have a page above it (see lowHeap).
        std::optional<DmaBuf> buffer{
            DmaBuf::allocate(DmaBufConfig{lowHeap, npu.inputBytes()}, log)};
        if (!buffer) {
            return std::nullopt;
        }
        std::optional<RgaImage> image{RgaImage::import(*buffer, log)};
        const std::optional<InputId> inputId{npu.attachInput(*buffer)};
        if (!image || !inputId || !letterbox->fillPadding(*image)) {
            return std::nullopt;
        }
        inputs.push_back(Input{std::move(*buffer), std::move(*image), *inputId});
    }
    return Capture{std::move(*camera), std::move(*letterbox), std::move(*images),
                   std::move(inputs)};
}

// The stream is only a view of the video: when it cannot be set up the application goes on
// without it.
std::optional<StreamWorker> makeStream(const AppOptions& options, const Logger& log) {
    if (!options.stream) {
        return std::nullopt;
    }
    StreamConfig config;
    config.command = ffmpegPushCommand(options.streamUrl);
    config.kbps = options.videoKbps;
    config.maxPacketBytes = defaultVideoCapacity;
    std::optional<StreamWorker> stream{StreamWorker::create(config, log)};
    if (!stream) {
        log.warn("the stream is off: it could not be set up");
    }
    return stream;
}

std::optional<Parts> makeParts(const AppOptions& options, const Logger& log) {
    const std::string model{options.modelPath};
    std::optional<NpuInfer> npu{NpuInfer::create(NpuConfig{model.c_str()}, log)};
    if (!npu) {
        return std::nullopt;
    }
    constexpr auto inputSide{static_cast<std::size_t>(defaultModelSize)};
    if (npu->inputBytes() != inputSide * inputSide * rgbChannels) {
        log.error("the model input has ", npu->inputBytes(), " bytes, expected ",
                  inputSide * inputSide * rgbChannels, " (640 x 640 RGB)");
        return std::nullopt;
    }
    std::optional<Capture> capture{makeCapture(options, *npu, log)};
    std::optional<Logic> logic{makeLogic(options, log)};
    if (!capture || !logic) {
        return std::nullopt;
    }
    const CameraFormat& format{capture->camera.format()};
    std::optional<RecordWorker> record;
    if (options.archive) {
        record = RecordWorker::create(options, log);
        if (!record) {
            return std::nullopt;
        }
    }
    std::optional<StreamWorker> stream{makeStream(options, log)};
    const PacketSinks sinks{record ? &record->queue() : nullptr,
                            stream ? &stream->queue() : nullptr};
    std::optional<OutputBranch> output{OutputBranch::create(options, format, sinks, log)};
    if (!output) {
        return std::nullopt;
    }
    std::optional<EvidenceWorker> evidence{EvidenceWorker::create(
        options, output->imageSize(), output->imageStride(), format.fullRange, log)};
    if (!evidence) {
        return std::nullopt;
    }
    const LetterboxLayout& layout{capture->letterbox.layout()};
    const SourceMapping mapping{layout.gain, layout.left, layout.top, layout.source.width,
                                layout.source.height};
    return Parts{std::move(capture->camera),
                 std::move(capture->letterbox),
                 std::move(capture->cameraImages),
                 std::move(capture->inputs),
                 std::move(*npu),
                 std::move(logic->postprocessor),
                 std::move(logic->tracker),
                 std::move(logic->rule),
                 mapping,
                 std::move(record),
                 std::move(stream),
                 std::move(*output),
                 std::move(*evidence)};
}

// The period of a thread's statistics lines.
class WindowTimer {
public:
    explicit WindowTimer(double periodSeconds) : period{periodSeconds * millisecondsPerSecond} {}

    bool due(Clock::time_point now) const {
        return millisecondsBetween(start, now) >= period;
    }

    double seconds(Clock::time_point now) const {
        return millisecondsBetween(start, now) / millisecondsPerSecond;
    }

    void restart(Clock::time_point now) {
        start = now;
    }

private:
    double period;
    Clock::time_point start{Clock::now()};
};

// Median and largest of a window.
struct Pair {
    double median{0.0};
    double maximum{0.0};
};

Pair readStats(StageStats& stats) {
    return Pair{stats.median(), stats.maximum()};
}

bool giveBack(PipelineState& state, std::size_t cameraIndex) {
    return state.camera.queue(cameraIndex);
}

// Ends the run after a failure: the camera thread stops, the mailboxes refuse and wake the others.
void failRun(PipelineState& state) {
    state.failed.store(true);
    state.toNpu.close();
    state.toWriter.close();
}

// ---- Camera thread ----

struct CameraStats {
    CameraStats() : letterbox{statsCapacity} {}
    StageStats letterbox;
    std::int64_t frames{0};
};

// Waits for a frame and takes it out of the camera. Ready: `frame` is set. Stopped: the signal
// arrived. Failed otherwise.
enum class Fetch : std::uint8_t { Ready, Stopped, Failed };

Fetch fetchFrame(PipelineState& state, const StopSignal& stop,
                 std::optional<std::int64_t>& lastSequence, Frame& frame) {
    while (true) {
        const WaitStatus waited{state.camera.waitFrame(stop)};
        if (waited == WaitStatus::Stopped) {
            state.log.warn("stopped by signal");
            return Fetch::Stopped;
        }
        if (waited != WaitStatus::Ready) {
            state.log.error("no frame from the camera");
            return Fetch::Failed;
        }
        const Dequeued dequeued{state.camera.dequeue()};
        if (dequeued.status == DequeueStatus::Failed) {
            return Fetch::Failed;
        }
        if (dequeued.status == DequeueStatus::NotReady) {
            continue;
        }
        frame = dequeued.frame;
        if (lastSequence && frame.sequence > *lastSequence + 1) {
            const std::int64_t gap{frame.sequence - *lastSequence - 1};
            state.counters.droppedCamera.fetch_add(gap);
            state.log.warn("dropped ", gap, " frame(s) before sequence ", frame.sequence);
        }
        lastSequence = frame.sequence;
        return Fetch::Ready;
    }
}

// Letterboxes the frame into a free model input and hands it to the NPU thread. false on a failure.
bool handOver(PipelineState& state, const Frame& frame, std::int64_t number, CameraStats& stats) {
    const auto slot{static_cast<std::size_t>(number % static_cast<std::int64_t>(inputCount))};
    if (state.inputBusy.at(slot).load()) {
        state.counters.skippedInference.fetch_add(1);
        return giveBack(state, frame.index);
    }
    const Clock::time_point begin{Clock::now()};
    if (!state.letterbox.run(state.cameraImages.at(frame.index), state.inputs.at(slot).image)) {
        return false;
    }
    stats.letterbox.add(millisecondsBetween(begin, Clock::now()));
    state.inputBusy.at(slot).store(true);
    if (!state.toNpu.put(InferJob{frame.index, slot, frame.captureSeconds, number})) {
        state.inputBusy.at(slot).store(false);
        state.counters.skippedInference.fetch_add(1);
        return giveBack(state, frame.index);
    }
    return true;
}

void reportCamera(PipelineState& state, CameraStats& stats, const WindowTimer& window,
                  Clock::time_point now) {
    const Pair letterbox{readStats(stats.letterbox)};
    const double fps{static_cast<double>(stats.frames) / window.seconds(now)};
    state.log.info(
        "camera: fps ", Fixed{fps, fpsPrecision}, ", letterbox ms median/max ",
        Fixed{letterbox.median, timePrecision}, "/", Fixed{letterbox.maximum, timePrecision},
        " of ", Fixed{cameraFrameMs, timePrecision}, ", frames ", state.counters.frames.load(),
        ", dropped ", state.counters.droppedCamera.load(), ", skipped ",
        state.counters.skippedInference.load(), ", bad ", state.counters.badFrames.load());
    stats.letterbox.clear();
    stats.frames = 0;
}

// The camera thread: runs in the thread that called run(). Returns when the run is over.
void cameraLoop(PipelineState& state, const StopSignal& stop) {
    pinCurrentThread(cameraCpu, "camera", state.log);
    CameraStats stats;
    WindowTimer window{state.options.statsSeconds};
    std::optional<std::int64_t> lastSequence;
    std::int64_t number{0};
    while (!state.failed.load() && (state.options.frames == 0 || number < state.options.frames)) {
        Frame frame;
        const Fetch fetched{fetchFrame(state, stop, lastSequence, frame)};
        if (fetched != Fetch::Ready) {
            if (fetched == Fetch::Failed) {
                failRun(state);
            }
            break;
        }
        if (frame.flaggedError) {
            state.counters.badFrames.fetch_add(1);
            state.log.warn("frame ", frame.sequence, " has the error flag, skipped");
            if (!giveBack(state, frame.index)) {
                failRun(state);
            }
            continue;
        }
        if (frame.captureSeconds <= 0.0) {
            state.log.error("the camera gives no monotonic capture time, the rule needs it");
            failRun(state);
            break;
        }
        if (!handOver(state, frame, number, stats)) {
            failRun(state);
            break;
        }
        ++number;
        ++stats.frames;
        state.counters.frames.fetch_add(1);
        const Clock::time_point now{Clock::now()};
        if (window.due(now)) {
            reportCamera(state, stats, window, now);
            window.restart(now);
        }
    }
    state.toNpu.close();
}

// ---- NPU thread ----

struct NpuStats {
    NpuStats() : npu{statsCapacity}, logic{statsCapacity} {}
    StageStats npu;
    StageStats logic;
    std::size_t persons{0};
};

void logEvents(const PipelineState& state, const std::vector<ViolationEvent>& events) {
    for (const ViolationEvent& event : events) {
        state.log.warn("violation track ", event.trackId, " noHelmet ", event.noHelmet, " noVest ",
                       event.noVest, " from ", Fixed{event.startSeconds, timePrecision}, " s to ",
                       Fixed{event.nowSeconds, timePrecision}, " s");
    }
}

// A FrameData that the writer thread is not using, or null.
FrameData* freeFrameData(PipelineState& state, std::size_t& index) {
    for (std::size_t at{0}; at < state.frames.size(); ++at) {
        if (!state.frames.at(at).busy.load(std::memory_order_acquire)) {
            index = at;
            return &state.frames.at(at);
        }
    }
    return nullptr;
}

// Copies the results of the frame into `data`.
void fillFrameData(PipelineState& state, FrameData& data, const std::vector<TrackedObject>& tracked,
                   const std::vector<ViolationEvent>& events) {
    const std::vector<Detection>& detections{state.postprocessor.detections()};
    data.detections.assign(detections.begin(), detections.end());
    data.tracks.assign(tracked.begin(), tracked.end());
    data.events.assign(events.begin(), events.end());
    data.violators.clear();
    for (const TrackedObject& track : tracked) {
        if (state.rule.isViolating(track.id)) {
            data.violators.push_back(track.id);
        }
    }
}

// Decode, track and apply the rule to the frame the NPU just processed, then hand the results to
// the writer thread. false on a failure.
bool analyseFrame(PipelineState& state, const InferJob& job, NpuStats& stats) {
    const Clock::time_point begin{Clock::now()};
    if (!state.postprocessor.process(state.npu.output(0), state.mapping)) {
        return false;
    }
    const std::vector<Detection>& detections{state.postprocessor.detections()};
    selectClass(detections, defaultPersonClass, state.persons);
    const std::vector<TrackedObject>& tracked{state.tracker.update(state.persons)};
    const std::vector<ViolationEvent>& events{
        state.rule.update(job.captureSeconds, tracked, detections)};
    logEvents(state, events);
    state.counters.violations.fetch_add(static_cast<std::int64_t>(events.size()));
    stats.persons = tracked.size();
    std::size_t index{0};
    FrameData* data{freeFrameData(state, index)};
    if (data != nullptr) {
        fillFrameData(state, *data, tracked, events);
        data->busy.store(true, std::memory_order_release);
        if (state.toWriter.put(OutputJob{job.cameraIndex, index, job.captureSeconds, job.number})) {
            stats.logic.add(millisecondsBetween(begin, Clock::now()));
            return true;
        }
        data->busy.store(false, std::memory_order_release);
    }
    state.counters.droppedOutput.fetch_add(1);
    state.counters.lostViolations.fetch_add(static_cast<std::int64_t>(events.size()));
    stats.logic.add(millisecondsBetween(begin, Clock::now()));
    return giveBack(state, job.cameraIndex);
}

void reportNpu(PipelineState& state, NpuStats& stats) {
    const Pair npu{readStats(stats.npu)};
    const Pair logic{readStats(stats.logic)};
    state.log.info("npu: ms median/max npu ", Fixed{npu.median, timePrecision}, "/",
                   Fixed{npu.maximum, timePrecision}, ", decode+track+rule ",
                   Fixed{logic.median, timePrecision}, "/", Fixed{logic.maximum, timePrecision},
                   ", persons ", stats.persons, ", open violations ", state.rule.openCount(),
                   ", violations ", state.counters.violations.load(), ", overflow ",
                   state.tracker.overflowCount() + state.rule.overflowCount());
    stats.npu.clear();
    stats.logic.clear();
}

void npuLoop(PipelineState& state) {
    pinCurrentThread(npuCpu, "npu", state.log);
    NpuStats stats;
    WindowTimer window{state.options.statsSeconds};
    InferJob job;
    while (state.toNpu.take(job)) {
        const Clock::time_point begin{Clock::now()};
        const bool ran{state.npu.run(state.inputs.at(job.inputSlot).inputId)};
        state.inputBusy.at(job.inputSlot).store(false);
        if (!ran) {
            failRun(state);
            break;
        }
        stats.npu.add(millisecondsBetween(begin, Clock::now()));
        if (!analyseFrame(state, job, stats)) {
            failRun(state);
            break;
        }
        const Clock::time_point now{Clock::now()};
        if (window.due(now)) {
            reportNpu(state, stats);
            window.restart(now);
        }
    }
    state.toNpu.close();
    state.toWriter.close();
}

// ---- Writer thread ----

struct WriterStats {
    WriterStats() : copy{statsCapacity}, draw{statsCapacity}, encode{statsCapacity} {}
    StageStats copy;
    StageStats draw;
    StageStats encode;
    double copyLast{0.0};
};

void reportWriter(PipelineState& state, WriterStats& stats) {
    const Pair copy{readStats(stats.copy)};
    const Pair draw{readStats(stats.draw)};
    const Pair encode{readStats(stats.encode)};
    const BranchCounts counts{state.output.counts()};
    state.log.info(
        "writer: ms median/max copy ", Fixed{copy.median, timePrecision}, "/",
        Fixed{copy.maximum, timePrecision}, ", draw ", Fixed{draw.median, timePrecision}, "/",
        Fixed{draw.maximum, timePrecision}, ", encode ", Fixed{encode.median, timePrecision}, "/",
        Fixed{encode.maximum, timePrecision}, ", frames ", counts.frames, ", video dropped ",
        state.counters.droppedOutput.load(), ", evidence dropped ", counts.evidenceDropped,
        ", clips skipped ", counts.clipsSkipped, ", boxes dropped ", counts.boxesDropped);
    stats.copy.clear();
    stats.draw.clear();
    stats.encode.clear();
}

// One frame of the writer thread: copy, give the camera buffer back, draw, encode and store.
bool writeFrame(PipelineState& state, const OutputJob& job, WriterStats& stats) {
    const Clock::time_point begin{Clock::now()};
    const bool copied{state.output.copy(state.cameraImages.at(job.cameraIndex))};
    const bool returned{giveBack(state, job.cameraIndex)};
    if (!copied || !returned) {
        return false;
    }
    stats.copyLast = millisecondsBetween(begin, Clock::now());
    stats.copy.add(stats.copyLast);
    BranchTimes times;
    const bool encoded{
        state.output.encode(state.frames.at(job.dataSlot), job.captureSeconds, times)};
    state.frames.at(job.dataSlot).busy.store(false, std::memory_order_release);
    stats.draw.add(times.draw);
    stats.encode.add(times.encode);
    const double total{millisecondsBetween(begin, Clock::now())};
    if (total > slowFrameMs) {
        state.log.warn("slow writer frame ", Fixed{total, timePrecision}, " ms: copy ",
                       Fixed{stats.copyLast, timePrecision}, ", evidence ",
                       Fixed{times.evidence, timePrecision}, ", draw ",
                       Fixed{times.draw, timePrecision}, ", encode ",
                       Fixed{times.encode, timePrecision}, ", events ",
                       state.frames.at(job.dataSlot).events.size());
    }
    return encoded;
}

void writerLoop(PipelineState& state) {
    pinCurrentThread(writerCpu, "writer", state.log);
    WriterStats stats;
    WindowTimer window{state.options.statsSeconds};
    OutputJob job;
    while (state.toWriter.take(job)) {
        if (!writeFrame(state, job, stats)) {
            failRun(state);
            break;
        }
        const Clock::time_point now{Clock::now()};
        if (window.due(now)) {
            reportWriter(state, stats);
            window.restart(now);
        }
    }
    state.toWriter.close();
    state.output.finish();
}

void evidenceLoop(PipelineState& state) {
    pinCurrentThread(evidenceCpu, "evidence", state.log);
    state.evidence.run(state.output.hub());
}

void streamLoop(PipelineState& state) {
    pinCurrentThread(streamCpu, "stream", state.log);
    if (state.stream) {
        state.stream->run();
    }
}

void recordLoop(PipelineState& state) {
    pinCurrentThread(recordCpu, "record", state.log);
    if (state.record) {
        state.record->run();
    }
}
} // namespace

namespace {
constexpr double bytesToGb{1.0 / bytesPerGigabyte};
constexpr int gbPrecision{1};

// Warns when the limits of the folders cannot all be reached on the disk: the free-space floor
// would trim a folder before it reaches its own limit.
void warnIfOverCommitted(const AppOptions& options, const EvidenceWorker& evidence,
                         const RecordWorker* record, const Logger& log) {
    const std::string violationDir{options.outputDir};
    const FolderBudget violations{
        static_cast<std::uint64_t>(options.maxStorageGb * bytesPerGigabyte), defaultMinFreeBytes,
        evidence.storedBytes()};
    std::vector<FolderBudget> sharing{violations};
    if (record != nullptr) {
        const std::string archiveDir{options.archiveDir};
        const FolderBudget archived{
            static_cast<std::uint64_t>(options.archiveMaxStorageGb * bytesPerGigabyte),
            defaultArchiveMinFreeBytes, record->storedBytes()};
        if (sameVolume(violationDir, archiveDir)) {
            sharing.push_back(archived);
        } else {
            const std::optional<std::uint64_t> otherFree{volumeFreeBytes(archiveDir)};
            if (otherFree && overCommitted({archived}, *otherFree)) {
                log.warn(
                    "the archive limits do not fit the free space of ", archiveDir, ": ",
                    Fixed{options.archiveMaxStorageGb, gbPrecision}, " GB plus a floor of ",
                    Fixed{static_cast<double>(defaultArchiveMinFreeBytes) * bytesToGb, gbPrecision},
                    " GB, with ", Fixed{static_cast<double>(*otherFree) * bytesToGb, gbPrecision},
                    " GB free: the oldest archive files will go before the limit is reached");
            }
        }
    }
    const std::optional<std::uint64_t> free{volumeFreeBytes(violationDir)};
    if (free && overCommitted(sharing, *free)) {
        log.warn("the storage limits do not fit the free space of ", violationDir, ": ",
                 sharing.size() > 1 ? "violations plus archive plus the floor"
                                    : "violations plus the floor",
                 " exceed the ", Fixed{static_cast<double>(*free) * bytesToGb, gbPrecision},
                 " GB free: the free-space floor will trim the oldest files before a limit is "
                 "reached");
    }
}
} // namespace

std::optional<Pipeline> Pipeline::create(const AppOptions& options, const Logger& log) {
    std::optional<Parts> parts{makeParts(options, log)};
    if (!parts) {
        return std::nullopt;
    }
    auto state{std::make_unique<PipelineState>(options, log, std::move(*parts))};
    const std::optional<RecordWorker>& record{state->record};
    const RecordWorker* recording{record ? &*record : nullptr};
    warnIfOverCommitted(options, state->evidence, recording, log);
    return std::optional<Pipeline>{Pipeline{std::move(state)}};
}

Pipeline::Pipeline(std::unique_ptr<PipelineState> initial) noexcept : state{std::move(initial)} {}
Pipeline::Pipeline(Pipeline&& other) noexcept = default;
Pipeline& Pipeline::operator=(Pipeline&& other) noexcept = default;
Pipeline::~Pipeline() = default;

bool Pipeline::run(const StopSignal& stop) {
    PipelineState& pipe{*state};
    std::thread evidenceThread{[&pipe] { evidenceLoop(pipe); }};
    std::thread recordThread;
    if (pipe.record) {
        recordThread = std::thread{[&pipe] { recordLoop(pipe); }};
    }
    std::thread streamThread;
    if (pipe.stream) {
        streamThread = std::thread{[&pipe] { streamLoop(pipe); }};
    }
    std::thread writerThread{[&pipe] { writerLoop(pipe); }};
    std::thread npuThread{[&pipe] { npuLoop(pipe); }};
    cameraLoop(pipe, stop);
    // In the order of the flow: each thread finishes what it was given before the next one is told
    // to end.
    npuThread.join();
    writerThread.join();
    pipe.output.hub().queue.close();
    if (pipe.stream) {
        // A live picture is not worth waiting for: what is still queued is dropped.
        pipe.stream->stop();
        streamThread.join();
    }
    if (pipe.record) {
        pipe.record->queue().close();
        recordThread.join();
    }
    evidenceThread.join();
    const EvidenceCounts& saved{pipe.evidence.counts()};
    pipe.log.info("processed ", pipe.counters.frames.load(), " frames, violations ",
                  pipe.counters.violations.load(), " (saved ", saved.violationsSaved.load(),
                  ", failed ", saved.violationsFailed.load(), ", lost ",
                  pipe.counters.lostViolations.load(), "), clips saved ", saved.clipsSaved.load(),
                  " (failed ", saved.clipsFailed.load(), "), refused ",
                  saved.violationsRefused.load(), ", old removed ", saved.violationsRemoved.load(),
                  ", dropped camera ", pipe.counters.droppedCamera.load(), ", bad ",
                  pipe.counters.badFrames.load());
    if (pipe.record) {
        const RecordCounts& recorded{pipe.record->counts()};
        const PacketQueue& queue{pipe.record->queue()};
        pipe.log.info("recording: segments saved ", recorded.segmentsSaved.load(), " (",
                      recorded.bytesSaved.load(), " bytes), refused ",
                      recorded.segmentsRefused.load(), ", write errors ",
                      recorded.writeErrors.load(), ", old removed ",
                      recorded.segmentsRemoved.load(), ", queue overflows ", queue.overflowCount(),
                      ", packets dropped ", queue.droppedCount());
    }
    if (pipe.stream) {
        const StreamCounts& streamed{pipe.stream->counts()};
        const PacketQueue& queue{pipe.stream->queue()};
        pipe.log.info("stream: packets sent ", streamed.packetsSent.load(), " (",
                      streamed.bytesSent.load(), " bytes), ",
                      streamed.failed.load() ? "GIVEN UP" : "ok", ", queue overflows ",
                      queue.overflowCount(), ", packets dropped ", queue.droppedCount());
    }
    return pipe.camera.close() && !pipe.failed.load();
}
