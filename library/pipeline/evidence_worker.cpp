#include "evidence_worker.h"

#include <chrono>
#include <ctime>
#include <utility>

#include "violation_retention.h"

namespace {
using Clock = std::chrono::steady_clock;

// Violations whose directory is remembered, for the clips that arrive later.
constexpr std::size_t rememberedCount{32};
constexpr int timePrecision{1};
// How often the folder is read again and the limits are checked when nothing happens.
constexpr std::chrono::hours housekeepingPeriod{1};

double millisecondsSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

struct Remembered {
    std::int64_t eventId{0};
    std::string directory;
};
} // namespace

struct EvidenceWorker::State {
    State(Logger logger, ImageSize imageSize, ViolationStore violationStore, JpegEncoder first,
          JpegEncoder second, ViolationRetention limits)
        : log{logger}, size{imageSize}, store{std::move(violationStore)},
          originalJpeg{std::move(first)}, markedJpeg{std::move(second)},
          retention{std::move(limits)} {}

    void housekeeping(bool rescan);
    bool hasRoom();

    void saveImages(EvidenceSlot& slot);
    void saveClip(EvidenceHub& hub, const EvidenceJob& job);
    void remember(std::int64_t eventId, const std::string& directory);
    const std::string* find(std::int64_t eventId) const;

    Logger log;
    ImageSize size;
    ViolationStore store;
    JpegEncoder originalJpeg;
    JpegEncoder markedJpeg;
    ViolationRetention retention;
    // Whether the last check found room on the volume. The check runs again before a refusal.
    bool roomLeft{true};
    EvidenceCounts counts;
    std::array<Remembered, rememberedCount> paths{};
    std::size_t nextPath{0};
};

// Applies the limits (and reads the folder again when asked) and notes whether there is room.
void EvidenceWorker::State::housekeeping(bool rescan) {
    if (rescan) {
        retention.rescan();
    }
    const bool before{roomLeft};
    roomLeft = retention.enforce(std::time(nullptr));
    counts.violationsRemoved.store(static_cast<std::int64_t>(retention.removedCount()));
    if (!roomLeft && before) {
        log.error("the volume has no room for evidence, nothing is written until it has");
    } else if (roomLeft && !before) {
        log.info("the volume has room for evidence again");
    }
}

// Whether to write: asks the volume again when the last answer was no.
bool EvidenceWorker::State::hasRoom() {
    if (!roomLeft) {
        housekeeping(false);
    }
    return roomLeft;
}

void EvidenceWorker::State::remember(std::int64_t eventId, const std::string& directory) {
    paths.at(nextPath) = Remembered{eventId, directory};
    nextPath = (nextPath + 1) % paths.size();
}

const std::string* EvidenceWorker::State::find(std::int64_t eventId) const {
    for (const Remembered& entry : paths) {
        if (entry.eventId == eventId && !entry.directory.empty()) {
            return &entry.directory;
        }
    }
    return nullptr;
}

// The two JPEGs of an evidence slot, then one stored violation per event of the frame.
void EvidenceWorker::State::saveImages(EvidenceSlot& slot) {
    if (!hasRoom()) {
        counts.violationsRefused.fetch_add(static_cast<std::int64_t>(slot.eventCount));
        log.error(slot.eventCount, " violation(s) not stored: no room on the volume");
        slot.eventCount = 0;
        slot.busy.store(false, std::memory_order_release);
        return;
    }
    const Clock::time_point start{Clock::now()};
    const std::optional<std::size_t> original{originalJpeg.encode(slot.original.source)};
    const std::optional<std::size_t> marked{markedJpeg.encode(slot.marked.source)};
    const double jpegMs{millisecondsSince(start)};
    for (std::size_t index{0}; index < slot.eventCount; ++index) {
        if (!original || !marked) {
            counts.violationsFailed.fetch_add(1);
            continue;
        }
        ViolationRecord record;
        record.event = slot.events.at(index);
        record.wallTime = slot.wallTime;
        record.imageWidth = size.width;
        record.imageHeight = size.height;
        record.original = StoredBytes{originalJpeg.data(), *original};
        record.marked = StoredBytes{markedJpeg.data(), *marked};
        const std::optional<std::string> directory{store.save(record)};
        if (!directory) {
            counts.violationsFailed.fetch_add(1);
            continue;
        }
        remember(slot.eventIds.at(index), *directory);
        retention.add(*directory);
        counts.violationsSaved.fetch_add(1);
        log.info("violation saved ", *directory, " (jpeg x2 ", Fixed{jpegMs, timePrecision},
                 " ms)");
    }
    slot.eventCount = 0;
    slot.busy.store(false, std::memory_order_release);
    housekeeping(false);
}

