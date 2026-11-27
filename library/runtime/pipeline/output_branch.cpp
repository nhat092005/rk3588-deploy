#include "output_branch.h"

#include <algorithm>
#include <ctime>
#include <initializer_list>
#include <utility>
#include <vector>

#include "output_plan.h"
#include "rga_labels.h"
#include "rga_overlay.h"
#include "video_encoder.h"

namespace {
using Clock = std::chrono::steady_clock;

constexpr int noFence{-1};
constexpr std::size_t bitsPerByte{8};
constexpr std::size_t bytesPerKilobit{125}; // 1000 bits / 8
// Boxes that one frame can draw: every detection and every track.
constexpr std::size_t boxCapacity{maxDetections + defaultTrackCapacity};

double millisecondsSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

// The bitrate of a stream of that height (0: the camera size).
int kbpsOf(const AppOptions& options, ImageSize full, int height) {
    return height == 0 ? options.videoKbps : scaledKbps(options.videoKbps, full, height);
}

// The clips are cut from the stream that the plan gives them, so their buffers follow its bitrate.
std::optional<ClipConfig> clipConfigOf(const AppOptions& options, ImageSize full) {
    if (!options.violationClip) {
        return std::nullopt;
    }
    const int height{options.violationClipDownscale ? options.violationClipHeight : 0};
    ClipConfig config;
    config.bytesPerSecond =
        static_cast<std::size_t>(kbpsOf(options, full, height)) * bytesPerKilobit;
    config.preSeconds = options.clipPreSeconds;
    config.postSeconds = options.clipPostSeconds;
    config.gopSeconds = 1.0;
    config.framesPerSecond = defaultVideoFps;
    return config;
}

// The picture scaled for a stream that is smaller than the frame: the RGA scaler, its target and
// the picture it scales: the frame (no `from`), or the target of the next larger scaled stream,
// which is cheaper than scaling the 4K frame again.
struct Scaling {
    RgaSnapshot scaler;
    FrameSlot target;
    std::optional<std::size_t> from;
};

// What the next scaled stream is made from: the last scaled one, if any.
struct ScaleSource {
    std::optional<std::size_t> lane;
    ImageSize size;
    int stride{0};
};

// One H.265 stream: the frame as it is, or scaled first, then encoded, and the queues that take
// the packets.
struct Lane {
    StreamPlan plan;
    std::vector<PacketQueue*> queues;
    std::optional<Scaling> scaling;
    VideoEncoder encoder;
    // The release fence of a scaling that is under way, or noFence.
    int fence{noFence};
};

// Said when a buffer of the low heap cannot be had: the board gives about 75 MB of it, and the
// images of a violation take most of it at the camera size.
void hintLowHeap(const Logger& log) {
    log.error("the low heap may be full (about 75 MB): scale the violation images "
              "(violation.image.downscale), turn them off (violation.image.enabled false), or "
              "scale fewer outputs");
}

// The queues that the plan of a stream names, those that exist.
std::vector<PacketQueue*> queuesOf(const StreamPlan& plan, const PacketSinks& sinks) {
    std::vector<PacketQueue*> queues;
    const std::initializer_list<std::pair<bool, PacketQueue*>> wanted{
        {plan.archive, sinks.recording},
        {plan.session, sinks.session},
        {plan.stream, sinks.stream}};
    for (const auto& [asked, queue] : wanted) {
        if (asked && queue != nullptr) {
            queues.push_back(queue);
        }
    }
    return queues;
}

// `source` is what a scaled stream is made from; it moves on to this stream when it is scaled.
std::optional<Lane> makeLane(const StreamPlan& plan, const AppOptions& options,
                             const CameraFormat& format, const RgaSnapshot& snapshot,
                             const PacketSinks& sinks, ScaleSource& source, std::size_t index,
                             const Logger& log) {
    const ImageSize full{snapshot.size()};
    ImageSize size{full};
    int stride{snapshot.stride()};
    std::optional<Scaling> scaling;
    if (plan.height != 0) {
        size = ImageSize{scaledWidth(full, plan.height), plan.height};
        std::optional<RgaSnapshot> scaler{RgaSnapshot::create(
            SnapshotConfig{source.size, source.stride, Rotation::None, size}, log)};
        if (!scaler) {
            return std::nullopt;
        }
        std::optional<FrameSlot> target{makeFrameSlot(scaler->bytes(), lowHeap, log)};
        if (!target) {
            hintLowHeap(log);
            return std::nullopt;
        }
        stride = scaler->stride();
        scaling = Scaling{*scaler, std::move(*target), source.lane};
        source = ScaleSource{index, size, stride};
    }
    VideoConfig config;
    config.width = size.width;
    config.height = size.height;
    config.horStride = stride;
    config.verStride = size.height;
    config.fullRange = format.fullRange;
    config.bt709 = format.matrix == ColorMatrix::Bt709;
    config.kbps = kbpsOf(options, full, plan.height);
    std::optional<VideoEncoder> encoder{VideoEncoder::create(config, log)};
    if (!encoder) {
        return std::nullopt;
    }
    log.info("video stream ", size.width, "x", size.height, " at ", config.kbps, " kbps");
    return Lane{plan, queuesOf(plan, sinks), std::move(scaling), std::move(*encoder), noFence};
}
} // namespace

