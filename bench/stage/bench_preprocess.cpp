// Stage 1 bench: the preprocess job (rotate 180, scale to fit, NV12 to RGB, place into a padded
// 640 x 640 image) on the CPU with OpenCV and on the RGA, one synthetic frame, one table.
// Usage: bench_preprocess [--size WxH] [--pattern gradient|bars|noise] [--runs N] [--warmup N]
//                         [--heap reserved|system] [--dump DIR]
// The frame is NV12 in BT.601 full range, what the camera of the board reports: the RGA takes two
// calls (colour conversion into a staging buffer, then scale and rotate), as in the application.
// Defaults: 3840x2160, gradient, 300 runs after 30 warmup runs, the "reserved" heap (the RGA2 maps
// only memory below 4 GB). The run is meant for a board with the frequencies locked
// (sh/env/lock_freq.sh): the bench does not lock them and does not record the state yet.
// Per method it prints the latency of one call (p50, p95, p99), the CPU time per frame of the
// whole process (all threads, waiting not counted) and the pixel difference of the output to the
// output of cv1 (a sanity check between methods, not the rk3588_common reference).
// --dump DIR (the folder must exist) writes frame.nv12 and one <label>.rgb per method (640x640x3,
// raw): tools/check/check_bench_preprocess.py compares them with rk3588_common.model_io.
// Exit code 0 when every method ran, 1 when a method failed, 2 on a bad command line.
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
#include "dmabuf.h"
#include "dump_file.h"
#include "logger.h"
#include "preprocess_method.h"
#include "synthetic_frame.h"
#include "timing.h"

namespace {
constexpr int exitUsage{2};
constexpr int defaultWidth{3840};
constexpr int defaultHeight{2160};
constexpr int defaultRuns{300};
constexpr int defaultWarmup{30};
constexpr int maxCount{100'000};
constexpr int bigDifference{16}; // check_letterbox.py BIG_DIFF
constexpr double percentOf{100.0};
constexpr int millisPrecision{3};

struct Options {
    ImageSize size{defaultWidth, defaultHeight};
    Pattern pattern{Pattern::Gradient};
    int runs{defaultRuns};
    int warmup{defaultWarmup};
    std::string_view heap{lowHeap};
    std::string_view dumpDir; // empty: no dump
};

// Pixel difference of one output to the reference output, over all channel values.
struct Difference {
    double mean{0.0};
    double largest{0.0};
    double bigShare{0.0}; // percent of values that differ by more than bigDifference
};

struct Measurement {
    int threads{1};
    Percentiles latency;
    double cpuMsPerFrame{0.0};
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
            log.error("usage: bench_preprocess [--size WxH] [--pattern gradient|bars|noise] ",
                      "[--runs N] [--warmup N] [--heap reserved|system] [--dump DIR]");
            return std::nullopt;
        }
    }
    if (options.runs < 1) {
        log.error("--runs must be at least 1");
        return std::nullopt;
    }
    return options;
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

void report(const Logger& log, MethodKind kind, const Measurement& timing,
            const std::optional<Difference>& difference) {
    log.info(methodLabel(kind), " threads=", timing.threads,
             " p50=", Fixed{timing.latency.p50, millisPrecision},
             " p95=", Fixed{timing.latency.p95, millisPrecision},
             " p99=", Fixed{timing.latency.p99, millisPrecision},
             " ms cpu=", Fixed{timing.cpuMsPerFrame, millisPrecision}, " ms/frame");
    if (difference) {
        log.info(methodLabel(kind), " diff vs cv1: mean=", Fixed{difference->mean, millisPrecision},
                 " max=", Fixed{difference->largest, 0}, " over ", bigDifference, ": ",
                 Fixed{difference->bigShare, millisPrecision}, " %");
    }
}

// The lines of the OpenCV build report that decide the CPU side: the parallel backend and the
// SIMD (NEON) baseline and dispatch lists.
void logBuildFacts(const Logger& log) {
    constexpr std::array<std::string_view, 3> keys{"Parallel framework", "Baseline", "Dispatched"};
    const std::string& report{cv::getBuildInformation()};
    std::size_t begin{0};
    while (begin < report.size()) {
        const std::size_t end{std::min(report.find('\n', begin), report.size())};
        const std::string_view line{std::string_view{report}.substr(begin, end - begin)};
        for (const std::string_view key : keys) {
            if (line.find(key) != std::string_view::npos) {
                log.info("cv build:", line);
                break;
            }
        }
        begin = end + 1;
    }
}
} // namespace

int main(int argc, char** argv) {
    const Logger log{"BENCH", LogLevel::Info};
    const std::optional<Options> options{parseOptions(commandLine(argc, argv), log)};
    if (!options) {
        return exitUsage;
    }
    log.info("OpenCV ", CV_VERSION, " cpus=", cv::getNumberOfCPUs());
    logBuildFacts(log);
    log.info("frame ", options->size.width, "x", options->size.height,
             " pattern=", patternName(options->pattern), " heap=", options->heap,
             " runs=", options->runs, " warmup=", options->warmup);
    std::optional<SourceFrame> frame{
        makeSourceFrame(options->pattern, options->size, options->heap, log)};
    if (!frame) {
        return EXIT_FAILURE;
    }
    bool allRan{true};
    const bool dumping{!options->dumpDir.empty()};
    if (dumping && !dump(options->dumpDir, "frame.nv12", nv12View(*frame), log)) {
        return EXIT_FAILURE;
    }
    cv::Mat reference;
    for (const MethodKind kind : allMethods) {
        std::optional<PreprocessMethod> method{PreprocessMethod::create(kind, *frame, log)};
        if (!method) {
            log.warn(methodLabel(kind), " skipped");
            continue;
        }
        const std::optional<Measurement> timing{measure(*method, *options)};
        cv::Mat output;
        if (!timing || !method->copyOutput(output)) {
            log.error(methodLabel(kind), " failed");
            allRan = false;
            continue;
        }
        if (kind == MethodKind::Cv1) {
            reference = output;
        }
        if (dumping &&
            !dump(options->dumpDir, std::string{methodLabel(kind)} + ".rgb", output, log)) {
            allRan = false;
        }
        report(log, kind, *timing,
               reference.empty() ? std::nullopt
                                 : std::optional<Difference>{compare(reference, output)});
    }
    return allRan ? EXIT_SUCCESS : EXIT_FAILURE;
}
