#include "byte_tracker.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

#include "kalman_filter.h"
#include "lap_solver.h"

namespace {
// Fixed values of Ultralytics BYTETracker that bytetrack.yaml does not expose.
constexpr double secondMatchThreshold{0.5};
constexpr double unconfirmedMatchThreshold{0.7};
constexpr float duplicateThreshold{0.15F};
// `eps` of bbox_ioa.
constexpr float iouEpsilon{1e-7F};
// Divisions by 2 of Ultralytics, written as products (exact in binary floating point).
constexpr float halfFloat{0.5F};
constexpr double halfDouble{0.5};
// Index of the height velocity in the mean of the filter (vh).
constexpr std::size_t heightVelocityIndex{7};

// left, top, right, bottom.
using Corners = std::array<float, 4>;

enum class Phase : std::uint8_t { New, Tracked, Lost, Removed };

struct Track {
    int id{0};
    Phase phase{Phase::New};
    bool confirmed{false};
    float score{0.0F};
    int classId{0};
    int detectionIndex{0};
    int frameId{0};
    int startFrame{0};
    // In the removed history of Ultralytics: it is dropped from the lost list at the next merge.
    bool inRemovedHistory{false};
    KalmanState kalman;
};

// A detection as BYTETracker holds it: a box that is not a track yet (STrack without mean).
struct DetectionBox {
    // left, top, width, height, rounded to float32 like `_tlwh`.
    std::array<float, 4> tlwh{};
    float score{0.0F};
    int classId{0};
    int index{0};
};

// tlwh_to_xyah in float32, the measurement of the filter.
Measurement toMeasurement(const std::array<float, 4>& tlwh) noexcept {
    const float centerX{tlwh[0] + (tlwh[2] * halfFloat)};
    const float centerY{tlwh[1] + (tlwh[3] * halfFloat)};
    const float aspect{tlwh[2] / tlwh[3]};
    return Measurement{static_cast<double>(centerX), static_cast<double>(centerY),
                       static_cast<double>(aspect), static_cast<double>(tlwh[3])};
}

// `xyxy` of an STrack that has no mean, float32.
Corners cornersOf(const DetectionBox& box) noexcept {
    return Corners{box.tlwh[0], box.tlwh[1], box.tlwh[2] + box.tlwh[0], box.tlwh[3] + box.tlwh[1]};
}

// `xyxy` of an STrack with a mean: float64, then cast to float32 by iou_distance.
Corners cornersOf(const KalmanState& state) noexcept {
    const double height{state.mean[3]};
    const double width{state.mean[2] * height};
    const double left{state.mean[0] - (width * halfDouble)};
    const double top{state.mean[1] - (height * halfDouble)};
    return Corners{static_cast<float>(left), static_cast<float>(top),
                   static_cast<float>(width + left), static_cast<float>(height + top)};
}

// 1 - IoU in float32, the operations of bbox_ioa(iou=True).
float iouDistance(const Corners& first, const Corners& second) noexcept {
    const float width{
        std::max(std::min(first[2], second[2]) - std::max(first[0], second[0]), 0.0F)};
    const float height{
        std::max(std::min(first[3], second[3]) - std::max(first[1], second[1]), 0.0F)};
    const float intersection{width * height};
    const float secondArea{(second[2] - second[0]) * (second[3] - second[1])};
    const float firstArea{(first[2] - first[0]) * (first[3] - first[1])};
    const float unionArea{(secondArea + firstArea) - intersection};
    return 1.0F - (intersection / (unionArea + iouEpsilon));
}

template <typename Predicate> void keepIf(std::vector<int>& list, Predicate keep) noexcept {
    list.erase(std::remove_if(list.begin(), list.end(), [&keep](int slot) { return !keep(slot); }),
               list.end());
}

// Keeps the entries of `list` whose flag in `drop` (same index) is 0.
void compact(std::vector<int>& list, const std::vector<char>& drop) noexcept {
    std::size_t kept{0};
    for (std::size_t i = 0; i < list.size(); ++i) {
        if (drop[i] == 0) {
            list[kept++] = list[i];
        }
    }
    list.resize(kept);
}
} // namespace

