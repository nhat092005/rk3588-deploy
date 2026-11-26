// Thread layouts of the pipeline, measured on the board with the real devices: camera, RGA
// letterbox, NPU, and the output branch (RGA copy of the frame, outlines and labels of made-up
// boxes, H.265 encode, now and then two JPEGs). It decides how stage 7 splits the work.
// Usage: probe_threads <node> <width> <height> <rotation> <model> <design> <frames> [flags]
//   design : main   letterbox and NPU only (no output branch), the baseline of the main thread
//            seq    everything on one thread
//            two    main thread: camera, letterbox, NPU; writer thread: copy, draw, encode
//            async  as two, but the main thread starts the RGA copy before the NPU runs, so the
//                   copy overlaps the NPU and the camera buffer goes back sooner
//            three  camera and letterbox thread, NPU thread, writer thread
//   frames : frames to measure, after 30 frames of warm-up
//   flags  : --buffers <n>      camera buffers (default 4)
//            --npu auto|012     NPU cores (default auto)
//            --cpus <a,b,c>     pin the threads to these CPUs, in the order of the design
//            --boxes <n>        made-up boxes drawn on every frame (default 33)
//            --jpeg-every <n>   two JPEG encodes (original and marked) every n frames, 0 for
//                               never (default 0)
//            --evidence 1       the JPEGs go to a thread of their own, fed by two RGA copies
// Logs the frame rate, the dropped camera frames and output frames, the time of every stage, the
// latency from capture to the end of the encode and the CPU use of every thread.
// Exit code 0 when the run ends, 2 on bad arguments.
#include <pthread.h>
#include <sched.h>
#include <time.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "arguments.h"
#include "camera.h"
#include "dmabuf.h"
#include "jpeg_encoder.h"
#include "logger.h"
#include "mpp_source.h"
#include "npu_infer.h"
#include "rga.h"
#include "rga_labels.h"
#include "rga_overlay.h"
#include "stage_stats.h"
#include "video_encoder.h"

