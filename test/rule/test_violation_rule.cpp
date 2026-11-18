// The violation rule: which Person a NoHelmet or NoVest box is attached to, when a violation is
// reported, and what ends it. Hand-made cases with times that are exact in binary floating point
// (multiples of 0.25 s). Exit code 0 when everything holds.
#include <cstddef>
#include <cstdlib>
#include <optional>
#include <utility>
#include <vector>

#include "logger.h"
#include "violation_rule.h"

namespace {
constexpr int personClass{4};
constexpr int noHelmetClass{1};
constexpr int noVestClass{2};
constexpr int helmetClass{0};
constexpr int vestClass{3};
constexpr float strong{0.9F};
constexpr float weak{0.2F};
constexpr float boxHalfSide{20.0F};
constexpr double step{0.25}; // seconds between frames
// The values of the project: the tests run the rule as it is configured by default.
constexpr double holdTime{defaultHoldSeconds};
constexpr double gapTime{defaultGapSeconds};
constexpr int holdFrames{static_cast<int>(holdTime / step)}; // frames until the report
constexpr std::size_t smallCapacity{4};
constexpr float bigConfidence{1.5F};

// The default person: x in [100, 300], y in [100, 500].
constexpr float personLeft{100.0F};
constexpr float personTop{100.0F};
constexpr float personRight{300.0F};
constexpr float personBottom{500.0F};
constexpr float headX{200.0F};
constexpr float headY{150.0F};
constexpr float chestY{300.0F};
constexpr float outsideX{400.0F};

constexpr int firstPerson{1};
constexpr int secondPerson{2};
constexpr int thirdPerson{3};
constexpr int fourthPerson{4};
constexpr int trackedPerson{7};

TrackedObject person(int trackId, float left = personLeft, float right = personRight) {
    return TrackedObject{left, personTop, right, personBottom, trackId, strong, personClass, 0};
}

// A box of `classId` with the centre at (centreX, centreY), 40 by 40.
Detection boxAt(int classId, float centreX, float centreY, float confidence = strong) {
    return Detection{centreX - boxHalfSide,
                     centreY - boxHalfSide,
                     centreX + boxHalfSide,
                     centreY + boxHalfSide,
                     confidence,
                     classId};
}

class Check {
public:
    explicit Check(const Logger& logger) : log{logger} {}

    void expect(bool condition, const char* what) {
        if (!condition) {
            log.error("FAIL: ", what);
            passed = false;
        }
    }