struct TrackerState {
    TrackerState(const ByteTrackerConfig& config, Logger logger);

    const std::vector<TrackedObject>& update(const std::vector<Detection>& input) noexcept;
    void reset() noexcept;

    struct Assignment {
        std::vector<std::pair<int, int>> matches;
        std::vector<int> unmatchedRows;
        std::vector<int> unmatchedColumns;
    };

    void splitDetections(const std::vector<Detection>& input) noexcept;
    void predictPool() noexcept;
    void firstAssociation() noexcept;
    void secondAssociation() noexcept;
    void unconfirmedAssociation() noexcept;
    void openNewTracks() noexcept;
    void markStaleTracks() noexcept;
    void mergePools() noexcept;
    void removeDuplicates() noexcept;
    void releaseUnusedSlots() noexcept;
    void formatOutput() noexcept;

    void markOverflow(const char* what) noexcept;
    int allocateSlot() noexcept;
    void applyMatch(int slot, const DetectionBox& detection) noexcept;
    void fillRowsWithTracks(const std::vector<int>& slots) noexcept;
    void fillColumnsWithDetections(const std::vector<DetectionBox>& source,
                                   const std::vector<int>& chosen) noexcept;
    void computeCosts(bool fuse) noexcept;
    void associate(double limit) noexcept;
    void joinInto(std::vector<int>& list, const std::vector<int>& more) noexcept;
    void subtract(std::vector<int>& list, const std::vector<int>& other) noexcept;

    ByteTrackerConfig settings;
    Logger log;
    LapSolver solver;
    bool overflowLogged{false};
    std::size_t overflows{0};
    int frameId{0};
    int lastId{0};
    std::uint32_t epoch{0};

    std::vector<Track> tracks;
    std::vector<char> inUse;
    std::vector<std::uint32_t> marks;
    std::vector<int> freeSlots;

    // Slot lists, in the order of Ultralytics.
    std::vector<int> tracked;
    std::vector<int> lost;
    std::vector<int> pool;
    std::vector<int> confirmedTracks;
    std::vector<int> unconfirmedTracks;
    std::vector<int> secondTracks;
    std::vector<int> activated;
    std::vector<int> refound;
    std::vector<int> newlyLost;
    std::vector<int> removedNow;

    std::vector<DetectionBox> highDetections;
    std::vector<DetectionBox> lowDetections;
    std::vector<int> remaining;
    std::vector<int> picks;

    std::vector<Corners> rowCorners;
    std::vector<Corners> columnCorners;
    std::vector<float> columnScores;
    std::vector<float> costs;
    Assignment assignment;
    Assignment firstResult;
    std::vector<char> dropTracked;
    std::vector<char> dropLost;

    std::vector<TrackedObject> output;
};

TrackerState::TrackerState(const ByteTrackerConfig& config, Logger logger)
    : settings{config}, log{logger},
      solver{LapCapacity{config.trackCapacity, config.detectionCapacity}} {
    const std::size_t slots{config.trackCapacity};
    const std::size_t columns{std::max(config.detectionCapacity, slots)};
    tracks.resize(slots);
    inUse.assign(slots, 0);
    marks.assign(slots, 0);
    freeSlots.reserve(slots);
    for (std::size_t slot = slots; slot > 0; --slot) {
        freeSlots.push_back(static_cast<int>(slot - 1));
    }
    for (auto* list :
         {&tracked, &lost, &pool, &confirmedTracks, &unconfirmedTracks, &secondTracks}) {
        list->reserve(2 * slots);
    }
    for (auto* list : {&activated, &refound, &newlyLost, &removedNow}) {
        list->reserve(3 * slots);
    }
    highDetections.reserve(config.detectionCapacity);
    lowDetections.reserve(config.detectionCapacity);
    remaining.reserve(columns);
    picks.reserve(columns);
    rowCorners.reserve(slots);
    columnCorners.reserve(columns);
    columnScores.reserve(columns);
    costs.assign(slots * columns, 0.0F);
    for (auto* result : {&assignment, &firstResult}) {
        result->matches.reserve(slots);
        result->unmatchedRows.reserve(slots);
        result->unmatchedColumns.reserve(columns);
    }
    dropTracked.assign(2 * slots, 0);
    dropLost.assign(2 * slots, 0);
    output.reserve(slots);
}