struct BranchState {
    BranchState(float confidence, int person, Logger logger, const RgaSnapshot& copier,
                FrameSlot target, std::optional<RgaSnapshot> imageCopier, RgaOverlay outlines,
                RgaLabels texts, std::vector<Lane> streams, std::unique_ptr<EvidenceHub> shared)
        : minConfidence{confidence}, personClass{person}, log{logger}, snapshot{copier},
          slot{std::move(target)}, evidenceCopy{imageCopier}, overlay{std::move(outlines)},
          labels{std::move(texts)}, lanes{std::move(streams)}, evidence{std::move(shared)} {
        boxes.reserve(boxCapacity);
    }
    BranchState(const BranchState&) = delete;
    BranchState& operator=(const BranchState&) = delete;
    BranchState(BranchState&&) = delete;
    BranchState& operator=(BranchState&&) = delete;
    ~BranchState() = default;

    EvidenceSlot* takeEvidence(const FrameData& data, std::size_t eventCount, BranchTimes& times);
    bool drawFrame(const FrameData& data, EvidenceSlot* evidenceSlot, BranchTimes& times);
    void dispatchEvidence(EvidenceSlot& evidenceSlot, std::size_t eventCount);
    void buildBoxes(const FrameData& data);
    void queueClips();
    bool encodeStreams(double seconds, BranchTimes& times);
    const RgaImage* scaleInput(const Scaling& scaling) const;
    void startScaling(Lane& lane) const;
    bool finishScaling(Lane& lane, BranchTimes& times) const;
    void abandonScaling();
    ClipRecorder* recorder() const;

    float minConfidence;
    // Class index of Person: its boxes come from the tracks, not from the detections.
    int personClass;
    Logger log;
    RgaSnapshot snapshot;
    FrameSlot slot;
    // Empty when the violation images are off.
    std::optional<RgaSnapshot> evidenceCopy;
    RgaOverlay overlay;
    RgaLabels labels;
    // Every stream that is encoded, the largest first.
    std::vector<Lane> lanes;
    // Null when the violations are not stored.
    std::unique_ptr<EvidenceHub> evidence;
    std::vector<OverlayBox> boxes;
    std::int64_t nextEventId{1};
    BranchCounts totals;
};

// The clip recorder, or null when clips are off or the violations are not stored.
ClipRecorder* BranchState::recorder() const {
    if (!evidence) {
        return nullptr;
    }
    std::optional<ClipRecorder>& clips{evidence->clips};
    if (!clips) {
        return nullptr;
    }
    return &clips.value();
}

// The picture that a scaled stream is made from: the frame, or a larger scaled stream.
const RgaImage* BranchState::scaleInput(const Scaling& scaling) const {
    if (!scaling.from) {
        return &slot.image;
    }
    const std::optional<Scaling>& larger{lanes.at(*scaling.from).scaling};
    return larger ? &larger->target.image : nullptr;
}

// Starts the scaling of a lane and goes on: the RGA works while the encoder does. A lane that is
// not scaled, or whose scaling is under way, is left as it is.
void BranchState::startScaling(Lane& lane) const {
    if (!lane.scaling || lane.fence >= 0) {
        return;
    }
    const RgaImage* input{scaleInput(*lane.scaling)};
    if (input == nullptr) {
        return;
    }
    const std::optional<int> fence{lane.scaling->scaler.start(*input, lane.scaling->target.image)};
    lane.fence = fence.value_or(noFence);
}

