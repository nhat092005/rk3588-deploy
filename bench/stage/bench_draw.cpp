// Stage 2 bench: drawing the box outlines of the detections on the NV12 camera frame, in place:
// OpenCV on the CPU (1 and 8 threads) against the RGA2 as the application draws (RgaOverlay, 12
// boxes per job). Synthetic frames, one table.
// Usage: bench_draw [--size WxH] [--boxes N] [--runs N] [--warmup N] [--heap reserved|system]
//                   [--frame reused|fresh] [--sweep boxes|source] [--json FILE] [--dump DIR]
// Defaults: 3840x2160, 12 boxes, 300 runs after 30 warmup runs, the "reserved" heap (the RGA2 maps
// only memory below 4 GB). The run is meant for a board with the frequencies locked
// (sh/env/lock_freq.sh): the bench does not lock them, it records their state in the JSON.
// --sweep boxes measures 1, 4, 12, 34, 64 and 128 boxes on --size, --sweep source the sizes
// 1280x720 to 7680x4320 with --boxes boxes. Without --sweep it measures the one point.
// The boxes are the same for every method (draw_method.h, makeDrawBoxes). The outline is 4 pixels
// wide, inside the box. Per method it prints the latency of one call (p50, p95, p99), the CPU time
// per frame of the whole process (all threads, waiting not counted), the share of all the cores
// that were busy, and how the frame differs from the frame of the RGA2 (a sanity check: bytes that
// are not equal, the largest difference). A buffer that the RGA2 refuses is "rejected".
// --frame fresh puts the untouched picture back before every run with an RGA copy (not timed): a
// new camera frame, written by a device, so the CPU finds none of it in its caches. The default
// "reused" draws on the same frame again, which the CPU finds in its caches.
// --dump DIR (one point only, the folder must exist) writes base.nv12 (the frame before the
// boxes), one <label>.nv12 per method (the frame after) and boxes.txt (left top width height
// class, one box per line).
// Exit code 0 when no method failed (a refusal is a result), 1 when one failed, 2 on a bad command
// line.
#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <opencv2/core.hpp>

#include "arguments.h"
#include "cpu_usage.h"
#include "draw_method.h"
#include "draw_result.h"
#include "dump_file.h"
#include "freq_state.h"
#include "json_writer.h"
#include "logger.h"
#include "timing.h"