void TrackerState::reset() noexcept {
    frameId = 0;
    lastId = 0;
    freeSlots.clear();
    for (std::size_t slot = settings.trackCapacity; slot > 0; --slot) {
        freeSlots.push_back(static_cast<int>(slot - 1));
    }
    std::fill(inUse.begin(), inUse.end(), static_cast<char>(0));
    for (auto* list : {&tracked, &lost, &pool, &confirmedTracks, &unconfirmedTracks, &secondTracks,
                       &activated, &refound, &newlyLost, &removedNow}) {
        list->clear();
    }
    output.clear();
}

void TrackerState::markOverflow(const char* what) noexcept {
    ++overflows;
    if (!overflowLogged) {
        overflowLogged = true;
        log.warn("tracker capacity full, dropping ", what, " (counted, logged once)");
    }
}

int TrackerState::allocateSlot() noexcept {
    if (freeSlots.empty()) {
        return -1;
    }
    const int slot{freeSlots.back()};
    freeSlots.pop_back();
    inUse[slot] = 1;
    tracks[slot] = Track{};
    return slot;
}

// joint_stracks: appends the entries of `more` whose slot is not in `list`.
void TrackerState::joinInto(std::vector<int>& list, const std::vector<int>& more) noexcept {
    ++epoch;
    for (const int slot : list) {
        marks[slot] = epoch;
    }
    for (const int slot : more) {
        if (marks[slot] != epoch) {
            list.push_back(slot);
        }
    }
}

// sub_stracks: removes from `list` the slots that are in `other`.
void TrackerState::subtract(std::vector<int>& list, const std::vector<int>& other) noexcept {
    ++epoch;
    for (const int slot : other) {
        marks[slot] = epoch;
    }
    keepIf(list, [this](int slot) { return marks[slot] != epoch; });
}

void TrackerState::splitDetections(const std::vector<Detection>& input) noexcept {
    highDetections.clear();
    lowDetections.clear();
    if (input.size() > settings.detectionCapacity) {
        markOverflow("detections");
    }
    const std::size_t count{std::min(input.size(), settings.detectionCapacity)};
    for (std::size_t k = 0; k < count; ++k) {
        const Detection& detection{input[k]};
        // Boxes.xywh in float32, then xywh2ltwh in float64 and a cast to float32.
        const float centerX{(detection.left + detection.right) * halfFloat};
        const float centerY{(detection.top + detection.bottom) * halfFloat};
        const float width{detection.right - detection.left};
        const float height{detection.bottom - detection.top};
        if (!(width > 0.0F && height > 0.0F)) {
            continue;
        }
        DetectionBox box;
        box.tlwh = {static_cast<float>(static_cast<double>(centerX) -
                                       (static_cast<double>(width) * halfDouble)),
                    static_cast<float>(static_cast<double>(centerY) -
                                       (static_cast<double>(height) * halfDouble)),
                    width, height};
        box.score = detection.confidence;
        box.classId = detection.classId;
        box.index = static_cast<int>(k);
        if (detection.confidence >= settings.highThreshold) {
            highDetections.push_back(box);
        } else if (detection.confidence > settings.lowThreshold &&
                   detection.confidence < settings.highThreshold) {
            lowDetections.push_back(box);
        }
    }
}

void TrackerState::predictPool() noexcept {
    for (const int slot : pool) {
        Track& track{tracks[slot]};
        if (track.phase != Phase::Tracked) {
            track.kalman.mean[heightVelocityIndex] = 0.0;
        }
        kalman::predict(track.kalman);
    }
}

