#include "violation_rule.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <vector>

namespace {
constexpr float half{0.5F};
constexpr float noFraction{-1.0F};

// The Person that a violation box is attached to, with the numbers that chose it.
struct Candidate {
    int index{-1};
    float fraction{noFraction};
    float area{0.0F};
};

bool centreInside(const Detection& box, const TrackedObject& person) {
    const float centreX{(box.left + box.right) * half};
    const float centreY{(box.top + box.bottom) * half};
    return centreX >= person.left && centreX <= person.right && centreY >= person.top &&
           centreY <= person.bottom;
}

// Share of the box that lies inside the person box.
float insideFraction(const Detection& box, const TrackedObject& person) {
    const float width{std::min(box.right, person.right) - std::max(box.left, person.left)};
    const float height{std::min(box.bottom, person.bottom) - std::max(box.top, person.top)};
    const float own{(box.right - box.left) * (box.bottom - box.top)};
    if (width <= 0.0F || height <= 0.0F || own <= 0.0F) {
        return 0.0F;
    }
    return width * height / own;
}

// Largest share of the box first, then the smaller person box, then the earlier one.
bool better(const Candidate& challenger, const Candidate& best) {
    if (challenger.fraction != best.fraction) {
        return challenger.fraction > best.fraction;
    }
    return challenger.area < best.area;
}

int attachTo(const Detection& box, const std::vector<TrackedObject>& persons, std::size_t count) {
    Candidate best;
    for (std::size_t index = 0; index < count; ++index) {
        const TrackedObject& person{persons[index]};
        if (!centreInside(box, person)) {
            continue;
        }
        const Candidate challenger{static_cast<int>(index), insideFraction(box, person),
                                   (person.right - person.left) * (person.bottom - person.top)};
        if (better(challenger, best)) {
            best = challenger;
        }
    }
    return best.index;
}

bool configValid(const ViolationConfig& config) {
    const bool classesValid{config.personClass >= 0 && config.noHelmetClass >= 0 &&
                            config.noVestClass >= 0 && config.personClass != config.noHelmetClass &&
                            config.personClass != config.noVestClass &&
                            config.noHelmetClass != config.noVestClass};
    return classesValid && config.minConfidence >= 0.0F && config.minConfidence <= 1.0F &&
           config.holdSeconds > 0.0 && config.gapSeconds >= 0.0 && config.capacity > 0;
}
} // namespace

void selectClass(const std::vector<Detection>& all, int classId,
                 std::vector<Detection>& selected) noexcept {
    selected.clear();
    for (const Detection& detection : all) {
        if (detection.classId == classId) {
            selected.push_back(detection);
        }
    }
}

ViolationRule::ViolationRule(const ViolationConfig& config, const Logger& logger)
    : settings{config}, log{logger} {
    attached.reserve(config.capacity);
    episodes.reserve(config.capacity);
    events.reserve(config.capacity);
}

std::optional<ViolationRule> ViolationRule::create(const ViolationConfig& config,
                                                   const Logger& log) {
    if (!configValid(config)) {
        log.error("bad violation config: classes ", config.personClass, " ", config.noHelmetClass,
                  " ", config.noVestClass, " confidence ", config.minConfidence, " hold ",
                  config.holdSeconds, " gap ", config.gapSeconds, " capacity ", config.capacity);
        return std::nullopt;
    }
    return ViolationRule{config, log};
}

void ViolationRule::countOverflow(std::size_t dropped) noexcept {
    if (overflow == 0) {
        log.warn("violation rule capacity ", settings.capacity, " is full, some are ignored");
    }
    overflow += dropped;
}

void ViolationRule::attachBoxes(const std::vector<TrackedObject>& persons, std::size_t count,
                                const std::vector<Detection>& detections) noexcept {
    attached.assign(count, Attached{});
    for (const Detection& box : detections) {
        const bool helmet{box.classId == settings.noHelmetClass};
        const bool vest{box.classId == settings.noVestClass};
        if ((!helmet && !vest) || box.confidence < settings.minConfidence) {
            continue;
        }
        const int index{attachTo(box, persons, count)};
        if (index >= 0) {
            Attached& boxes{attached[static_cast<std::size_t>(index)]};
            boxes.noHelmet = boxes.noHelmet || helmet;
            boxes.noVest = boxes.noVest || vest;
        }
    }
}

void ViolationRule::advance(double seconds, const TrackedObject& person,
                            const Attached& boxes) noexcept {
    // dropEnded() ran first, so a violation found here has not ended.
    auto found{std::find_if(episodes.begin(), episodes.end(), [&person](const Episode& episode) {
        return episode.trackId == person.id;
    })};
    if (found == episodes.end()) {
        if (episodes.size() >= settings.capacity) {
            countOverflow(1);
            return;
        }
        episodes.push_back(Episode{person.id, seconds, seconds, false});
        found = episodes.end() - 1;
    }
    found->lastSeconds = seconds;
    if (!found->reported && seconds - found->startSeconds >= settings.holdSeconds) {
        found->reported = true;
        events.push_back(ViolationEvent{person.id, boxes.noHelmet, boxes.noVest,
                                        found->startSeconds, seconds, person.left, person.top,
                                        person.right, person.bottom});
    }
}

void ViolationRule::dropEnded(double seconds) noexcept {
    episodes.erase(std::remove_if(episodes.begin(), episodes.end(),
                                  [this, seconds](const Episode& episode) {
                                      return seconds - episode.lastSeconds > settings.gapSeconds;
                                  }),
                   episodes.end());
}

const std::vector<ViolationEvent>&
ViolationRule::update(double seconds, const std::vector<TrackedObject>& persons,
                      const std::vector<Detection>& detections) noexcept {
    events.clear();
    // First, so that an ended violation does not hold a slot that this frame needs.
    dropEnded(seconds);
    const std::size_t count{std::min(persons.size(), settings.capacity)};
    if (persons.size() > count) {
        countOverflow(persons.size() - count);
    }
    attachBoxes(persons, count, detections);
    for (std::size_t index = 0; index < count; ++index) {
        if (attached[index].noHelmet || attached[index].noVest) {
            advance(seconds, persons[index], attached[index]);
        }
    }
    return events;
}

void ViolationRule::reset() noexcept {
    episodes.clear();
    events.clear();
}

bool ViolationRule::isViolating(int trackId) const noexcept {
    return std::any_of(episodes.begin(), episodes.end(), [trackId](const Episode& episode) {
        return episode.trackId == trackId && episode.reported;
    });
}