void EvidenceWorker::State::saveClip(EvidenceHub& hub, const EvidenceJob& job) {
    const std::string* directory{find(job.eventId)};
    const bool saved{directory != nullptr && hasRoom() &&
                     store.saveClip(*directory, StoredBytes{job.data, job.size})};
    if (saved) {
        retention.addBytes(*directory, job.size);
        counts.clipsSaved.fetch_add(1);
        log.info("clip saved in ", *directory, " (", job.size, " bytes)");
    } else {
        counts.clipsFailed.fetch_add(1);
        log.warn("clip of event ", job.eventId, " not saved");
    }
    if (hub.clips) {
        hub.clips->release(job.slot);
    }
    if (saved) {
        housekeeping(false);
    }
}

std::optional<EvidenceWorker> EvidenceWorker::create(const AppOptions& options, ImageSize size,
                                                     int stride, bool fullRange,
                                                     const Logger& log) {
    std::optional<ViolationStore> store{
        ViolationStore::create(StoreConfig{options.outputDir, defaultFileMode}, log)};
    const JpegConfig config{size.width,  size.height, stride,
                            size.height, fullRange,   options.jpegQuality};
    std::optional<JpegEncoder> first{JpegEncoder::create(config, log)};
    std::optional<JpegEncoder> second{JpegEncoder::create(config, log)};
    std::optional<ViolationRetention> retention{ViolationRetention::create(
        RetentionConfig{options.outputDir,
                        static_cast<std::uint64_t>(options.maxStorageGb * bytesPerGigabyte),
                        options.keepDays, defaultMinFreeBytes},
        log)};
    if (!store || !first || !second || !retention) {
        return std::nullopt;
    }
    auto state{std::make_unique<State>(log, size, std::move(*store), std::move(*first),
                                       std::move(*second), std::move(*retention))};
    state->housekeeping(false);
    return std::optional<EvidenceWorker>{EvidenceWorker{std::move(state)}};
}

EvidenceWorker::EvidenceWorker(std::unique_ptr<State> initial) noexcept
    : state{std::move(initial)} {}
EvidenceWorker::EvidenceWorker(EvidenceWorker&& other) noexcept = default;
EvidenceWorker& EvidenceWorker::operator=(EvidenceWorker&& other) noexcept = default;
EvidenceWorker::~EvidenceWorker() = default;

void EvidenceWorker::run(EvidenceHub& hub) noexcept {
    EvidenceJob job;
    while (true) {
        const TakeResult result{hub.queue.takeFor(job, housekeepingPeriod)};
        if (result == TakeResult::Closed) {
            break;
        }
        if (result == TakeResult::Timeout) {
            state->housekeeping(true);
        } else if (job.kind == EvidenceKind::Images) {
            state->saveImages(*hub.slots.at(job.slot));
        } else {
            state->saveClip(hub, job);
        }
    }
}

const EvidenceCounts& EvidenceWorker::counts() const noexcept {
    return state->counts;
}

std::uint64_t EvidenceWorker::storedBytes() const noexcept {
    return state->retention.totalBytes();
}