void TrackerState::fillRowsWithTracks(const std::vector<int>& slots) noexcept {
    rowCorners.clear();
    for (const int slot : slots) {
        rowCorners.push_back(cornersOf(tracks[slot].kalman));
    }
}

void TrackerState::fillColumnsWithDetections(const std::vector<DetectionBox>& source,
                                             const std::vector<int>& chosen) noexcept {
    columnCorners.clear();
    columnScores.clear();
    for (const int index : chosen) {
        columnCorners.push_back(cornersOf(source[index]));
        columnScores.push_back(source[index].score);
    }
}

// iou_distance, then fuse_score when asked, over rowCorners x columnCorners.
void TrackerState::computeCosts(bool fuse) noexcept {
    const std::size_t columns{columnCorners.size()};
    for (std::size_t row = 0; row < rowCorners.size(); ++row) {
        for (std::size_t column = 0; column < columns; ++column) {
            float cost{iouDistance(rowCorners[row], columnCorners[column])};
            if (fuse) {
                cost = 1.0F - ((1.0F - cost) * columnScores[column]);
            }
            costs[(row * columns) + column] = cost;
        }
    }
}

// linear_assignment of Ultralytics over the costs of the current rows and columns.
void TrackerState::associate(double limit) noexcept {
    const std::size_t rows{rowCorners.size()};
    const std::size_t columns{columnCorners.size()};
    assignment.matches.clear();
    assignment.unmatchedRows.clear();
    assignment.unmatchedColumns.clear();
    if (!solver.solve(CostMatrix{costs.data(), rows, columns}, limit)) {
        log.error("tracker matrix ", rows, "x", columns, " exceeds the solver capacity");
    }
    for (std::size_t row = 0; row < rows; ++row) {
        const int column{solver.columnOfRow(row)};
        if (column >= 0) {
            assignment.matches.emplace_back(static_cast<int>(row), column);
        } else {
            assignment.unmatchedRows.push_back(static_cast<int>(row));
        }
    }
    for (std::size_t column = 0; column < columns; ++column) {
        if (solver.rowOfColumn(column) < 0) {
            assignment.unmatchedColumns.push_back(static_cast<int>(column));
        }
    }
}

// STrack.update and re_activate: the track follows the detection.
void TrackerState::applyMatch(int slot, const DetectionBox& detection) noexcept {
    Track& track{tracks[slot]};
    kalman::update(track.kalman, toMeasurement(detection.tlwh));
    track.frameId = frameId;
    track.phase = Phase::Tracked;
    track.confirmed = true;
    track.score = detection.score;
    track.classId = detection.classId;
    track.detectionIndex = detection.index;
}

void TrackerState::firstAssociation() noexcept {
    picks.clear();
    for (std::size_t i = 0; i < highDetections.size(); ++i) {
        picks.push_back(static_cast<int>(i));
    }
    fillRowsWithTracks(pool);
    fillColumnsWithDetections(highDetections, picks);
    computeCosts(settings.fuseScore);
    associate(settings.matchThreshold);
    for (const auto& [row, column] : assignment.matches) {
        const int slot{pool[row]};
        const bool wasTracked{tracks[slot].phase == Phase::Tracked};
        applyMatch(slot, highDetections[column]);
        (wasTracked ? activated : refound).push_back(slot);
    }
    firstResult.unmatchedRows = assignment.unmatchedRows;
    firstResult.unmatchedColumns = assignment.unmatchedColumns;
}

