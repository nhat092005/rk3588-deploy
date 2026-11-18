// Time of ByteTracker::update with moving people (seeded, synthetic boxes), the C++ counterpart of
// the Python tracker measurement in docs section 10.
// Usage: probe_tracker [people] [frames]
//   people : boxes per frame, default 34
//   frames : timed frames after 30 warm-up frames, default 600
// Logs the mean, median, 99th percentile and maximum of update() in ms, the confirmed tracks of the
// last frame and the highest track id (equal to the number of people when no id is lost). Pin it
// like the application: `taskset -c 6 probe_tracker`. Exit code 0 when the tracker ran, 2 on bad
// arguments.
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <numeric>
#include <optional>
#include <vector>

#include "arguments.h"
#include "byte_tracker.h"
#include "logger.h"

namespace {
constexpr int defaultPeople{34};
constexpr int defaultFrames{600};
constexpr int warmupFrames{30};
constexpr long maxCount{100'000};
constexpr int exitUsage{2};
constexpr int decimalBase{10};
constexpr std::size_t peopleArg{1};
constexpr std::size_t framesArg{2};
constexpr std::size_t maxArgs{3};
constexpr float imageWidth{1920.0F};
constexpr float imageHeight{1080.0F};
constexpr double medianFraction{0.5};
constexpr double percentile99{0.99};
constexpr double millisecondsPerSecond{1000.0};
constexpr std::uint64_t seed{20261115};
// xorshift64 shifts.
constexpr unsigned shiftLeftFirst{13U};
constexpr unsigned shiftRight{7U};
constexpr unsigned shiftLeftSecond{17U};

// xorshift64: the same numbers on every machine.
class Random {
public:
    explicit Random(std::uint64_t start) : value{start} {}

    // Uniform in [low, high).
    float next(float low, float high) {
        value ^= value << shiftLeftFirst;
        value ^= value >> shiftRight;
        value ^= value << shiftLeftSecond;
        constexpr double scale{1.0 / 18446744073709551616.0};
        return low + (high - low) * static_cast<float>(static_cast<double>(value) * scale);
    }

private:
    std::uint64_t value;
};

struct Walker {
    float x{0.0F};
    float y{0.0F};
    float vx{0.0F};
    float vy{0.0F};
    float width{0.0F};
    float height{0.0F};
};

struct Options {
    int people{defaultPeople};
    int frames{defaultFrames};
};

std::optional<int> parseCount(const char* text) noexcept {
    char* end{nullptr};
    const long value{std::strtol(text, &end, decimalBase)};
    if (end == text || *end != '\0' || value <= 0 || value > maxCount) {
        return std::nullopt;
    }
    return static_cast<int>(value);
}

std::optional<Options> parseArgs(const std::vector<const char*>& args, const Logger& log) {
    Options options;
    if (args.size() > maxArgs) {
        log.error("usage: probe_tracker [people] [frames]");
        return std::nullopt;
    }
    if (args.size() > peopleArg) {
        const std::optional<int> people{parseCount(args.at(peopleArg))};
        if (!people) {
            log.error("bad people count: ", args.at(peopleArg));
            return std::nullopt;
        }
        options.people = *people;
    }
    if (args.size() > framesArg) {
        const std::optional<int> frames{parseCount(args.at(framesArg))};
        if (!frames) {
            log.error("bad frame count: ", args.at(framesArg));
            return std::nullopt;
        }
        options.frames = *frames;
    }
    return options;
}

// Moves a person one frame, bouncing at the image border.
void step(Walker& walker) {
    walker.x += walker.vx;
    walker.y += walker.vy;
    if (walker.x < walker.width || walker.x > imageWidth - walker.width) {
        walker.vx = -walker.vx;
    }
    if (walker.y < walker.height || walker.y > imageHeight - walker.height) {
        walker.vy = -walker.vy;
    }
}

// All frames up front, so that only update() is timed.
std::vector<std::vector<Detection>> makeFrames(const Options& options) {
    constexpr float minWidth{60.0F};
    constexpr float maxWidth{140.0F};
    constexpr float minHeight{150.0F};
    constexpr float maxHeight{320.0F};
    constexpr float maxSpeed{6.0F};
    constexpr float highConfidence{0.9F};
    constexpr float dipChance{0.1F};
    constexpr float minDip{0.12F};
    constexpr float maxDip{0.24F};
    constexpr int personClass{4};

    Random random{seed};
    std::vector<Walker> walkers(static_cast<std::size_t>(options.people));
    for (Walker& walker : walkers) {
        walker.width = random.next(minWidth, maxWidth);
        walker.height = random.next(minHeight, maxHeight);
        walker.x = random.next(walker.width, imageWidth - walker.width);
        walker.y = random.next(walker.height, imageHeight - walker.height);
        walker.vx = random.next(-maxSpeed, maxSpeed);
        walker.vy = random.next(-maxSpeed, maxSpeed);
    }
    const std::size_t total{static_cast<std::size_t>(options.frames + warmupFrames)};
    std::vector<std::vector<Detection>> frames(total);
    for (std::vector<Detection>& frame : frames) {
        frame.reserve(walkers.size());
        for (Walker& walker : walkers) {
            step(walker);
            // Some frames a score dips below the high threshold, like the Python measurement.
            const float confidence{random.next(0.0F, 1.0F) < dipChance ? random.next(minDip, maxDip)
                                                                       : highConfidence};
            frame.push_back(Detection{walker.x - walker.width / 2, walker.y - walker.height / 2,
                                      walker.x + walker.width / 2, walker.y + walker.height / 2,
                                      confidence, personClass});
        }
    }
    return frames;
}

double milliseconds(std::chrono::steady_clock::duration span) {
    return std::chrono::duration<double>(span).count() * millisecondsPerSecond;
}
} // namespace

int main(int argc, char** argv) {
    const Logger log{"PROBE_TRACKER", LogLevel::Info};
    const std::optional<Options> options{parseArgs(commandLine(argc, argv), log)};
    if (!options) {
        return exitUsage;
    }
    ByteTrackerConfig config;
    config.trackCapacity =
        std::max(config.trackCapacity, static_cast<std::size_t>(options->people) * 2);
    config.detectionCapacity =
        std::max(config.detectionCapacity, static_cast<std::size_t>(options->people));
    std::optional<ByteTracker> tracker{ByteTracker::create(config, log)};
    if (!tracker) {
        return EXIT_FAILURE;
    }
    const std::vector<std::vector<Detection>> frames{makeFrames(*options)};

    std::vector<double> timings;
    timings.reserve(static_cast<std::size_t>(options->frames));
    std::size_t confirmed{0};
    int highestId{0};
    for (std::size_t index{0}; index < frames.size(); ++index) {
        const auto start{std::chrono::steady_clock::now()};
        const std::vector<TrackedObject>& tracks{tracker->update(frames.at(index))};
        const auto stop{std::chrono::steady_clock::now()};
        if (index >= static_cast<std::size_t>(warmupFrames)) {
            timings.push_back(milliseconds(stop - start));
        }
        confirmed = tracks.size();
        for (const TrackedObject& track : tracks) {
            highestId = std::max(highestId, track.id);
        }
    }
    std::sort(timings.begin(), timings.end());
    const double sum{std::accumulate(timings.begin(), timings.end(), 0.0)};
    const auto percentile{[&timings](double fraction) {
        return timings.at(
            static_cast<std::size_t>(fraction * static_cast<double>(timings.size() - 1)));
    }};
    log.info(options->people, " people, ", timings.size(), " frames: mean ",
             sum / static_cast<double>(timings.size()), " ms, median ", percentile(medianFraction),
             " ms, p99 ", percentile(percentile99), " ms, max ", timings.back(), " ms");
    log.info("last frame ", confirmed, " tracks, highest id ", highestId, ", overflow ",
             tracker->overflowCount());
    return EXIT_SUCCESS;
}
