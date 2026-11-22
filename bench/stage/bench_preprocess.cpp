// Stage 1 bench: the preprocess job (rotate 180, scale to fit, NV12 to RGB, place into a padded
// square image) on the CPU with OpenCV and on the RGA, synthetic frames, one table.
// Usage: bench_preprocess [--size WxH] [--target N] [--pattern gradient|bars|noise] [--runs N]
//                         [--warmup N] [--heap reserved|system] [--dump DIR]
//                         [--sweep source|output] [--json FILE]
// The frame is NV12 in BT.601 full range, what the camera of the board reports: the RGA takes two
// calls (colour conversion into a staging buffer, then scale and rotate), as in the application.
// Defaults: 3840x2160 into 640x640, gradient, 300 runs after 30 warmup runs, the "reserved" heap
// (the RGA2 maps only memory below 4 GB). The run is meant for a board with the frequencies locked
// (sh/env/lock_freq.sh): the bench does not lock them, it records their state in the JSON.
// --sweep source measures the sizes 1280x720 to 7680x4320 into --target, --sweep output the sizes
// 320 to 1024 from --size. Without --sweep it measures the one point of --size and --target.
// Per method it prints the latency of one call (p50, p95, p99), the CPU time per frame of the
// whole process (all threads, waiting not counted) and the pixel difference of the output to the
// output of cv1 (a sanity check between methods, not the rk3588_common reference).
// A method that the RGA driver refuses at a size is reported as "rejected" with the driver
// message, any other failure as "failed"; the run goes on with the next method and point.
// --json FILE writes the result: every point and method, the OpenCV build and the clock state of
// the board before and after the run. --dump DIR (one point only, the folder must exist) writes
// frame.nv12 and one <label>.rgb per method (raw): tools/check/check_bench_preprocess.py compares
// them with rk3588_common.model_io.
// Exit code 0 when no method failed (a refusal is a result), 1 when one failed, 2 on a bad command
// line.
#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <opencv2/core.hpp>

#include "arguments.h"
#include "dmabuf.h"
#include "dump_file.h"
#include "freq_state.h"
#include "json_writer.h"
#include "logger.h"
#include "preprocess_method.h"
#include "preprocess_result.h"
#include "synthetic_frame.h"
#include "timing.h"