void TrackerState::secondAssociation() noexcept {
    secondTracks.clear();
    for (const int row : firstResult.unmatchedRows) {
        if (tracks[pool[row]].phase == Phase::Tracked) {
            secondTracks.push_back(pool[row]);
        }
    }
    if (!secondTracks.empty() && !lowDetections.empty()) {
        picks.clear();
        for (std::size_t i = 0; i < lowDetections.size(); ++i) {
            picks.push_back(static_cast<int>(i));
        }
        fillRowsWithTracks(secondTracks);
        fillColumnsWithDetections(lowDetections, picks);
        computeCosts(false);
        associate(secondMatchThreshold);
        for (const auto& [row, column] : assignment.matches) {
            applyMatch(secondTracks[row], lowDetections[column]);
            activated.push_back(secondTracks[row]);
        }
    } else {
        assignment.unmatchedRows.clear();
        for (std::size_t i = 0; i < secondTracks.size(); ++i) {
            assignment.unmatchedRows.push_back(static_cast<int>(i));
        }
    }
    for (const int row : assignment.unmatchedRows) {
        Track& track{tracks[secondTracks[row]]};
        if (track.phase != Phase::Lost) {
            track.phase = Phase::Lost;
            newlyLost.push_back(secondTracks[row]);
        }
    }
}

void TrackerState::unconfirmedAssociation() noexcept {
    remaining.clear();
    for (const int column : firstResult.unmatchedColumns) {
        remaining.push_back(column);
    }
    if (unconfirmedTracks.empty()) {
        return;
    }
    fillRowsWithTracks(unconfirmedTracks);
    fillColumnsWithDetections(highDetections, remaining);
    computeCosts(settings.fuseScore);
    associate(unconfirmedMatchThreshold);
    for (const auto& [row, column] : assignment.matches) {
        applyMatch(unconfirmedTracks[row], highDetections[remaining[column]]);
        activated.push_back(unconfirmedTracks[row]);
    }
    for (const int row : assignment.unmatchedRows) {
        tracks[unconfirmedTracks[row]].phase = Phase::Removed;
        removedNow.push_back(unconfirmedTracks[row]);
    }
    // What is left of the detections, as indexes of the high detections.
    picks.clear();
    for (const int column : assignment.unmatchedColumns) {
        picks.push_back(remaining[column]);
    }
    remaining = picks;
}

void TrackerState::openNewTracks() noexcept {
    for (const int index : remaining) {
        const DetectionBox& detection{highDetections[index]};
        if (detection.score < settings.newTrackThreshold) {
            continue;
        }
        const int slot{allocateSlot()};
        if (slot < 0) {
            markOverflow("new tracks");
            continue;
        }
        Track& track{tracks[slot]};
        track.id = ++lastId;
        track.kalman = kalman::initiate(toMeasurement(detection.tlwh));
        track.phase = Phase::Tracked;
        track.confirmed = frameId == 1;
        track.frameId = frameId;
        track.startFrame = frameId;
        track.score = detection.score;
        track.classId = detection.classId;
        track.detectionIndex = detection.index;
        activated.push_back(slot);
    }
}

void TrackerState::markStaleTracks() noexcept {
    for (const int slot : lost) {
        Track& track{tracks[slot]};
        if (frameId - track.frameId > settings.trackBuffer) {
            track.phase = Phase::Removed;
            removedNow.push_back(slot);
        }
    }
}

// remove_duplicate_stracks: of two tracks that overlap by more than 85% the younger is dropped.
void TrackerState::removeDuplicates() noexcept {
    if (tracked.empty() || lost.empty()) {
        return;
    }
    fillRowsWithTracks(tracked);
    columnCorners.clear();
    for (const int slot : lost) {
        columnCorners.push_back(cornersOf(tracks[slot].kalman));
    }
    computeCosts(false);
    std::fill(dropTracked.begin(),
              dropTracked.begin() + static_cast<std::ptrdiff_t>(tracked.size()), 0);
    std::fill(dropLost.begin(), dropLost.begin() + static_cast<std::ptrdiff_t>(lost.size()), 0);
    const std::size_t columns{lost.size()};
    for (std::size_t trackedIndex = 0; trackedIndex < tracked.size(); ++trackedIndex) {
        for (std::size_t lostIndex = 0; lostIndex < columns; ++lostIndex) {
            if (costs[(trackedIndex * columns) + lostIndex] < duplicateThreshold) {
                const Track& first{tracks[tracked[trackedIndex]]};
                const Track& second{tracks[lost[lostIndex]]};
                if (first.frameId - first.startFrame > second.frameId - second.startFrame) {
                    dropLost[lostIndex] = 1;
                } else {
                    dropTracked[trackedIndex] = 1;
                }
            }
        }
    }
    compact(tracked, dropTracked);
    compact(lost, dropLost);
}

