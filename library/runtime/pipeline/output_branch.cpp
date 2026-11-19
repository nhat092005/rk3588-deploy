#include "output_branch.h"

#include <algorithm>
#include <ctime>
#include <initializer_list>
#include <utility>
#include <vector>

#include "rga_labels.h"
#include "rga_overlay.h"
#include "video_encoder.h"

namespace {
using Clock = std::chrono::steady_clock;

constexpr std::size_t bitsPerByte{8};
constexpr std::size_t bytesPerKilobit{125}; // 1000 bits / 8
// Boxes that one frame can draw: every detection and every track.
constexpr std::size_t boxCapacity{maxDetections + defaultTrackCapacity};

double millisecondsSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

std::optional<ClipConfig> clipConfigOf(const AppOptions& options) {
    if (options.clipPreSeconds <= 0.0 && options.clipPostSeconds <= 0.0) {
        return std::nullopt;
    }
    ClipConfig config;
    config.bytesPerSecond = static_cast<std::size_t>(options.videoKbps) * bytesPerKilobit;
    config.preSeconds = options.clipPreSeconds;
    config.postSeconds = options.clipPostSeconds;
    config.gopSeconds = 1.0;
    config.framesPerSecond = defaultVideoFps;
    return config;
}
} // namespace

struct BranchState {
    BranchState(float confidence, Logger logger, const PacketSinks& outputs,
                const RgaSnapshot& copier, FrameSlot target, const RgaSnapshot& imageCopier,
                RgaOverlay outlines, RgaLabels texts, VideoEncoder encoder,
                std::unique_ptr<EvidenceHub> shared)
        : minConfidence{confidence}, log{logger}, sinks{outputs}, snapshot{copier},
          slot{std::move(target)}, evidenceCopy{imageCopier}, overlay{std::move(outlines)},
          labels{std::move(texts)}, video{std::move(encoder)}, evidence{std::move(shared)} {
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
    ClipRecorder* recorder() const;

    float minConfidence;
    Logger log;
    PacketSinks sinks;
    RgaSnapshot snapshot;
    FrameSlot slot;
    RgaSnapshot evidenceCopy;
    RgaOverlay overlay;
    RgaLabels labels;
    VideoEncoder video;
    std::unique_ptr<EvidenceHub> evidence;
    std::vector<OverlayBox> boxes;
    std::int64_t nextEventId{1};
    BranchCounts totals;
};

// The clip recorder, or null when clips are off.
ClipRecorder* BranchState::recorder() const {
    std::optional<ClipRecorder>& clips{evidence->clips};
    if (!clips) {
        return nullptr;
    }
    return &clips.value();
}

// The boxes of the frame: every detection above the confidence except Person, and the Person
// tracks with their identifiers.
void BranchState::buildBoxes(const FrameData& data) {
    boxes.clear();
    for (const Detection& detection : data.detections) {
        if (detection.classId == defaultPersonClass || detection.confidence < minConfidence) {
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
                                   defaultPersonClass, violator, track.score, track.id});
    }
}

// Takes a free evidence slot for the violations of the frame and copies the unmarked frame into
// it. Null when the frame has none or no slot is free (the violations are then counted as
// dropped). Fails the copy by returning null as well, after counting.
EvidenceSlot* BranchState::takeEvidence(const FrameData& data, std::size_t eventCount,
                                        BranchTimes& times) {
    if (eventCount == 0) {
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
    if (!evidenceCopy.run(slot.image, taken.original.image)) {
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
    if (evidenceSlot == nullptr) {
        return true;
    }
    const Clock::time_point copyStart{Clock::now()};
    const bool copied{evidenceCopy.run(slot.image, evidenceSlot->marked.image)};
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
        if (!evidence->queue.put(job)) {
            clips->release(clip->slot);
            ++totals.clipsSkipped;
        }
    }
}

std::optional<OutputBranch> OutputBranch::create(const AppOptions& options,
                                                 const CameraFormat& format,
                                                 const PacketSinks& sinks, const Logger& log) {
    std::optional<RgaSnapshot> snapshot{
        RgaSnapshot::create(SnapshotConfig{ImageSize{format.width, format.height},
                                           format.bytesPerLine, options.rotation},
                            log)};
    if (!snapshot) {
        return std::nullopt;
    }
    const ImageSize size{snapshot->size()};
    std::optional<RgaSnapshot> imageCopier{
        RgaSnapshot::create(SnapshotConfig{size, snapshot->stride(), Rotation::None}, log)};
    std::optional<FrameSlot> slot{makeFrameSlot(snapshot->bytes(), lowHeap, log)};
    OverlayConfig overlayConfig;
    overlayConfig.size = size;
    overlayConfig.stride = snapshot->stride();
    std::optional<RgaOverlay> overlay{RgaOverlay::create(overlayConfig, log)};
    std::optional<RgaLabels> labels{RgaLabels::create(overlayConfig, LabelConfig{}, log)};
    VideoConfig videoConfig;
    videoConfig.width = size.width;
    videoConfig.height = size.height;
    videoConfig.horStride = snapshot->stride();
    videoConfig.verStride = size.height;
    videoConfig.fullRange = format.fullRange;
    videoConfig.kbps = options.videoKbps;
    std::optional<VideoEncoder> video{VideoEncoder::create(videoConfig, log)};
    if (!imageCopier || !slot || !overlay || !labels || !video) {
        return std::nullopt;
    }
    if (!overlay->check(slot->image)) {
        return std::nullopt;
    }
    std::unique_ptr<EvidenceHub> hub{
        makeEvidenceHub(EvidenceConfig{snapshot->bytes(), clipConfigOf(options)}, log)};
    if (!hub) {
        return std::nullopt;
    }
    auto state{std::make_unique<BranchState>(
        options.minConfidence, log, sinks, *snapshot, std::move(*slot), *imageCopier,
        std::move(*overlay), std::move(*labels), std::move(*video), std::move(hub))};
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

EvidenceHub& OutputBranch::hub() noexcept {
    return *state->evidence;
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
    const Clock::time_point encodeStart{Clock::now()};
    const std::optional<VideoPacket> packet{branch.video.encode(branch.slot.source)};
    if (!packet) {
        return false;
    }
    times.encode = millisecondsSince(encodeStart);
    if (ClipRecorder * clips{branch.recorder()}; clips != nullptr) {
        clips->addPacket(packet->data, packet->size, packet->keyFrame, seconds);
    }
    // A full queue drops the packet and counts it (the recording or the stream then has a break):
    // the frame threads never wait for the disk or the network.
    for (PacketQueue* sink : {branch.sinks.recording, branch.sinks.stream}) {
        if (sink != nullptr) {
            (void)sink->push(packet->data, packet->size, packet->keyFrame, seconds);
        }
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