namespace {
constexpr int exitUsage{2};
constexpr int defaultWidth{3840};
constexpr int defaultHeight{2160};
constexpr int defaultRuns{300};
constexpr int defaultWarmup{30};
constexpr int maxCount{100'000};
constexpr int minTarget{16};
constexpr int maxTarget{4096};
constexpr int bigDifference{16}; // check_letterbox.py BIG_DIFF
constexpr double percentOf{100.0};
constexpr int millisPrecision{3};

enum class Sweep : std::uint8_t { None, Source, Output };

struct Options {
    ImageSize size{defaultWidth, defaultHeight};
    int target{defaultModelSize};
    Sweep sweep{Sweep::None};
    Pattern pattern{Pattern::Gradient};
    int runs{defaultRuns};
    int warmup{defaultWarmup};
    std::string_view heap{lowHeap};
    std::string_view dumpDir;  // empty: no dump
    std::string_view jsonPath; // empty: no JSON file
};

// The values of the sweeps (ADR section 6): every one is a multiple of 16, the width alignment of
// an RGB888 RGA buffer.
constexpr std::array<ImageSize, 6> sourceSweep{
    {{1280, 720}, {1920, 1080}, {2560, 1440}, {3840, 2160}, {4224, 2376}, {7680, 4320}}};
constexpr std::array<int, 6> outputSweep{320, 416, 512, 640, 800, 1024};

// One source size and target size to measure.
struct Point {
    ImageSize source;
    int target{0};
};

std::optional<int> readInt(std::string_view text) noexcept {
    int value{0};
    const char* last{text.data() + text.size()};
    // The range is [data, data + size): the view needs no terminator.
    // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage)
    const std::from_chars_result result{std::from_chars(text.data(), last, value)};
    if (result.ec != std::errc{} || result.ptr != last) {
        return std::nullopt;
    }
    return value;
}

std::optional<ImageSize> readSize(std::string_view text) noexcept {
    const std::size_t cross{text.find('x')};
    if (cross == std::string_view::npos) {
        return std::nullopt;
    }
    const std::optional<int> width{readInt(text.substr(0, cross))};
    const std::optional<int> height{readInt(text.substr(cross + 1))};
    if (!width || !height) {
        return std::nullopt;
    }
    return ImageSize{*width, *height};
}

// One "--name value" pair of the command line.
struct Argument {
    std::string_view name;
    std::string_view value;
};

bool applyOption(Options& options, const Argument& argument) noexcept {
    const std::string_view name{argument.name};
    const std::string_view value{argument.value};
    if (name == "--size") {
        const std::optional<ImageSize> size{readSize(value)};
        options.size = size.value_or(options.size);
        return size.has_value();
    }
    if (name == "--target") {
        const std::optional<int> target{readInt(value)};
        options.target = target.value_or(options.target);
        return target.has_value() && *target >= minTarget && *target <= maxTarget;
    }
    if (name == "--sweep") {
        if (value != "source" && value != "output") {
            return false;
        }
        options.sweep = value == "source" ? Sweep::Source : Sweep::Output;
        return true;
    }
    if (name == "--json") {
        options.jsonPath = value;
        return !value.empty();
    }
    if (name == "--pattern") {
        const std::optional<Pattern> pattern{parsePattern(value)};
        options.pattern = pattern.value_or(options.pattern);
        return pattern.has_value();
    }
    if (name == "--runs" || name == "--warmup") {
        const std::optional<int> count{readInt(value)};
        if (!count || *count < 0 || *count > maxCount) {
            return false;
        }
        (name == "--runs" ? options.runs : options.warmup) = *count;
        return true;
    }
    if (name == "--heap") {
        options.heap = value;
        return true;
    }
    if (name == "--dump") {
        options.dumpDir = value;
        return !value.empty();
    }
    return false;
}

std::optional<Options> parseOptions(const std::vector<const char*>& args, const Logger& log) {
    Options options{};
    for (std::size_t index{1}; index < args.size(); index += 2) {
        const std::string_view name{args.at(index)};
        if (index + 1 >= args.size() || !applyOption(options, Argument{name, args.at(index + 1)})) {
            log.error("usage: bench_preprocess [--size WxH] [--target N] ",
                      "[--pattern gradient|bars|noise] [--runs N] [--warmup N] ",
                      "[--heap reserved|system] [--dump DIR] [--sweep source|output] ",
                      "[--json FILE]");
            return std::nullopt;
        }
    }
    if (options.runs < 1) {
        log.error("--runs must be at least 1");
        return std::nullopt;
    }
    if (options.sweep != Sweep::None && !options.dumpDir.empty()) {
        log.error("--dump covers one point: it cannot go with --sweep");
        return std::nullopt;
    }
    return options;
}

std::vector<Point> pointsOf(const Options& options) {
    std::vector<Point> points;
    switch (options.sweep) {
    case Sweep::Source:
        for (const ImageSize size : sourceSweep) {
            points.push_back(Point{size, options.target});
        }
        break;
    case Sweep::Output:
        for (const int target : outputSweep) {
            points.push_back(Point{options.size, target});
        }
        break;
    case Sweep::None:
        points.push_back(Point{options.size, options.target});
        break;
    }
    return points;
}

std::string_view sweepName(Sweep sweep) noexcept {
    switch (sweep) {
    case Sweep::Source:
        return "source";
    case Sweep::Output:
        return "output";
    case Sweep::None:
        break;
    }
    return "none";
}

// Times one method: warmup, then `runs` calls one by one for the latency, and the CPU time of
// the process over the whole window.
std::optional<Measurement> measure(PreprocessMethod& method, const Options& options) {
    const std::optional<int> threads{method.prepare()};
    if (!threads) {
        return std::nullopt;
    }
    for (int run{0}; run < options.warmup; ++run) {
        if (!method.run()) {
            return std::nullopt;
        }
    }
    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(options.runs));
    const double cpuStart{processCpuMs()};
    for (int run{0}; run < options.runs; ++run) {
        const double begin{wallMs()};
        const bool done{method.run()};
        samples.push_back(wallMs() - begin);
        if (!done) {
            return std::nullopt;
        }
    }
    const double cpuTotal{processCpuMs() - cpuStart};
    return Measurement{*threads, percentilesOf(samples), cpuTotal / options.runs};
}

Difference compare(const cv::Mat& reference, const cv::Mat& other) {
    cv::Mat delta;
    cv::absdiff(reference, other, delta);
    const cv::Mat values{delta.reshape(1)};
    Difference result{};
    cv::minMaxLoc(values, nullptr, &result.largest);
    result.mean = cv::mean(values)[0];
    cv::Mat big;
    cv::compare(values, bigDifference, big, cv::CMP_GT);
    result.bigShare = percentOf * static_cast<double>(cv::countNonZero(big)) /
                      static_cast<double>(values.total());
    return result;
}

// Writes a file of the dump folder: <dir>/<name>.
bool dump(std::string_view dir, std::string_view name, const cv::Mat& image, const Logger& log) {
    const std::string path{std::string{dir} + "/" + std::string{name}};
    return writeImage(path, image, log);
}

void report(const Logger& log, const MethodResult& result) {
    if (result.status != MethodStatus::Ok) {
        log.warn(methodLabel(result.kind), " ", statusName(result.status), ": ", result.message);
        return;
    }
    const Measurement& timing{result.timing};
    log.info(methodLabel(result.kind), " threads=", timing.threads,
             " p50=", Fixed{timing.latency.p50, millisPrecision},
             " p95=", Fixed{timing.latency.p95, millisPrecision},
             " p99=", Fixed{timing.latency.p99, millisPrecision},
             " ms cpu=", Fixed{timing.cpuMsPerFrame, millisPrecision}, " ms/frame");
    if (result.difference) {
        log.info(methodLabel(result.kind),
                 " diff vs cv1: mean=", Fixed{result.difference->mean, millisPrecision},
                 " max=", Fixed{result.difference->largest, 0}, " over ", bigDifference, ": ",
                 Fixed{result.difference->bigShare, millisPrecision}, " %");
    }
}

// The lines of the OpenCV build report that decide the CPU side: the parallel backend and the
// SIMD (NEON) baseline and dispatch lists.
std::vector<std::string> buildFacts() {
    constexpr std::array<std::string_view, 3> keys{"Parallel framework", "Baseline", "Dispatched"};
    std::vector<std::string> facts;
    const std::string& report{cv::getBuildInformation()};
    std::size_t begin{0};
    while (begin < report.size()) {
        const std::size_t end{std::min(report.find('\n', begin), report.size())};
        const std::string_view line{std::string_view{report}.substr(begin, end - begin)};
        for (const std::string_view key : keys) {
            if (line.find(key) != std::string_view::npos) {
                facts.emplace_back(line);
                break;
            }
        }
        begin = end + 1;
    }
    return facts;
}

// Runs the six methods on one frame size and target size.
PointResult measurePoint(const Point& point, const Options& options, const Logger& log) {
    PointResult result{point.source, point.target, {}, {}};
    log.info("point: frame ", point.source.width, "x", point.source.height, " into ", point.target,
             "x", point.target, " pattern=", patternName(options.pattern), " heap=", options.heap,
             " runs=", options.runs, " warmup=", options.warmup);
    std::optional<SourceFrame> frame{
        makeSourceFrame(options.pattern, point.source, options.heap, log)};
    if (!frame) {
        result.error = "the frame could not be made, see the log";
        return result;
    }
    const bool dumping{!options.dumpDir.empty()};
    if (dumping && !dump(options.dumpDir, "frame.nv12", nv12View(*frame), log)) {
        result.error = "the dump of the frame failed";
        return result;
    }
    cv::Mat reference;
    for (const MethodKind kind : allMethods) {
        MethodResult entry{};
        entry.kind = kind;
        std::optional<PreprocessMethod> method{
            PreprocessMethod::create(kind, *frame, point.target, log)};
        if (!method) {
            entry.status = MethodStatus::Failed;
            entry.message = "the setup failed (a buffer or the RGA setup), see the log";
            report(log, entry);
            result.methods.push_back(std::move(entry));
            continue;
        }
        const std::optional<Measurement> timing{measure(*method, options)};
        cv::Mat output;
        if (!timing) {
            const Failure failure{method->failure()};
            entry.status = failure.status;
            entry.message = failure.message;
        } else if (!method->copyOutput(output)) {
            entry.status = MethodStatus::Failed;
            entry.message = "the cache sync before the copy of the output failed";
        } else {
            entry.timing = *timing;
        }
        if (entry.status == MethodStatus::Ok) {
            if (kind == MethodKind::Cv1) {
                reference = output;
            } else if (!reference.empty()) {
                entry.difference = compare(reference, output);
            }
            if (dumping &&
                !dump(options.dumpDir, std::string{methodLabel(kind)} + ".rgb", output, log)) {
                entry.status = MethodStatus::Failed;
                entry.message = "writing the dump failed";
            }
        }
        report(log, entry);
        result.methods.push_back(std::move(entry));
    }
    return result;
}

bool anyFailed(const std::vector<PointResult>& points) {
    return std::any_of(points.begin(), points.end(), [](const PointResult& point) {
        return !point.error.empty() || std::any_of(point.methods.begin(), point.methods.end(),
                                                   [](const MethodResult& method) {
                                                       return method.status == MethodStatus::Failed;
                                                   });
    });
}
} // namespace

