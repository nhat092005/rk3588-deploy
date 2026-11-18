#ifndef VIOLATION_RULE_H
#define VIOLATION_RULE_H

#include <cstddef>
#include <optional>
#include <vector>

#include "byte_tracker.h"
#include "logger.h"
#include "postprocess.h"

/** @brief Class index of Person in the model of this project (Helmet, NoHelmet, NoVest, Vest,
 *         Person). */
constexpr int defaultPersonClass{4};
/** @brief Class index of NoHelmet. */
constexpr int defaultNoHelmetClass{1};
/** @brief Class index of NoVest. */
constexpr int defaultNoVestClass{2};
/** @brief A NoHelmet or NoVest box needs at least this confidence to count. */
constexpr float defaultViolationConfidence{0.25F};
/** @brief A violation is reported after it has lasted this long, in seconds. */
constexpr double defaultHoldSeconds{1.0};
/** @brief A break in the violation up to this long, in seconds, does not end it. The value is a
 *         choice of this project, not a measurement: the detector misses a box now and then. */
constexpr double defaultGapSeconds{0.5};
/** @brief Most persons per frame, and most violations followed at once. */
constexpr std::size_t defaultRuleCapacity{128};

/** @brief Classes and times of the violation rule. */
struct ViolationConfig {
    int personClass{defaultPersonClass};
    int noHelmetClass{defaultNoHelmetClass};
    int noVestClass{defaultNoVestClass};
    float minConfidence{defaultViolationConfidence};
    double holdSeconds{defaultHoldSeconds};
    double gapSeconds{defaultGapSeconds};
    /** Persons beyond it in a frame, and violations beyond it at once, are ignored. */
    std::size_t capacity{defaultRuleCapacity};
};

/** @brief A person whose violation has lasted holdSeconds. Reported once per violation. */
struct ViolationEvent {
    /** Identifier of the Person track. */
    int trackId{0};
    /** Which boxes were attached to the person in the frame that reported it. */
    bool noHelmet{false};
    bool noVest{false};
    /** Time of the first frame of the violation, in seconds. */
    double startSeconds{0.0};
    /** Time of the frame that reported it, in seconds. */
    double nowSeconds{0.0};
    /** Box of the person in that frame, in the coordinates of the detections. */
    float left{0.0F};
    float top{0.0F};
    float right{0.0F};
    float bottom{0.0F};
};

/**
 * @brief Copies the detections of one class.
 * @param all The detections of a frame.
 * @param classId Class to keep.
 * @param selected Receives the copies in order, its old content is dropped. Reserve it once and
 *        the call allocates nothing.
 */
void selectClass(const std::vector<Detection>& all, int classId,
                 std::vector<Detection>& selected) noexcept;

/**
 * @brief The violation rule of the project: a NoHelmet or NoVest box attached to a Person that
 *        lasts holdSeconds is reported. Time comes from the caller, so the rule is deterministic.
 *
 * A box is attached to a Person when its centre is inside the Person box. When several Persons
 * contain the centre, the one that holds the largest share of the box wins, then the smaller
 * Person box, then the first. A violation lasts while the person has an attached box at least
 * every gapSeconds. After it is reported it is not reported again until it has ended, that is
 * until no box was attached for more than gapSeconds. Persons come from a tracker that sees only
 * the Person class (selectClass), so a track identifier is one person. All storage is allocated
 * by create(): update() allocates nothing.
 */
class ViolationRule {
public:
    /**
     * @brief Checks the configuration and allocates the storage.
     * @param config Classes, times and capacity. The three classes differ and are not negative,
     *        the confidence is in [0, 1], holdSeconds is above 0, gapSeconds is not negative, the
     *        capacity is above 0.
     * @param log Logger that the object keeps for the rest of its life.
     * @return The rule, or empty when the configuration is invalid, which is logged.
     */
    static std::optional<ViolationRule> create(const ViolationConfig& config, const Logger& log);

    /**
     * @brief Processes one frame.
     * @param seconds Time of the frame. It never goes back between calls.
     * @param persons The tracks of the Person class of the frame, for example
     *        ByteTracker::update() on selectClass() of the detections.
     * @param detections The detections of the frame of all classes. Only NoHelmet and NoVest at
     *        or above minConfidence are used.
     * @return The violations that reached holdSeconds in this frame, valid until the next call.
     *
     * Call it for every frame, also when there is no person.
     */
    const std::vector<ViolationEvent>& update(double seconds,
                                              const std::vector<TrackedObject>& persons,
                                              const std::vector<Detection>& detections) noexcept;

    /** @brief Forgets all violations that are being followed. */
    void reset() noexcept;

    /** @brief Whether the Person track has a violation that was reported and has not ended. */
    bool isViolating(int trackId) const noexcept;

    /** @brief Violations being followed now: started, not ended, reported or not. */
    std::size_t openCount() const noexcept {
        return episodes.size();
    }

    /**
     * @brief Persons and violations dropped since create() because the capacity was full. Zero
     *        in normal use; a warning is logged the first time it is not.
     */
    std::size_t overflowCount() const noexcept {
        return overflow;
    }

private:
    // The boxes attached to one person in a frame.
    struct Attached {
        bool noHelmet{false};
        bool noVest{false};
    };
    // A violation that is being followed.
    struct Episode {
        int trackId{0};
        double startSeconds{0.0};
        double lastSeconds{0.0};
        bool reported{false};
    };

    ViolationRule(const ViolationConfig& config, const Logger& logger);

    void attachBoxes(const std::vector<TrackedObject>& persons, std::size_t count,
                     const std::vector<Detection>& detections) noexcept;
    void advance(double seconds, const TrackedObject& person, const Attached& boxes) noexcept;
    void dropEnded(double seconds) noexcept;
    void countOverflow(std::size_t dropped) noexcept;

    ViolationConfig settings;
    Logger log;
    std::vector<Attached> attached;
    std::vector<Episode> episodes;
    std::vector<ViolationEvent> events;
    std::size_t overflow{0};
};

#endif // VIOLATION_RULE_H