// Has the scaling of a lane done when it returns: waits for the one that was started, or runs it
// now (the first scaled lane, or one that could not be started).
bool BranchState::finishScaling(Lane& lane, BranchTimes& times) const {
    if (!lane.scaling) {
        return true;
    }
    const Clock::time_point scaleStart{Clock::now()};
    bool done{false};
    if (lane.fence >= 0) {
        done = lane.scaling->scaler.finish(lane.fence);
        lane.fence = noFence;
    } else if (const RgaImage * input{scaleInput(*lane.scaling)}; input != nullptr) {
        done = lane.scaling->scaler.run(*input, lane.scaling->target.image);
    }
    times.scale += millisecondsSince(scaleStart);
    return done;
}

// After a failure: waits for the scalings that are still under way, so that no fence is left.
void BranchState::abandonScaling() {
    for (Lane& lane : lanes) {
        if (lane.scaling && lane.fence >= 0) {
            (void)lane.scaling->scaler.finish(lane.fence);
            lane.fence = noFence;
        }
    }
}

// Scales the drawn frame for every stream that is smaller than it, encodes every stream and hands
// the packets to the clips and the queues. The scaling of the next stream runs on the RGA while
// the encoder works on this one. false on an RGA or encoder failure.
bool BranchState::encodeStreams(double seconds, BranchTimes& times) {
    for (std::size_t index{0}; index < lanes.size(); ++index) {
        Lane& lane{lanes.at(index)};
        if (!finishScaling(lane, times)) {
            abandonScaling();
            return false;
        }
        if (index + 1 < lanes.size()) {
            startScaling(lanes.at(index + 1));
        }
        const MppSource& source{lane.scaling ? lane.scaling->target.source : slot.source};
        const Clock::time_point encodeStart{Clock::now()};
        const std::optional<VideoPacket> packet{lane.encoder.encode(source)};
        if (!packet) {
            abandonScaling();
            return false;
        }
        times.encode += millisecondsSince(encodeStart);
        if (ClipRecorder * clips{recorder()}; clips != nullptr && lane.plan.clip) {
            clips->addPacket(packet->data, packet->size, packet->keyFrame, seconds);
        }
        // A full queue drops the packet and counts it (the recording, the session or the stream
        // then has a break): the frame threads never wait for the disk or the network.
        for (PacketQueue* queue : lane.queues) {
            (void)queue->push(packet->data, packet->size, packet->keyFrame, seconds);
        }
    }
    return true;
}

// The boxes of the frame: every detection above the confidence except Person, and the Person
// tracks with their identifiers.
void BranchState::buildBoxes(const FrameData& data) {
    boxes.clear();
    for (const Detection& detection : data.detections) {
        if (detection.classId == personClass || detection.confidence < minConfidence) {
            continue;
        }
        boxes.push_back(
            OverlayBox{PixelBox{detection.left, detection.top, detection.right, detection.bottom},
                       detection.classId, false, detection.confidence, -1});
    }
    for (const TrackedObject& track : data.tracks) {
        const bool violator{std::find(data.violators.begin(), data.violators.end(), track.id) !=
                            data.violators.end()};
        boxes.push_back(OverlayBox{PixelBox{track.left, track.top, track.right, track.bottom},
                                   personClass, violator, track.score, track.id});
    }
}

// Takes a free evidence slot for the violations of the frame and copies the unmarked frame into
// it (when the images are on). Null when the violations are not stored, the frame has none or no
// slot is free (the violations are then counted as dropped). Fails the copy by returning null as
// well, after counting.
EvidenceSlot* BranchState::takeEvidence(const FrameData& data, std::size_t eventCount,
                                        BranchTimes& times) {
    if (!evidence || eventCount == 0) {
        return nullptr;
    }
    const auto free{std::find_if(evidence->slots.begin(), evidence->slots.end(),
                                 [](const std::unique_ptr<EvidenceSlot>& candidate) {
                                     return !candidate->busy.load(std::memory_order_acquire);
                                 })};
    if (free == evidence->slots.end()) {
        totals.evidenceDropped += eventCount;
        return nullptr;
    }
    EvidenceSlot& taken{**free};
    const Clock::time_point start{Clock::now()};
    for (std::size_t index{0}; index < eventCount; ++index) {
        taken.events.at(index) = data.events.at(index);
        taken.eventIds.at(index) = nextEventId++;
    }
    taken.eventCount = eventCount;
    taken.wallTime = std::time(nullptr);
    if (taken.original && evidenceCopy && !evidenceCopy->run(slot.image, taken.original->image)) {
        totals.evidenceDropped += eventCount;
        return nullptr;
    }
    taken.busy.store(true, std::memory_order_release);
    times.evidence = millisecondsSince(start);
    return &taken;
}