// merge_track_pools: the bookkeeping at the end of a frame.
void TrackerState::mergePools() noexcept {
    keepIf(tracked, [this](int slot) { return tracks[slot].phase == Phase::Tracked; });
    joinInto(tracked, activated);
    joinInto(tracked, refound);
    subtract(lost, tracked);
    lost.insert(lost.end(), newlyLost.begin(), newlyLost.end());
    keepIf(lost, [this](int slot) { return !tracks[slot].inRemovedHistory; });
    removeDuplicates();
    for (const int slot : removedNow) {
        tracks[slot].inRemovedHistory = true;
    }
}

void TrackerState::releaseUnusedSlots() noexcept {
    ++epoch;
    for (const int slot : tracked) {
        marks[slot] = epoch;
    }
    for (const int slot : lost) {
        marks[slot] = epoch;
    }
    for (std::size_t slot = 0; slot < inUse.size(); ++slot) {
        if (inUse[slot] != 0 && marks[slot] != epoch) {
            inUse[slot] = 0;
            freeSlots.push_back(static_cast<int>(slot));
        }
    }
}

void TrackerState::formatOutput() noexcept {
    output.clear();
    for (const int slot : tracked) {
        const Track& track{tracks[slot]};
        if (!track.confirmed) {
            continue;
        }
        const Corners corners{cornersOf(track.kalman)};
        output.push_back(TrackedObject{corners[0], corners[1], corners[2], corners[3], track.id,
                                       track.score, track.classId, track.detectionIndex});
    }
}

const std::vector<TrackedObject>&
TrackerState::update(const std::vector<Detection>& input) noexcept {
    ++frameId;
    activated.clear();
    refound.clear();
    newlyLost.clear();
    removedNow.clear();

    splitDetections(input);

    confirmedTracks.clear();
    unconfirmedTracks.clear();
    for (const int slot : tracked) {
        (tracks[slot].confirmed ? confirmedTracks : unconfirmedTracks).push_back(slot);
    }
    pool = confirmedTracks;
    joinInto(pool, lost);
    predictPool();

    firstAssociation();
    secondAssociation();
    unconfirmedAssociation();
    openNewTracks();
    markStaleTracks();
    mergePools();
    releaseUnusedSlots();
    formatOutput();
    return output;
}

std::optional<ByteTracker> ByteTracker::create(const ByteTrackerConfig& config, const Logger& log) {
    if (config.trackBuffer < 1 || config.trackCapacity == 0 || config.detectionCapacity == 0 ||
        !(config.lowThreshold < config.highThreshold)) {
        log.error("bad tracker config: trackBuffer=", config.trackBuffer,
                  " trackCapacity=", config.trackCapacity,
                  " detectionCapacity=", config.detectionCapacity, " low=", config.lowThreshold,
                  " high=", config.highThreshold);
        return std::nullopt;
    }
    return std::optional<ByteTracker>{ByteTracker{std::make_unique<TrackerState>(config, log)}};
}

ByteTracker::ByteTracker(std::unique_ptr<TrackerState> initial) noexcept
    : state{std::move(initial)} {}

ByteTracker::ByteTracker(ByteTracker&& other) noexcept = default;

ByteTracker& ByteTracker::operator=(ByteTracker&& other) noexcept = default;

ByteTracker::~ByteTracker() = default;

const std::vector<TrackedObject>&
ByteTracker::update(const std::vector<Detection>& detections) noexcept {
    return state->update(detections);
}

void ByteTracker::reset() noexcept {
    state->reset();
}

std::size_t ByteTracker::overflowCount() const noexcept {
    return state->overflows;
}