namespace {
using Clock = std::chrono::steady_clock;

constexpr int exitUsage{2};
constexpr int decimalBase{10};
constexpr int warmupFrames{30};
constexpr std::size_t statsCapacity{20'000};
constexpr int timePrecision{2};
constexpr double millisecondsPerSecond{1000.0};
constexpr double nanosecondsPerSecond{1'000'000'000.0};
constexpr double percentScale{100.0};
constexpr std::size_t slotCount{2};
constexpr int defaultBoxes{33};
constexpr int boxDivisor{12};
constexpr int boxSpread{20};
constexpr std::size_t headerArgs{8};
constexpr std::size_t nodeArg{1};
constexpr std::size_t widthArg{2};
constexpr std::size_t heightArg{3};
constexpr std::size_t rotationArg{4};
constexpr std::size_t modelArg{5};
constexpr std::size_t designArg{6};
constexpr std::size_t framesArg{7};
constexpr int classCount{5};
constexpr float boxScore{0.8F};
constexpr std::uint64_t seed{88172645463325252ULL};
constexpr unsigned shiftLeftFirst{13U};
constexpr unsigned shiftRight{7U};
constexpr unsigned shiftLeftSecond{17U};

enum class Design : std::uint8_t { Main, Seq, Two, Async, Three };

struct Options {
    const char* node{nullptr};
    int width{0};
    int height{0};
    Rotation rotation{Rotation::None};
    const char* model{nullptr};
    Design design{Design::Main};
    int frames{0};
    std::size_t buffers{defaultBufferCount};
    NpuCores cores{NpuCores::Auto};
    std::vector<int> cpus;
    int boxes{defaultBoxes};
    int jpegEvery{0};
    bool evidenceThread{false};
};

std::optional<long> parseNumber(const char* text, long low, long high) {
    char* end{nullptr};
    const long value{std::strtol(text, &end, decimalBase)};
    if (end == text || *end != '\0' || value < low || value > high) {
        return std::nullopt;
    }
    return value;
}

std::optional<Design> parseDesign(std::string_view name) {
    constexpr std::array<std::pair<std::string_view, Design>, 5> names{{
        {"main", Design::Main},
        {"seq", Design::Seq},
        {"two", Design::Two},
        {"async", Design::Async},
        {"three", Design::Three},
    }};
    for (const auto& entry : names) {
        if (entry.first == name) {
            return entry.second;
        }
    }
    return std::nullopt;
}

bool parseCpus(const char* text, std::vector<int>& cpus) {
    const std::string list{text};
    std::size_t start{0};
    while (start <= list.size()) {
        const std::size_t comma{list.find(',', start)};
        const std::string item{
            list.substr(start, comma == std::string::npos ? comma : comma - start)};
        const std::optional<long> cpu{parseNumber(item.c_str(), 0, CPU_SETSIZE - 1)};
        if (!cpu) {
            return false;
        }
        cpus.push_back(static_cast<int>(*cpu));
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    return true;
}

// One flag with its value: false for an unknown flag or a bad value.
bool applyFlag(Options& options, std::string_view name, const char* value) {
    if (name == "--buffers") {
        const std::optional<long> count{parseNumber(value, 2, static_cast<long>(maxBufferCount))};
        options.buffers = count ? static_cast<std::size_t>(*count) : 0;
        return count.has_value();
    }
    if (name == "--npu") {
        const std::string_view mode{value};
        options.cores = mode == "012" ? NpuCores::Core012 : NpuCores::Auto;
        return mode == "012" || mode == "auto";
    }
    if (name == "--cpus") {
        return parseCpus(value, options.cpus);
    }
    if (name == "--boxes") {
        const std::optional<long> count{parseNumber(value, 0, 1000)};
        options.boxes = count ? static_cast<int>(*count) : 0;
        return count.has_value();
    }
    if (name == "--evidence") {
        options.evidenceThread = std::string_view{value} == "1";
        return std::string_view{value} == "1" || std::string_view{value} == "0";
    }
    if (name == "--jpeg-every") {
        const std::optional<long> count{parseNumber(value, 0, 1'000'000)};
        options.jpegEvery = count ? static_cast<int>(*count) : 0;
        return count.has_value();
    }
    return false;
}

std::optional<Options> parseArgs(const std::vector<const char*>& args, const Logger& log) {
    if (args.size() < headerArgs || (args.size() - headerArgs) % 2 != 0) {
        log.error("usage: ", args.empty() ? "probe_threads" : args.at(0),
                  " <node> <width> <height> <rotation> <model> <main|seq|two|async|three> <frames>"
                  " [--buffers n] [--npu auto|012] [--cpus a,b,c] [--boxes n] [--jpeg-every n] "
                  "[--evidence 0|1]");
        return std::nullopt;
    }
    Options options;
    options.node = args.at(nodeArg);
    options.model = args.at(modelArg);
    const std::optional<long> width{parseNumber(args.at(widthArg), 2, 16'384)};
    const std::optional<long> height{parseNumber(args.at(heightArg), 2, 16'384)};
    const std::optional<Rotation> rotation{parseRotation(args.at(rotationArg))};
    const std::optional<Design> design{parseDesign(args.at(designArg))};
    const std::optional<long> frames{parseNumber(args.at(framesArg), 1, 1'000'000)};
    if (!width || !height || !rotation || !design || !frames) {
        log.error("bad size, rotation, design or frames");
        return std::nullopt;
    }
    options.width = static_cast<int>(*width);
    options.height = static_cast<int>(*height);
    options.rotation = *rotation;
    options.design = *design;
    options.frames = static_cast<int>(*frames);
    for (std::size_t at{headerArgs}; at < args.size(); at += 2) {
        if (!applyFlag(options, args.at(at), args.at(at + 1))) {
            log.error("bad flag ", args.at(at), " ", args.at(at + 1));
            return std::nullopt;
        }
    }
    return options;
}

double millisecondsBetween(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

// The CPU time of the calling thread in seconds.
double threadCpuSeconds() {
    timespec spent{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &spent);
    return static_cast<double>(spent.tv_sec) +
           (static_cast<double>(spent.tv_nsec) / nanosecondsPerSecond);
}

// Pins the calling thread to one CPU, the list entry `index` when there is one.
void pinThread(const std::vector<int>& cpus, std::size_t index, const Logger& log) {
    if (index >= cpus.size()) {
        return;
    }
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpus.at(index), &set);
    const int status{pthread_setaffinity_np(pthread_self(), sizeof(set), &set)};
    if (status != 0) {
        log.error("pinning to CPU ", cpus.at(index), " failed: ", status);
    }
}

// A frame in flight between the threads.
struct Job {
    std::size_t cameraIndex{0};
    // The input buffer of the model (three) or the snapshot slot (async) that the job uses.
    std::size_t slot{0};
    double captureSeconds{0.0};
    std::int64_t number{0};
};

// One job at a time between two threads. put() never waits: a job that finds the place taken is
// refused. take() waits for a job or for close().
class Mailbox {
public:
    bool put(const Job& job) {
        const std::lock_guard<std::mutex> lock{guard};
        if (full || finished) {
            return false;
        }
        content = job;
        full = true;
        ready.notify_one();
        return true;
    }

    bool take(Job& job) {
        std::unique_lock<std::mutex> lock{guard};
        ready.wait(lock, [this] { return full || finished; });
        if (!full) {
            return false;
        }
        job = content;
        full = false;
        return true;
    }

    void close() {
        const std::lock_guard<std::mutex> lock{guard};
        finished = true;
        ready.notify_all();
    }

private:
    std::mutex guard;
    std::condition_variable ready;
    Job content;
    bool full{false};
    bool finished{false};
};

// Times in milliseconds of the stages of one thread.
struct ThreadStats {
    ThreadStats()
        : letterbox{statsCapacity}, npu{statsCapacity}, copy{statsCapacity}, draw{statsCapacity},
          encode{statsCapacity}, jpeg{statsCapacity}, busy{statsCapacity}, latency{statsCapacity} {}

    StageStats letterbox;
    StageStats npu;
    StageStats copy;
    StageStats draw;
    StageStats encode;
    StageStats jpeg;
    StageStats busy;
    StageStats latency;
    double cpuSeconds{0.0};
    double wallSeconds{0.0};
};

// A snapshot slot: a buffer of the low heap with its RGA and MPP imports.
struct Slot {
    DmaBuf buffer;
    RgaImage image;
    MppSource source;
};

std::optional<Slot> makeSlot(std::size_t bytes, const Logger& log) {
    std::optional<DmaBuf> buffer{DmaBuf::allocate(DmaBufConfig{lowHeap, bytes}, log)};
    if (!buffer) {
        return std::nullopt;
    }
    std::optional<RgaImage> image{RgaImage::import(*buffer, log)};
    std::optional<MppSource> source{MppSource::import(*buffer, log)};
    if (!image || !source) {
        return std::nullopt;
    }
    return Slot{std::move(*buffer), std::move(*image), std::move(*source)};
}

// A model input: a buffer with its RGA image and its NPU id.
struct Input {
    DmaBuf buffer;
    RgaImage image;
    InputId inputId;
};

struct Random {
    std::uint64_t state{seed};
    int next(int below) {
        state ^= state << shiftLeftFirst;
        state ^= state >> shiftRight;
        state ^= state << shiftLeftSecond;
        return below > 0 ? static_cast<int>(state % static_cast<std::uint64_t>(below)) : 0;
    }
};

std::vector<OverlayBox> makeBoxes(const ImageSize& size, int count) {
    std::vector<OverlayBox> boxes;
    Random random;
    for (int index{0}; index < count; ++index) {
        const int boxWidth{(size.width / boxDivisor) + random.next(size.width / boxSpread)};
        const int boxHeight{(size.height / boxDivisor) + random.next(size.height / boxSpread)};
        const int left{random.next(size.width - boxWidth)};
        const int top{random.next(size.height - boxHeight)};
        boxes.push_back(OverlayBox{PixelBox{static_cast<float>(left), static_cast<float>(top),
                                            static_cast<float>(left + boxWidth),
                                            static_cast<float>(top + boxHeight)},
                                   index % classCount, index % boxDivisor == 0, boxScore,
                                   index % boxDivisor == 0 ? index : -1});
    }
    return boxes;
}

// Everything the threads share. Each thread writes only its own statistics.
struct Rig {
    Rig(Camera cameraIn, RgaLetterbox letterboxIn, std::vector<RgaImage> cameraImagesIn,
        NpuInfer npuIn, const Options& config)
        : camera{std::move(cameraIn)}, letterbox{std::move(letterboxIn)},
          cameraImages{std::move(cameraImagesIn)}, npu{std::move(npuIn)}, options{config} {}

    Camera camera;
    RgaLetterbox letterbox;
    std::vector<RgaImage> cameraImages;
    NpuInfer npu;
    Options options;
    std::vector<Input> inputs;
    std::optional<RgaSnapshot> snapshot;
    std::vector<Slot> slots;
    std::optional<RgaOverlay> overlay;
    std::optional<RgaLabels> labels;
    std::optional<VideoEncoder> video;
    std::optional<JpegEncoder> jpeg;
    std::vector<OverlayBox> boxes;
    std::optional<RgaSnapshot> evidenceCopy;
    std::vector<Slot> evidenceSlots;
    std::optional<JpegEncoder> evidenceJpeg;
    Mailbox evidenceBox;
    ThreadStats fourth;
    std::array<std::atomic<bool>, slotCount> slotBusy{};
    std::array<std::atomic<bool>, slotCount> inputBusy{};
    std::atomic<std::int64_t> droppedCamera{0};
    std::atomic<std::int64_t> droppedOutput{0};
    std::atomic<std::int64_t> droppedInference{0};
    std::atomic<std::int64_t> encodedFrames{0};
    std::atomic<bool> failed{false};
    ThreadStats first;
    ThreadStats second;
    ThreadStats third;
    Clock::time_point measureStart;
    Clock::time_point measureEnd;
};

// The camera, the RGA letterbox, the NPU and its inputs.
std::unique_ptr<Rig> buildInputSide(const Options& options, const Logger& log) {
    std::optional<Camera> camera{Camera::open(
        CameraConfig{options.node, options.width, options.height, defaultHeap, options.buffers},
        log)};
    if (!camera) {
        return nullptr;
    }
    const CameraFormat& format{camera->format()};
    std::optional<NpuInfer> npu{NpuInfer::create(NpuConfig{options.model, options.cores}, log)};
    if (!npu) {
        return nullptr;
    }
    std::optional<RgaLetterbox> letterbox{RgaLetterbox::create(
        LetterboxConfig{ImageSize{format.width, format.height}, format.bytesPerLine,
                        npu->inputSide(), options.rotation, format.fullRange},
        log)};
    if (!letterbox) {
        return nullptr;
    }
    std::vector<RgaImage> images;
    for (std::size_t index{0}; index < camera->bufferCount(); ++index) {
        std::optional<RgaImage> image{RgaImage::import(camera->planeBuffer(index, 0), log)};
        if (!image) {
            return nullptr;
        }
        images.push_back(std::move(*image));
    }
    auto rig{std::make_unique<Rig>(std::move(*camera), std::move(*letterbox), std::move(images),
                                   std::move(*npu), options)};
    rig->inputs.reserve(slotCount);
    for (std::size_t index{0}; index < slotCount; ++index) {
        std::optional<DmaBuf> buffer{
            DmaBuf::allocate(DmaBufConfig{defaultHeap, rig->npu.inputBytes()}, log)};
        if (!buffer) {
            return nullptr;
        }
        std::optional<RgaImage> image{RgaImage::import(*buffer, log)};
        const std::optional<InputId> inputId{rig->npu.attachInput(*buffer)};
        if (!image || !inputId || !rig->letterbox.fillPadding(*image)) {
            return nullptr;
        }
        rig->inputs.push_back(Input{std::move(*buffer), std::move(*image), *inputId});
    }
    return rig;
}

// The output branch: snapshot slots, drawing, the video encoder and the JPEG encoder.
bool buildOutputSide(Rig& rig, const Logger& log) {
    const CameraFormat& format{rig.camera.format()};
    rig.snapshot = RgaSnapshot::create(SnapshotConfig{ImageSize{format.width, format.height},
                                                      format.bytesPerLine, rig.options.rotation},
                                       log);
    if (!rig.snapshot) {
        return false;
    }
    const ImageSize size{rig.snapshot->size()};
    rig.slots.reserve(slotCount);
    for (std::size_t index{0}; index < slotCount; ++index) {
        std::optional<Slot> slot{makeSlot(rig.snapshot->bytes(), log)};
        if (!slot) {
            return false;
        }
        rig.slots.push_back(std::move(*slot));
    }
    OverlayConfig overlayConfig;
    overlayConfig.size = size;
    overlayConfig.stride = rig.snapshot->stride();
    rig.overlay = RgaOverlay::create(overlayConfig, log);
    rig.labels = RgaLabels::create(overlayConfig, LabelConfig{}, log);
    VideoConfig video;
    video.width = size.width;
    video.height = size.height;
    video.horStride = rig.snapshot->stride();
    video.verStride = size.height;
    video.fullRange = format.fullRange;
    rig.video = VideoEncoder::create(video, log);
    rig.jpeg = JpegEncoder::create(
        JpegConfig{size.width, size.height, rig.snapshot->stride(), size.height, format.fullRange},
        log);
    rig.boxes = makeBoxes(size, rig.options.boxes);
    if (rig.options.evidenceThread) {
        rig.evidenceCopy =
            RgaSnapshot::create(SnapshotConfig{size, rig.snapshot->stride(), Rotation::None}, log);
        rig.evidenceJpeg =
            JpegEncoder::create(JpegConfig{size.width, size.height, rig.snapshot->stride(),
                                           size.height, format.fullRange},
                                log);
        for (std::size_t index{0}; index < slotCount; ++index) {
            std::optional<Slot> slot{makeSlot(rig.snapshot->bytes(), log)};
            if (!slot) {
                return false;
            }
            rig.evidenceSlots.push_back(std::move(*slot));
        }
        if (!rig.evidenceCopy || !rig.evidenceJpeg) {
            return false;
        }
    }
    return rig.overlay && rig.labels && rig.video && rig.jpeg &&
           rig.overlay->check(rig.slots.at(0).image);
}

// Seconds of CLOCK_MONOTONIC now, the clock of Frame::captureSeconds.
double monotonicNow() {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<double>(now.tv_sec) +
           (static_cast<double>(now.tv_nsec) / nanosecondsPerSecond);
}

// The output branch on the frame in a slot: draw, encode, and now and then two JPEGs.
bool outputFrame(Rig& rig, std::size_t slot, const Job& job, ThreadStats& stats) {
    if (!rig.overlay || !rig.labels || !rig.video || !rig.jpeg) {
        return false;
    }
    const Slot& target{rig.slots.at(slot)};
    const bool counting{job.number >= warmupFrames};
    const bool withJpeg{rig.options.jpegEvery > 0 && job.number % rig.options.jpegEvery == 0};
    double jpegMs{0.0};
    const bool hand{withJpeg && rig.options.evidenceThread};
    // Inline: encode here. With an evidence thread: copy the frame (RGA) for it instead.
    const auto jpegTime = [&](std::size_t evidenceIndex) {
        const Clock::time_point start{Clock::now()};
        bool done{true};
        if (hand) {
            const Slot& copy{rig.evidenceSlots.at(evidenceIndex)};
            done = rig.evidenceCopy.has_value() && rig.evidenceCopy->run(target.image, copy.image);
        } else {
            done = rig.jpeg->encode(target.source).has_value();
        }
        jpegMs += millisecondsBetween(start, Clock::now());
        return done;
    };
    // The original JPEG is taken before anything is drawn.
    if (withJpeg && !jpegTime(0)) {
        return false;
    }
    const Clock::time_point begin{Clock::now()};
    if (!rig.overlay->draw(target.image, rig.boxes) || !rig.labels->draw(target.image, rig.boxes)) {
        return false;
    }
    const Clock::time_point drawn{Clock::now()};
    if (!rig.video->encode(target.source)) {
        return false;
    }
    const Clock::time_point encoded{Clock::now()};
    if (withJpeg && !jpegTime(1)) {
        return false;
    }
    if (hand && !rig.evidenceBox.put(job)) {
        rig.droppedOutput.fetch_add(1);
    }
    if (counting) {
        stats.draw.add(millisecondsBetween(begin, drawn));
        stats.encode.add(millisecondsBetween(drawn, encoded));
        if (withJpeg) {
            stats.jpeg.add(jpegMs);
        }
        stats.latency.add((monotonicNow() - job.captureSeconds) * millisecondsPerSecond);
        rig.encodedFrames.fetch_add(1);
    }
    return true;
}

// Waits for a camera frame and takes it out. Counts a gap of the sequence as dropped frames.
bool nextFrame(Rig& rig, const StopSignal& stop, std::optional<std::int64_t>& lastSequence,
               Frame& frame) {
    while (true) {
        if (rig.camera.waitFrame(stop) != WaitStatus::Ready) {
            return false;
        }
        const Dequeued dequeued{rig.camera.dequeue()};
        if (dequeued.status == DequeueStatus::Failed) {
            return false;
        }
        if (dequeued.status == DequeueStatus::Frame) {
            if (lastSequence && dequeued.frame.sequence > *lastSequence + 1) {
                rig.droppedCamera.fetch_add(dequeued.frame.sequence - *lastSequence - 1);
            }
            lastSequence = dequeued.frame.sequence;
            frame = dequeued.frame;
            return true;
        }
    }
}

// Times the calling thread from now on: wall and CPU seconds are filled by the destructor.
class ThreadTimer {
public:
    explicit ThreadTimer(ThreadStats& target)
        : stats{target}, wall{Clock::now()}, cpu{threadCpuSeconds()} {}
    ThreadTimer(const ThreadTimer&) = delete;
    ThreadTimer& operator=(const ThreadTimer&) = delete;
    ThreadTimer(ThreadTimer&&) = delete;
    ThreadTimer& operator=(ThreadTimer&&) = delete;
    ~ThreadTimer() {
        stats.cpuSeconds = threadCpuSeconds() - cpu;
        stats.wallSeconds = millisecondsBetween(wall, Clock::now()) / millisecondsPerSecond;
    }

private:
    ThreadStats& stats;
    Clock::time_point wall;
    double cpu;
};

// The first thread of every design: takes camera frames until the measured frames are done.
// `handle` does the work of one frame and returns false on a failure.
template <typename Handle> void cameraLoop(Rig& rig, const StopSignal& stop, Handle handle) {
    pinThread(rig.options.cpus, 0, Logger{"PIN", LogLevel::Info});
    const ThreadTimer timer{rig.first};
    std::optional<std::int64_t> lastSequence;
    const std::int64_t total{static_cast<std::int64_t>(warmupFrames) + rig.options.frames};
    for (std::int64_t number{0}; number < total && !rig.failed.load(); ++number) {
        Frame frame;
        if (!nextFrame(rig, stop, lastSequence, frame)) {
            rig.failed.store(true);
            return;
        }
        if (number == warmupFrames) {
            rig.measureStart = Clock::now();
        }
        const Job job{frame.index, static_cast<std::size_t>(number % slotCount),
                      frame.captureSeconds, number};
        if (!handle(job)) {
            rig.failed.store(true);
        }
    }
    rig.measureEnd = Clock::now();
}

// letterbox, NPU on the input `inputIndex`, times into the first thread's statistics.
bool inferFrame(Rig& rig, const Job& job, std::size_t inputIndex, bool requeueAfterLetterbox,
                ThreadStats& stats) {
    const Input& input{rig.inputs.at(inputIndex)};
    const Clock::time_point begin{Clock::now()};
    if (!rig.letterbox.run(rig.cameraImages.at(job.cameraIndex), input.image)) {
        return false;
    }
    if (requeueAfterLetterbox && !rig.camera.queue(job.cameraIndex)) {
        return false;
    }
    const Clock::time_point converted{Clock::now()};
    if (!rig.npu.run(input.inputId)) {
        return false;
    }
    const Clock::time_point end{Clock::now()};
    if (job.number >= warmupFrames) {
        stats.letterbox.add(millisecondsBetween(begin, converted));
        stats.npu.add(millisecondsBetween(converted, end));
        stats.busy.add(millisecondsBetween(begin, end));
    }
    return true;
}

// The writer thread of the designs with a writer: for each job copy (unless the slot has it),
// give the camera buffer back, draw and encode.
void writerLoop(Rig& rig, Mailbox& box, bool copyHere, std::size_t cpuIndex, ThreadStats& stats) {
    pinThread(rig.options.cpus, cpuIndex, Logger{"PIN", LogLevel::Info});
    const ThreadTimer timer{stats};
    Job job;
    if (!rig.snapshot) {
        rig.failed.store(true);
        return;
    }
    while (box.take(job)) {
        if (copyHere) {
            const Clock::time_point start{Clock::now()};
            if (!rig.snapshot->run(rig.cameraImages.at(job.cameraIndex), rig.slots.at(0).image) ||
                !rig.camera.queue(job.cameraIndex)) {
                rig.failed.store(true);
                return;
            }
            if (job.number >= warmupFrames) {
                stats.copy.add(millisecondsBetween(start, Clock::now()));
            }
        }
        const std::size_t slot{copyHere ? 0 : job.slot};
        if (!outputFrame(rig, slot, job, stats)) {
            rig.failed.store(true);
            return;
        }
        if (!copyHere) {
            rig.slotBusy.at(slot).store(false);
        }
    }
}

// The evidence thread: encodes the two copies of an event as JPEGs.
void evidenceLoop(Rig& rig, std::size_t cpuIndex) {
    pinThread(rig.options.cpus, cpuIndex, Logger{"PIN", LogLevel::Info});
    const ThreadTimer timer{rig.fourth};
    Job job;
    if (!rig.evidenceJpeg) {
        rig.failed.store(true);
        return;
    }
    while (rig.evidenceBox.take(job)) {
        const Clock::time_point start{Clock::now()};
        for (const Slot& copy : rig.evidenceSlots) {
            if (!rig.evidenceJpeg->encode(copy.source)) {
                rig.failed.store(true);
                return;
            }
        }
        if (job.number >= warmupFrames) {
            rig.fourth.jpeg.add(millisecondsBetween(start, Clock::now()));
        }
    }
}

// Runs the evidence thread while it lives, when the options ask for one.
class Evidence {
public:
    explicit Evidence(Rig& shared) : rig{shared} {
        if (rig.options.evidenceThread) {
            worker = std::thread{[this] { evidenceLoop(rig, evidenceCpuIndex); }};
        }
    }
    Evidence(const Evidence&) = delete;
    Evidence& operator=(const Evidence&) = delete;
    Evidence(Evidence&&) = delete;
    Evidence& operator=(Evidence&&) = delete;
    ~Evidence() {
        rig.evidenceBox.close();
        if (worker.joinable()) {
            worker.join();
        }
    }

private:
    static constexpr std::size_t evidenceCpuIndex{3};
    Rig& rig;
    std::thread worker;
};

// The main design: letterbox and NPU only.
void runMain(Rig& rig, const StopSignal& stop) {
    cameraLoop(rig, stop,
               [&rig](const Job& job) { return inferFrame(rig, job, 0, true, rig.first); });
}

// Everything on one thread.
void runSeq(Rig& rig, const StopSignal& stop) {
    if (!rig.snapshot) {
        rig.failed.store(true);
        return;
    }
    cameraLoop(rig, stop, [&rig](const Job& job) {
        const Clock::time_point start{Clock::now()};
        if (!rig.snapshot->run(rig.cameraImages.at(job.cameraIndex), rig.slots.at(0).image)) {
            return false;
        }
        if (job.number >= warmupFrames) {
            rig.first.copy.add(millisecondsBetween(start, Clock::now()));
        }
        return inferFrame(rig, job, 0, true, rig.first) && outputFrame(rig, 0, job, rig.first);
    });
}

// Main thread: camera, letterbox, NPU. Writer thread: copy, draw, encode.
void runTwo(Rig& rig, const StopSignal& stop) {
    const Evidence evidence{rig};
    Mailbox box;
    std::thread writer{[&] { writerLoop(rig, box, true, 1, rig.second); }};
    cameraLoop(rig, stop, [&](const Job& job) {
        if (!inferFrame(rig, job, 0, false, rig.first)) {
            return false;
        }
        if (!box.put(job)) {
            rig.droppedOutput.fetch_add(1);
            return rig.camera.queue(job.cameraIndex);
        }
        return true;
    });
    box.close();
    writer.join();
}

// As runTwo, but the main thread starts the copy before the NPU runs.
void runAsync(Rig& rig, const StopSignal& stop) {
    const Evidence evidence{rig};
    Mailbox box;
    std::thread writer{[&] { writerLoop(rig, box, false, 1, rig.second); }};
    cameraLoop(rig, stop, [&](const Job& job) {
        const std::size_t slot{job.slot};
        std::optional<int> fence;
        if (!rig.slotBusy.at(slot).load()) {
            const Clock::time_point start{Clock::now()};
            fence =
                rig.snapshot->start(rig.cameraImages.at(job.cameraIndex), rig.slots.at(slot).image);
            if (!fence) {
                return false;
            }
            if (job.number >= warmupFrames) {
                rig.first.copy.add(millisecondsBetween(start, Clock::now()));
            }
        }
        if (!inferFrame(rig, job, 0, false, rig.first)) {
            return false;
        }
        bool copied{true};
        if (fence) {
            const Clock::time_point start{Clock::now()};
            copied = rig.snapshot->finish(*fence);
            if (job.number >= warmupFrames) {
                rig.first.draw.add(millisecondsBetween(start, Clock::now()));
            }
        }
        if (!copied || !rig.camera.queue(job.cameraIndex)) {
            return false;
        }
        if (!fence) {
            rig.droppedOutput.fetch_add(1);
            return true;
        }
        rig.slotBusy.at(slot).store(true);
        if (!box.put(job)) {
            rig.slotBusy.at(slot).store(false);
            rig.droppedOutput.fetch_add(1);
        }
        return true;
    });
    box.close();
    writer.join();
}

// Thread 1: camera and letterbox. Thread 2: NPU. Thread 3: copy, draw, encode.
void runThree(Rig& rig, const StopSignal& stop) {
    const Evidence evidence{rig};
    Mailbox toNpu;
    Mailbox toWriter;
    std::thread writer{[&] { writerLoop(rig, toWriter, true, 2, rig.third); }};
    std::thread npu{[&] {
        pinThread(rig.options.cpus, 1, Logger{"PIN", LogLevel::Info});
        const ThreadTimer timer{rig.second};
        Job job;
        while (toNpu.take(job)) {
            const Clock::time_point start{Clock::now()};
            const bool ran{rig.npu.run(rig.inputs.at(job.slot).inputId)};
            rig.inputBusy.at(job.slot).store(false);
            if (!ran) {
                rig.failed.store(true);
                return;
            }
            if (job.number >= warmupFrames) {
                rig.second.npu.add(millisecondsBetween(start, Clock::now()));
            }
            if (!toWriter.put(job)) {
                rig.droppedOutput.fetch_add(1);
                if (!rig.camera.queue(job.cameraIndex)) {
                    rig.failed.store(true);
                    return;
                }
            }
        }
    }};
    cameraLoop(rig, stop, [&](const Job& job) {
        if (rig.inputBusy.at(job.slot).load()) {
            rig.droppedInference.fetch_add(1);
            return rig.camera.queue(job.cameraIndex);
        }
        const Input& input{rig.inputs.at(job.slot)};
        const Clock::time_point start{Clock::now()};
        if (!rig.letterbox.run(rig.cameraImages.at(job.cameraIndex), input.image)) {
            return false;
        }
        if (job.number >= warmupFrames) {
            rig.first.letterbox.add(millisecondsBetween(start, Clock::now()));
        }
        rig.inputBusy.at(job.slot).store(true);
        if (!toNpu.put(job)) {
            rig.inputBusy.at(job.slot).store(false);
            rig.droppedInference.fetch_add(1);
            return rig.camera.queue(job.cameraIndex);
        }
        return true;
    });
    toNpu.close();
    npu.join();
    toWriter.close();
    writer.join();
}

void logStat(const Logger& log, const char* name, StageStats& stats) {
    if (stats.count() > 0) {
        log.info("  ", name, ": median ", Fixed{stats.median(), timePrecision}, " ms, max ",
                 Fixed{stats.maximum(), timePrecision}, " ms, n ", stats.count());
    }
}

void logThread(const Logger& log, const char* name, ThreadStats& stats) {
    log.info(name, ": CPU ", Fixed{stats.cpuSeconds / stats.wallSeconds * percentScale, 0},
             " % of ", Fixed{stats.wallSeconds, 1}, " s");
    logStat(log, "letterbox", stats.letterbox);
    logStat(log, "npu", stats.npu);
    logStat(log, "copy (or start)", stats.copy);
    logStat(log, "draw (async: finish)", stats.draw);
    logStat(log, "video encode", stats.encode);
    logStat(log, "jpeg x2 per event", stats.jpeg);
    logStat(log, "busy", stats.busy);
    logStat(log, "capture to end of encode", stats.latency);
}

void report(Rig& rig, const Logger& log) {
    const double seconds{millisecondsBetween(rig.measureStart, rig.measureEnd) /
                         millisecondsPerSecond};
    log.info("measured ", rig.options.frames, " frames in ", Fixed{seconds, 2},
             " s: ", Fixed{static_cast<double>(rig.options.frames) / seconds, 2},
             " fps, camera frames dropped ", rig.droppedCamera.load(), ", inference skipped ",
             rig.droppedInference.load(), ", output frames dropped ", rig.droppedOutput.load(),
             ", output frames encoded ", rig.encodedFrames.load());
    logThread(log, "thread 1", rig.first);
    if (rig.second.wallSeconds > 0.0) {
        logThread(log, "thread 2", rig.second);
    }
    if (rig.third.wallSeconds > 0.0) {
        logThread(log, "thread 3", rig.third);
    }
    if (rig.fourth.wallSeconds > 0.0) {
        logThread(log, "evidence thread", rig.fourth);
    }
}

bool run(const Options& options, const StopSignal& stop, const Logger& log) {
    std::unique_ptr<Rig> rig{buildInputSide(options, log)};
    if (!rig || (options.design != Design::Main && !buildOutputSide(*rig, log))) {
        return false;
    }
    switch (options.design) {
    case Design::Main:
        runMain(*rig, stop);
        break;
    case Design::Seq:
        runSeq(*rig, stop);
        break;
    case Design::Two:
        runTwo(*rig, stop);
        break;
    case Design::Async:
        runAsync(*rig, stop);
        break;
    case Design::Three:
        runThree(*rig, stop);
        break;
    }
    const bool closed{rig->camera.close()};
    if (!rig->failed.load()) {
        report(*rig, log);
    }
    return closed && !rig->failed.load();
}
} // namespace

int main(int argc, char** argv) {
    const std::vector<const char*> args{commandLine(argc, argv)};
    const Logger log{"PROBE_THREADS", LogLevel::Info};
    const std::optional<Options> options{parseArgs(args, log)};
    if (!options) {
        return exitUsage;
    }
    const std::optional<StopSignal> stop{StopSignal::create(log)};
    const bool passed{stop && run(*options, *stop, log)};
    if (passed) {
        log.info("PASS");
    } else {
        log.error("FAIL");
    }
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