// Boxes and labels on the frame, then the marked copy for the evidence. false on an RGA failure.
bool BranchState::drawFrame(const FrameData& data, EvidenceSlot* evidenceSlot, BranchTimes& times) {
    const Clock::time_point start{Clock::now()};
    buildBoxes(data);
    if (!overlay.draw(slot.image, boxes) || !labels.draw(slot.image, boxes)) {
        return false;
    }
    times.draw = millisecondsSince(start);
    if (evidenceSlot == nullptr || !evidenceSlot->marked || !evidenceCopy) {
        return true;
    }
    const Clock::time_point copyStart{Clock::now()};
    const bool copied{evidenceCopy->run(slot.image, evidenceSlot->marked->image)};
    times.evidence += millisecondsSince(copyStart);
    return copied;
}

// Hands the images of an evidence slot to the evidence thread and starts the clips of its
// violations. A full queue loses the evidence and is counted.
void BranchState::dispatchEvidence(EvidenceSlot& evidenceSlot, std::size_t eventCount) {
    const auto position{std::find_if(evidence->slots.begin(), evidence->slots.end(),
                                     [&evidenceSlot](const std::unique_ptr<EvidenceSlot>& item) {
                                         return item.get() == &evidenceSlot;
                                     })};
    EvidenceJob job;
    job.kind = EvidenceKind::Images;
    job.slot = static_cast<std::size_t>(position - evidence->slots.begin());
    if (!evidence->queue.put(job)) {
        evidenceSlot.busy.store(false, std::memory_order_release);
        totals.evidenceDropped += eventCount;
        return;
    }
    ClipRecorder* clips{recorder()};
    for (std::size_t event{0}; clips != nullptr && event < eventCount; ++event) {
        if (!clips->start(evidenceSlot.eventIds.at(event))) {
            ++totals.clipsSkipped;
        }
    }
}

// Hands every finished clip to the evidence thread, or gives its buffer back when the queue is
// full.
void BranchState::queueClips() {
    ClipRecorder* clips{recorder()};
    if (clips == nullptr) {
        return;
    }
    while (const std::optional<ClipRef> clip{clips->finished()}) {
        EvidenceJob job;
        job.kind = EvidenceKind::Clip;
        job.slot = clip->slot;
        job.eventId = clip->eventId;
        job.data = clip->data;
        job.size = clip->size;
        job.packets = clip->packets;
        job.packetCount = clip->packetCount;
        if (!evidence->queue.put(job)) {
            clips->release(clip->slot);
            ++totals.clipsSkipped;
        }
    }
}