    bool ok() const {
        return passed;
    }

private:
    Logger log;
    bool passed{true};
};

// Builds a rule from a configuration that the test knows is valid.
ViolationRule makeRule(const Logger& log, const ViolationConfig& config = {}) {
    std::optional<ViolationRule> rule{ViolationRule::create(config, log)};
    if (!rule) {
        std::abort();
    }
    return std::move(*rule);
}

// The reports of frames 0 to `frames` of the same persons and boxes.
std::vector<ViolationEvent> runFrames(ViolationRule& rule,
                                      const std::vector<TrackedObject>& persons,
                                      const std::vector<Detection>& boxes, int frames) {
    std::vector<ViolationEvent> reported;
    for (int frame = 0; frame <= frames; ++frame) {
        const std::vector<ViolationEvent>& events{
            rule.update(static_cast<double>(frame) * step, persons, boxes)};
        reported.insert(reported.end(), events.begin(), events.end());
    }
    return reported;
}

// Report times of a rule that sees a NoHelmet box on person 1 at the given times only.
std::vector<double> reportTimes(ViolationRule& rule, const std::vector<double>& times) {
    const std::vector<TrackedObject> persons{person(firstPerson)};
    const std::vector<Detection> boxes{boxAt(noHelmetClass, headX, headY)};
    std::vector<double> reported;
    for (const double seconds : times) {
        for (const ViolationEvent& event : rule.update(seconds, persons, boxes)) {
            reported.push_back(event.nowSeconds);
        }
    }
    return reported;
}

// Times from `first` in steps of `step`, `count` of them.
std::vector<double> frameTimes(double first, int count) {
    std::vector<double> times(static_cast<std::size_t>(count));
    for (int frame = 0; frame < count; ++frame) {
        times[static_cast<std::size_t>(frame)] = first + (static_cast<double>(frame) * step);
    }
    return times;
}

void checkSelect(Check& check) {
    const std::vector<Detection> all{boxAt(personClass, 1.0F, 1.0F), boxAt(noVestClass, 2.0F, 2.0F),
                                     boxAt(personClass, 3.0F, 3.0F)};
    std::vector<Detection> selected{boxAt(helmetClass, headX, headY)};
    selectClass(all, personClass, selected);
    check.expect(selected.size() == 2 && selected[0].left == all[0].left &&
                     selected[1].left == all[2].left,
                 "selectClass keeps the Person boxes in order and drops the old content");
    selectClass(all, vestClass, selected);
    check.expect(selected.empty(), "selectClass of an absent class is empty");
}

// How many reports the boxes give on one person in a hold time.
std::size_t reportsOf(const Logger& log, const std::vector<Detection>& boxes) {
    ViolationRule rule{makeRule(log)};
    return runFrames(rule, {person(firstPerson)}, boxes, holdFrames).size();
}

void checkAttach(Check& check, const Logger& log) {
    check.expect(reportsOf(log, {boxAt(noHelmetClass, headX, headY)}) == 1, "NoHelmet inside");
    check.expect(reportsOf(log, {boxAt(noVestClass, headX, chestY)}) == 1, "NoVest inside");
    check.expect(reportsOf(log, {boxAt(noHelmetClass, outsideX, headY)}) == 0, "centre outside");
    check.expect(reportsOf(log, {boxAt(noHelmetClass, headX, headY, weak)}) == 0,
                 "below confidence");
    check.expect(reportsOf(log, {boxAt(noHelmetClass, headX, headY, defaultViolationConfidence)}) ==
                     1,
                 "at the confidence threshold");
    check.expect(reportsOf(log, {boxAt(helmetClass, headX, headY), boxAt(vestClass, headX, chestY),
                                 boxAt(personClass, headX, chestY)}) == 0,
                 "Helmet, Vest and Person are not violations");
}

// Which of two persons is reported for one box: the id of the only report, or 0.
int chosenPerson(const Logger& log, const TrackedObject& first, const TrackedObject& second,
                 const Detection& box) {
    ViolationRule rule{makeRule(log)};
    const std::vector<ViolationEvent> reported{runFrames(rule, {first, second}, {box}, holdFrames)};
    return reported.size() == 1 ? reported.front().trackId : 0;
}

void checkChoice(Check& check, const Logger& log) {
    // The box [230, 270] x [280, 320] has its centre in both persons: `wide` holds all of it,
    // `narrow` (x up to 260) holds 30 of its 40 width.
    constexpr float wideRight{400.0F};
    constexpr float narrowLeft{200.0F};
    constexpr float narrowRight{260.0F};
    constexpr float boxCentreX{250.0F};
    check.expect(chosenPerson(log, person(firstPerson, personLeft, wideRight),
                              person(secondPerson, narrowLeft, narrowRight),
                              boxAt(noVestClass, boxCentreX, chestY)) == firstPerson,
                 "the larger share of the box wins");
    // The whole box is inside both persons: the smaller person wins.
    constexpr float bigRight{500.0F};
    constexpr float smallLeft{150.0F};
    constexpr float smallRight{350.0F};
    check.expect(chosenPerson(log, person(thirdPerson, personLeft, bigRight),
                              person(fourthPerson, smallLeft, smallRight),
                              boxAt(noHelmetClass, boxCentreX, headY)) == fourthPerson,
                 "equal share: the smaller person wins");
}

void checkTiming(Check& check, const Logger& log) {
    ViolationRule rule{makeRule(log)};
    constexpr int frames{20};
    const std::vector<ViolationEvent> reported{
        runFrames(rule, {person(trackedPerson)},
                  {boxAt(noHelmetClass, headX, headY), boxAt(noVestClass, headX, chestY)}, frames)};
    check.expect(reported.size() == 1, "reported once");
    if (reported.empty()) {
        return;
    }
    const ViolationEvent& event{reported.front()};
    check.expect(event.nowSeconds == holdTime && event.startSeconds == 0.0,
                 "at exactly the hold time");
    check.expect(event.trackId == trackedPerson && event.noHelmet && event.noVest, "event flags");
    check.expect(event.left == personLeft && event.bottom == personBottom, "event person box");
    check.expect(rule.openCount() == 1, "a reported violation is still open");
    check.expect(rule.isViolating(trackedPerson), "the person of a reported violation violates");
    check.expect(!rule.isViolating(trackedPerson + 1), "another person does not");
    constexpr double longAfter{100.0};
    (void)rule.update(longAfter, {}, {});
    check.expect(rule.openCount() == 0, "an ended violation is not open");
    check.expect(!rule.isViolating(trackedPerson), "an ended violation does not count");
}

void checkViolating(Check& check, const Logger& log) {
    ViolationRule rule{makeRule(log)};
    const std::vector<TrackedObject> persons{person(firstPerson)};
    const std::vector<Detection> boxes{boxAt(noHelmetClass, headX, headY)};
    (void)rule.update(0.0, persons, boxes);
    check.expect(rule.openCount() == 1 && !rule.isViolating(firstPerson),
                 "a violation that is not reported yet is open but not violating");
    for (const double seconds : frameTimes(step, static_cast<int>(holdTime / step))) {
        (void)rule.update(seconds, persons, boxes);
    }
    check.expect(rule.isViolating(firstPerson), "violating once reported");
}

void checkGap(Check& check, const Logger& log) {
    constexpr double breakEnd{step + gapTime};   // exactly the gap after the box at `step`
    constexpr double longBreak{breakEnd + step}; // longer than the gap
    constexpr double secondStart{10.0};
    constexpr int episodeFrames{6};
    constexpr int continueFrames{3};
    constexpr int restartFrames{5};
    ViolationRule held{makeRule(log)};
    // A break of exactly the gap does not end the violation: it was started at 0 and is reported
    // at the first box after the break.
    std::vector<double> times{0.0, step};
    for (const double seconds : frameTimes(breakEnd, continueFrames)) {
        times.push_back(seconds);
    }
    const std::vector<double> continued{reportTimes(held, times)};
    check.expect(continued.size() == 1 && continued[0] == holdTime, "a break of the gap continues");

    // A longer break ends it: it starts again after the break and is reported a hold later.
    ViolationRule restarted{makeRule(log)};
    times = {0.0, step};
    for (const double seconds : frameTimes(longBreak, restartFrames)) {
        times.push_back(seconds);
    }
    const std::vector<double> again{reportTimes(restarted, times)};
    check.expect(again.size() == 1 && again[0] == longBreak + holdTime, "a longer break restarts");

    // After a report the same violation is not reported again, a new one is.
    ViolationRule twice{makeRule(log)};
    times = frameTimes(0.0, episodeFrames);
    for (const double seconds : frameTimes(secondStart, episodeFrames)) {
        times.push_back(seconds);
    }
    const std::vector<double> reported{reportTimes(twice, times)};
    check.expect(reported.size() == 2 && reported[0] == holdTime &&
                     reported[1] == secondStart + holdTime,
                 "one report per violation, re-armed after it ended");
}

void checkPersons(Check& check, const Logger& log) {
    constexpr float farLeft{600.0F};
    constexpr float farRight{800.0F};
    constexpr float farCentre{700.0F};
    ViolationRule rule{makeRule(log)};
    const std::vector<ViolationEvent> reported{
        runFrames(rule, {person(firstPerson), person(secondPerson, farLeft, farRight)},
                  {boxAt(noVestClass, farCentre, chestY)}, holdFrames)};
    check.expect(reported.size() == 1 && reported.front().trackId == secondPerson,
                 "only the person with the box is reported");
}

void checkCapacity(Check& check, const Logger& log) {
    constexpr int manyPersons{10};
    constexpr int crowdSize{6};
    constexpr double apart{4.0}; // seconds between two persons
    constexpr double laterTime{100.0};
    constexpr float crowdStep{300.0F};
    constexpr float crowdWidth{200.0F};
    constexpr int crowdFirstId{100};
    ViolationConfig config;
    config.capacity = smallCapacity;
    ViolationRule rule{makeRule(log, config)};
    // Persons one after the other, each seen once: ended violations are forgotten, so nothing
    // overflows.
    for (int trackId = 1; trackId <= manyPersons; ++trackId) {
        (void)rule.update(trackId * apart, {person(trackId)}, {boxAt(noHelmetClass, headX, headY)});
    }
    check.expect(rule.overflowCount() == 0, "ended violations free their slot");
    // Six persons in one frame with a capacity of four: two are ignored and counted.
    std::vector<TrackedObject> crowd;
    std::vector<Detection> boxes;
    for (int index = 0; index < crowdSize; ++index) {
        const float left{static_cast<float>(index) * crowdStep};
        crowd.push_back(person(crowdFirstId + index, left, left + crowdWidth));
        boxes.push_back(boxAt(noHelmetClass, left + (crowdWidth / 2), headY));
    }
    (void)rule.update(laterTime, crowd, boxes);
    check.expect(rule.overflowCount() == 2, "persons beyond the capacity are counted");
}

void checkConfig(Check& check, const Logger& log) {
    ViolationConfig bad;
    bad.noVestClass = bad.noHelmetClass;
    check.expect(!ViolationRule::create(bad, log), "equal classes refused");
    bad = ViolationConfig{};
    bad.holdSeconds = 0.0;
    check.expect(!ViolationRule::create(bad, log), "zero hold refused");
    bad = ViolationConfig{};
    bad.capacity = 0;
    check.expect(!ViolationRule::create(bad, log), "zero capacity refused");
    bad = ViolationConfig{};
    bad.minConfidence = bigConfidence;
    check.expect(!ViolationRule::create(bad, log), "confidence above 1 refused");
}
} // namespace

int main() {
    const Logger log{"TEST_RULE", LogLevel::Debug};
    Check check{log};
    checkSelect(check);
    checkAttach(check, log);
    checkChoice(check, log);
    checkTiming(check, log);
    checkViolating(check, log);
    checkGap(check, log);
    checkPersons(check, log);
    checkCapacity(check, log);
    checkConfig(check, log);
    if (check.ok()) {
        log.info("PASS");
        return EXIT_SUCCESS;
    }
    log.error("FAIL");
    return EXIT_FAILURE;
}