namespace {
constexpr int exitUsage{2};
constexpr int defaultWidth{3840};
constexpr int defaultHeight{2160};
constexpr int defaultBoxes{12};
constexpr int defaultRuns{300};
constexpr int defaultWarmup{30};
constexpr int maxCount{100'000};
constexpr int maxBoxes{10'000};
constexpr int bigDifference{16};
constexpr int millisPrecision{3};

enum class Sweep : std::uint8_t { None, Boxes, Source };

struct Options {
    ImageSize size{defaultWidth, defaultHeight};
    int boxes{defaultBoxes};
    Sweep sweep{Sweep::None};
    int runs{defaultRuns};
    int warmup{defaultWarmup};
    std::string_view heap{lowHeap};
    bool fresh{false};         // a new frame per run, written by a device
    std::string_view dumpDir;  // empty: no dump
    std::string_view jsonPath; // empty: no JSON file
};

// The values of the sweeps: the box counts of a busy scene (12 is one RGA2 job, 34 three jobs,
// 128 is the capacity of the overlay), and the sizes of the preprocess sweep.
constexpr std::array<int, 6> boxSweep{1, 4, 12, 34, 64, 128};
constexpr std::array<ImageSize, 6> sourceSweep{
    {{1280, 720}, {1920, 1080}, {2560, 1440}, {3840, 2160}, {4224, 2376}, {7680, 4320}}};

struct Point {
    ImageSize size;
    int boxes{0};
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
    if (name == "--boxes") {
        const std::optional<int> boxes{readInt(value)};
        options.boxes = boxes.value_or(options.boxes);
        return boxes.has_value() && *boxes >= 0 && *boxes <= maxBoxes;
    }
    if (name == "--sweep") {
        if (value != "boxes" && value != "source") {
            return false;
        }
        options.sweep = value == "boxes" ? Sweep::Boxes : Sweep::Source;
        return true;
    }
    if (name == "--runs" || name == "--warmup") {
        const std::optional<int> count{readInt(value)};
        if (!count || *count < 0 || *count > maxCount) {
            return false;
        }
        (name == "--runs" ? options.runs : options.warmup) = *count;
        return true;
    }
    if (name == "--frame") {
        if (value != "reused" && value != "fresh") {
            return false;
        }
        options.fresh = value == "fresh";
        return true;
    }
    if (name == "--heap") {
        options.heap = value;
        return true;
    }
    if (name == "--json") {
        options.jsonPath = value;
        return !value.empty();
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
        if (index + 1 >= args.size() ||
            !applyOption(options, Argument{args.at(index), args.at(index + 1)})) {
            log.error("usage: bench_draw [--size WxH] [--boxes N] [--runs N] [--warmup N] ",
                      "[--heap reserved|system] [--frame reused|fresh] [--sweep boxes|source] ",
                      "[--json FILE] ", "[--dump DIR]");
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
    case Sweep::Boxes:
        for (const int boxes : boxSweep) {
            points.push_back(Point{options.size, boxes});
        }
        break;
    case Sweep::Source:
        for (const ImageSize size : sourceSweep) {
            points.push_back(Point{size, options.boxes});
        }
        break;
    case Sweep::None:
        points.push_back(Point{options.size, options.boxes});
        break;
    }
    return points;
}

std::string_view sweepName(Sweep sweep) noexcept {
    switch (sweep) {
    case Sweep::Boxes:
        return "boxes";
    case Sweep::Source:
        return "source";
    case Sweep::None:
        break;
    }
    return "none";
}

// Times one method: warmup, then the calls one by one for the latency. The CPU time of the
// process and the busy share of all the cores are taken over the whole window.
std::optional<DrawTiming> measure(DrawMethod& method, const Options& options) {
    DrawTiming result{};
    result.threads = method.prepare();
    for (int run{0}; run < options.warmup; ++run) {
        if (!method.refresh() || !method.run()) {
            return std::nullopt;
        }
    }
    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(options.runs));
    double cpuMs{0.0};
    const CpuTimes systemStart{readCpuTimes()};
    for (int run{0}; run < options.runs; ++run) {
        if (!method.refresh()) { // not timed, not counted
            return std::nullopt;
        }
        const double cpuBegin{processCpuMs()};
        const double begin{wallMs()};
        const bool done{method.run()};
        samples.push_back(wallMs() - begin);
        cpuMs += processCpuMs() - cpuBegin;
        if (!done) {
            return std::nullopt;
        }
    }
    const CpuTimes systemEnd{readCpuTimes()};
    result.cpuMsPerFrame = cpuMs / options.runs;
    result.systemCpuPercent = busyPercent(systemStart, systemEnd);
    result.latency = percentilesOf(samples);
    return result;
}

DrawDifference compare(const cv::Mat& reference, const cv::Mat& other) {
    cv::Mat delta;
    cv::absdiff(reference, other, delta);
    DrawDifference result{};
    double largest{0.0};
    cv::minMaxLoc(delta, nullptr, &largest);
    result.largest = static_cast<int>(largest);
    result.differing = cv::countNonZero(delta);
    cv::Mat big;
    cv::compare(delta, bigDifference, big, cv::CMP_GT);
    result.overSixteen = cv::countNonZero(big);
    return result;
}

void report(const Logger& log, const DrawMethodResult& result) {
    const std::string_view label{drawLabel(result.kind)};
    if (result.status != MethodStatus::Ok) {
        log.warn(label, " ", statusName(result.status), ": ", result.message);
        return;
    }
    const DrawTiming& timing{result.timing};
    log.info(label, " threads=", timing.threads,
             " p50=", Fixed{timing.latency.p50, millisPrecision},
             " p95=", Fixed{timing.latency.p95, millisPrecision},
             " p99=", Fixed{timing.latency.p99, millisPrecision},
             " ms cpu=", Fixed{timing.cpuMsPerFrame, millisPrecision},
             " ms/frame system=", Fixed{timing.systemCpuPercent, 1}, " %");
    if (result.difference) {
        log.info(label, " diff vs R2: bytes=", result.difference->differing,
                 " max=", result.difference->largest, " over ", bigDifference, ": ",
                 result.difference->overSixteen);
    }
}

// The lines of the OpenCV build report that decide the CPU side.
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

// Writes boxes.txt and base.nv12 (the frame before the boxes) of a point.
bool dumpInputs(const Options& options, const Point& point, const std::vector<DrawBox>& boxes,
                const Logger& log) {
    std::string text;
    for (const DrawBox& box : boxes) {
        text += std::to_string(box.left) + " " + std::to_string(box.top) + " " +
                std::to_string(box.width) + " " + std::to_string(box.height) + " " +
                std::to_string(box.classId) + "\n";
    }
    if (!writeTextFile(std::string{options.dumpDir} + "/boxes.txt", text, log)) {
        return false;
    }
    std::optional<SourceFrame> base{
        makeSourceFrame(Pattern::Gradient, point.size, options.heap, log)};
    return base && writeImage(std::string{options.dumpDir} + "/base.nv12", nv12View(*base), log);
}

// Runs the methods on one frame size and box count.
DrawPointResult measurePoint(const Point& point, const Options& options, const Logger& log) {
    DrawPointResult result{point.size, point.boxes, {}, {}};
    log.info("point: frame ", point.size.width, "x", point.size.height, " boxes=", point.boxes,
             " heap=", options.heap, " frame=", options.fresh ? "fresh" : "reused",
             " runs=", options.runs, " warmup=", options.warmup);
    const std::vector<DrawBox> boxes{makeDrawBoxes(point.size, point.boxes)};
    const bool dumping{!options.dumpDir.empty()};
    if (dumping && !dumpInputs(options, point, boxes, log)) {
        result.error = "the dump of the inputs failed";
        return result;
    }
    cv::Mat reference;
    for (const DrawKind kind : allDrawMethods) {
        DrawMethodResult entry{};
        entry.kind = kind;
        std::optional<DrawMethod> method{DrawMethod::create(
            kind, DrawMethod::Spec{point.size, Pattern::Gradient, options.heap, options.fresh},
            boxes, log)};
        if (!method) {
            entry.status = MethodStatus::Failed;
            entry.message = "the setup failed (a buffer or the RGA setup), see the log";
            report(log, entry);
            result.methods.push_back(std::move(entry));
            continue;
        }
        const std::optional<DrawTiming> timing{measure(*method, options)};
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
            if (kind == DrawKind::Rga2) {
                reference = output;
            } else if (!reference.empty()) {
                entry.difference = compare(reference, output);
            }
            if (dumping && !writeImage(std::string{options.dumpDir} + "/" +
                                           std::string{drawLabel(kind)} + ".nv12",
                                       output, log)) {
                entry.status = MethodStatus::Failed;
                entry.message = "writing the dump failed";
            }
        }
        report(log, entry);
        result.methods.push_back(std::move(entry));
    }
    return result;
}

bool anyFailed(const std::vector<DrawPointResult>& points) {
    return std::any_of(points.begin(), points.end(), [](const DrawPointResult& point) {
        return !point.error.empty() || std::any_of(point.methods.begin(), point.methods.end(),
                                                   [](const DrawMethodResult& method) {
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
    DrawRunInfo info{};
    info.sweep = sweepName(options->sweep);
    info.pattern = "gradient";
    info.heap = options->heap;
    info.frame = options->fresh ? "fresh" : "reused";
    info.runs = options->runs;
    info.warmup = options->warmup;
    info.opencv = CV_VERSION;
    info.build = buildFacts();
    for (const std::string& line : info.build) {
        log.info("cv build:", line);
    }
    info.before = readFreqState();
    log.info("frequencies locked: ", info.before.locked ? "yes" : "no");
    std::vector<DrawPointResult> points;
    for (const Point& point : pointsOf(*options)) {
        points.push_back(measurePoint(point, *options, log));
    }
    info.after = readFreqState();
    if (info.before.locked != info.after.locked) {
        log.warn("the frequency lock changed during the run");
    }
    bool written{true};
    if (!options->jsonPath.empty()) {
        written = writeTextFile(std::string{options->jsonPath}, drawResultJson(info, points), log);
    }
    return anyFailed(points) || !written ? EXIT_FAILURE : EXIT_SUCCESS;
}