std::optional<OutputBranch> OutputBranch::create(const AppOptions& options,
                                                 const CameraFormat& format,
                                                 const PacketSinks& sinks, const Logger& log) {
    std::optional<RgaSnapshot> snapshot{RgaSnapshot::create(
        SnapshotConfig{ImageSize{format.width, format.height}, format.bytesPerLine,
                       options.rotation, ImageSize{}, format.verticalStride},
        log)};
    if (!snapshot) {
        return std::nullopt;
    }
    const ImageSize size{snapshot->size()};
    const bool archive{sinks.recording != nullptr};
    const bool session{sinks.session != nullptr};
    const std::string problem{downscaleProblem(options, size.height, archive, session)};
    if (!problem.empty()) {
        log.error(problem);
        return std::nullopt;
    }
    const bool images{options.violation && options.violationImage};
    std::optional<RgaSnapshot> imageCopier;
    if (images) {
        ImageSize imageSize{size};
        if (options.violationImageDownscale) {
            imageSize = ImageSize{scaledWidth(size, options.violationImageHeight),
                                  options.violationImageHeight};
        }
        imageCopier = RgaSnapshot::create(
            SnapshotConfig{size, snapshot->stride(), Rotation::None, imageSize}, log);
    }
    std::optional<FrameSlot> slot{makeFrameSlot(snapshot->bytes(), lowHeap, log)};
    OverlayConfig overlayConfig;
    overlayConfig.size = size;
    overlayConfig.stride = snapshot->stride();
    const ClassMap& classes{options.classes};
    overlayConfig.classStyles =
        classStylesFor(classes.names.size(), classes.person, classes.noHelmet, classes.noVest);
    LabelConfig labelConfig;
    labelConfig.classNames = classes.names;
    labelConfig.personClass = classes.person;
    std::optional<RgaOverlay> overlay{RgaOverlay::create(overlayConfig, log)};
    std::optional<RgaLabels> labels{RgaLabels::create(overlayConfig, labelConfig, log)};
    if ((images && !imageCopier) || !slot || !overlay || !labels) {
        return std::nullopt;
    }
    if (!overlay->check(slot->image)) {
        return std::nullopt;
    }
    std::unique_ptr<EvidenceHub> hub;
    if (options.violation) {
        const std::size_t imageBytes{imageCopier ? imageCopier->bytes() : 0};
        hub = makeEvidenceHub(EvidenceConfig{imageBytes, images, clipConfigOf(options, size)}, log);
        if (!hub) {
            hintLowHeap(log);
            return std::nullopt;
        }
    }
    // After the evidence images: the big buffers of the low heap go first, so that the small ones
    // of the scaled streams do not split it into pieces too short for them.
    std::vector<Lane> lanes;
    ScaleSource source{std::nullopt, size, snapshot->stride()};
    for (const StreamPlan& plan : planStreams(options, archive, session, sinks.stream != nullptr)) {
        std::optional<Lane> lane{
            makeLane(plan, options, format, *snapshot, sinks, source, lanes.size(), log)};
        if (!lane) {
            return std::nullopt;
        }
        lanes.push_back(std::move(*lane));
    }
    auto state{std::make_unique<BranchState>(options.minConfidence, classes.person, log, *snapshot,
                                             std::move(*slot), imageCopier, std::move(*overlay),
                                             std::move(*labels), std::move(lanes), std::move(hub))};
    return std::optional<OutputBranch>{OutputBranch{std::move(state)}};
}

OutputBranch::OutputBranch(std::unique_ptr<BranchState> initial) noexcept
    : state{std::move(initial)} {}
OutputBranch::OutputBranch(OutputBranch&& other) noexcept = default;
OutputBranch& OutputBranch::operator=(OutputBranch&& other) noexcept = default;
OutputBranch::~OutputBranch() = default;

ImageSize OutputBranch::imageSize() const noexcept {
    return state->snapshot.size();
}

int OutputBranch::imageStride() const noexcept {
    return state->snapshot.stride();
}

ImageSize OutputBranch::evidenceImageSize() const noexcept {
    const std::optional<RgaSnapshot>& copier{state->evidenceCopy};
    return copier ? copier->size() : state->snapshot.size();
}

int OutputBranch::evidenceImageStride() const noexcept {
    const std::optional<RgaSnapshot>& copier{state->evidenceCopy};
    return copier ? copier->stride() : state->snapshot.stride();
}

EvidenceHub* OutputBranch::hub() noexcept {
    return state->evidence.get();
}

bool OutputBranch::copy(const RgaImage& cameraFrame) noexcept {
    return state->snapshot.run(cameraFrame, state->slot.image);
}

bool OutputBranch::encode(const FrameData& data, double seconds, BranchTimes& times) noexcept {
    BranchState& branch{*state};
    ++branch.totals.frames;
    const std::size_t eventCount{std::min(data.events.size(), maxEventsPerFrame)};
    EvidenceSlot* evidenceSlot{branch.takeEvidence(data, eventCount, times)};
    if (!branch.drawFrame(data, evidenceSlot, times)) {
        if (evidenceSlot != nullptr) {
            evidenceSlot->busy.store(false, std::memory_order_release);
        }
        return false;
    }
    if (!branch.encodeStreams(seconds, times)) {
        return false;
    }
    if (evidenceSlot != nullptr) {
        branch.dispatchEvidence(*evidenceSlot, eventCount);
    }
    branch.queueClips();
    return true;
}

void OutputBranch::finish() noexcept {
    if (ClipRecorder * clips{state->recorder()}; clips != nullptr) {
        clips->finishAll();
    }
    state->queueClips();
}

BranchCounts OutputBranch::counts() const noexcept {
    BranchCounts result{state->totals};
    result.boxesDropped = state->overlay.overflowCount() + state->labels.overflowCount();
    return result;
}