int main(int argc, char** argv) {
    const Logger log{"BENCH", LogLevel::Info};
    const std::optional<Options> options{parseOptions(commandLine(argc, argv), log)};
    if (!options) {
        return exitUsage;
    }
    log.info("OpenCV ", CV_VERSION, " cpus=", cv::getNumberOfCPUs());
    RunInfo info{};
    info.sweep = sweepName(options->sweep);
    info.pattern = patternName(options->pattern);
    info.heap = options->heap;
    info.runs = options->runs;
    info.warmup = options->warmup;
    info.opencv = CV_VERSION;
    info.build = buildFacts();
    for (const std::string& line : info.build) {
        log.info("cv build:", line);
    }
    info.before = readFreqState();
    log.info("frequencies locked: ", info.before.locked ? "yes" : "no");
    std::vector<PointResult> points;
    for (const Point& point : pointsOf(*options)) {
        points.push_back(measurePoint(point, *options, log));
    }
    info.after = readFreqState();
    if (info.before.locked != info.after.locked) {
        log.warn("the frequency lock changed during the run");
    }
    bool written{true};
    if (!options->jsonPath.empty()) {
        written = writeTextFile(std::string{options->jsonPath}, resultJson(info, points), log);
    }
    return anyFailed(points) || !written ? EXIT_FAILURE : EXIT_SUCCESS;
}
