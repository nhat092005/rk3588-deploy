// One RKNN model, repeated inference, optional raw input and output dump.
// Usage: probe_infer model.rknn loops [input.raw output.f32]
//   input.raw : uint8 NHWC, exactly the input size of the model (640x640x3 or 960x960x3), same
//               bytes as the Python preprocess. Default: fixed random.
//   output.f32: first output of the first run, float32 as returned by want_float=1.
// Prints per-run wall time of rknn_run (not a benchmark: frequencies are not locked) and RSS.
// Exit code 0 on success, 1 when loading, inference or destroy fails, 2 on bad arguments or files.
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "arguments.h"
#include "logger.h"
#include "rknn_api.h"

namespace {
constexpr int warmupRuns{10};
constexpr int rssProbeRun{20};
constexpr unsigned int fixedSeed{42};
constexpr int byteMask{0xFF};
constexpr int decimalBase{10};
constexpr int timePrecision{2};
constexpr int exitUsage{2};
constexpr std::string_view rssPrefix{"VmRSS:"};
// Positions of the command line arguments.
constexpr std::size_t modelArg{1};
constexpr std::size_t loopsArg{2};
constexpr std::size_t inputArg{3};
constexpr std::size_t dumpArg{4};
constexpr std::size_t argsWithoutFiles{3};
constexpr std::size_t argsWithFiles{5};

struct Options {
    const char* modelPath{nullptr};
    int loops{0};
    const char* inputPath{nullptr}; // nullptr: fixed random input
    const char* dumpPath{nullptr};  // nullptr: no dump
};

struct Stats {
    double sumMs{0.0};
    double maxMs{0.0};
    std::optional<std::int64_t> rssEarlyKb;
    int timed{0};
    bool failed{false};
};

// Layout: program, model, loops, then optionally input.raw and output.f32 together.
std::optional<Options> parseArgs(const std::vector<const char*>& args, const Logger& log) {
    if (args.size() != argsWithoutFiles && args.size() != argsWithFiles) {
        log.error("usage: ", args.empty() ? "probe_infer" : args.at(0),
                  " model.rknn loops [input.raw output.f32]");
        return std::nullopt;
    }
    Options options{};
    options.modelPath = args.at(modelArg);
    char* end{nullptr};
    errno = 0;
    const long long loops{std::strtoll(args.at(loopsArg), &end, decimalBase)};
    if (errno != 0 || end == args.at(loopsArg) || *end != '\0' || loops <= 0 ||
        loops > std::numeric_limits<int>::max()) {
        log.error("invalid loops: ", args.at(loopsArg));
        return std::nullopt;
    }
    options.loops = static_cast<int>(loops);
    if (args.size() == argsWithFiles) {
        options.inputPath = args.at(inputArg);
        options.dumpPath = args.at(dumpArg);
    }
    return options;
}

// Resident set size of this process in kB, from /proc/self/status.
std::optional<std::int64_t> readRssKb() {
    std::ifstream status{"/proc/self/status"};
    std::string line{};
    while (std::getline(status, line)) {
        if (line.rfind(rssPrefix, 0) != 0) {
            continue;
        }
        const std::string number{line.substr(rssPrefix.size())};
        char* end{nullptr};
        errno = 0;
        const long long value{std::strtoll(number.c_str(), &end, decimalBase)};
        if (errno != 0 || end == number.c_str() || value < 0) {
            return std::nullopt;
        }
        return value;
    }
    return std::nullopt;
}

// The whole file, or nothing when it cannot be read or is empty.
std::optional<std::vector<char>> readFile(const char* path) {
    std::ifstream file{path, std::ios::binary | std::ios::ate};
    if (!file) {
        return std::nullopt;
    }
    const std::streamoff size{file.tellg()};
    if (size <= 0) {
        return std::nullopt;
    }
    std::vector<char> content(static_cast<std::size_t>(size));
    file.seekg(0);
    file.read(content.data(), size);
    if (!file) {
        return std::nullopt;
    }
    return content;
}

// The model input: the bytes of `inputPath`, which must have exactly the input size, or fixed
// pseudo-random bytes when no path is given.
std::optional<std::vector<char>> makeImage(const char* inputPath, std::size_t imageBytes) {
    if (inputPath == nullptr) {
        std::vector<char> image(imageBytes);
        std::srand(fixedSeed);
        for (char& pixel : image) {
            pixel = static_cast<char>(static_cast<unsigned char>(std::rand() & byteMask));
        }
        return image;
    }
    std::optional<std::vector<char>> image{readFile(inputPath)};
    if (!image || image->size() != imageBytes) {
        return std::nullopt;
    }
    return image;
}

// One loaded RKNN model. All RKNN types stay inside this class.
class RknnModel {
public:
    // rknn_init on the model bytes, then logs the SDK version and the tensor counts.
    static std::optional<RknnModel> load(std::vector<char>& blob, const Logger& log) {
        rknn_context rawContext{0};
        const int status{rknn_init(&rawContext, blob.data(),
                                   static_cast<std::uint32_t>(blob.size()), 0, nullptr)};
        if (status != 0) {
            log.error("rknn_init ret=", status);
            return std::nullopt;
        }
        RknnModel model{rawContext, log};
        log.info("rknn_init ret=", status);
        if (!model.describe()) {
            return std::nullopt;
        }
        return std::optional<RknnModel>{std::move(model)};
    }

    RknnModel(const RknnModel&) = delete;
    RknnModel& operator=(const RknnModel&) = delete;
    RknnModel(RknnModel&& other) noexcept
        : context{std::exchange(other.context, 0)}, loaded{std::exchange(other.loaded, false)},
          log{other.log}, outputs{std::move(other.outputs)}, inputSize{other.inputSize} {}
    RknnModel& operator=(RknnModel&& other) noexcept {
        if (this != &other) {
            destroyAndLog();
            context = std::exchange(other.context, 0);
            loaded = std::exchange(other.loaded, false);
            log = other.log;
            outputs = std::move(other.outputs);
            inputSize = other.inputSize;
        }
        return *this;
    }
    // A failure to destroy is logged by the destructor. After an explicit destroy() nothing is
    // logged twice.
    ~RknnModel() {
        destroyAndLog();
    }

    // One inference on `image` (uint8 NHWC). Returns the wall time of rknn_run in milliseconds.
    // Writes output 0 as float32 to `dumpPath` when it is given.
    [[nodiscard]] std::optional<double> infer(std::vector<char>& image, const char* dumpPath);
    // rknn_destroy. Returns its status, 0 when already destroyed. Safe to call twice.
    [[nodiscard]] int destroy() noexcept;
    // Bytes of the model input (uint8 NHWC), read from the model.
    std::size_t inputBytes() const noexcept {
        return inputSize;
    }

private:
    RknnModel(rknn_context rawContext, Logger logger) noexcept
        : context{rawContext}, loaded{true}, log{logger} {}

    void destroyAndLog() noexcept;
    [[nodiscard]] bool describe();
    [[nodiscard]] bool dumpFirstOutput(const char* dumpPath) const;

    rknn_context context{0};
    bool loaded{false};
    Logger log;
    // Reused by every inference, so the loop allocates nothing.
    std::vector<rknn_output> outputs;
    std::size_t inputSize{0};
};

bool RknnModel::describe() {
    rknn_sdk_version version{};
    int status{rknn_query(context, RKNN_QUERY_SDK_VERSION, &version, sizeof(version))};
    if (status != 0) {
        log.error("rknn_query(SDK_VERSION) ret=", status);
        return false;
    }
    log.info("api=", &version.api_version[0], " drv=", &version.drv_version[0]);

    rknn_input_output_num counts{};
    status = rknn_query(context, RKNN_QUERY_IN_OUT_NUM, &counts, sizeof(counts));
    if (status != 0 || counts.n_input == 0 || counts.n_output == 0) {
        log.error("rknn_query(IN_OUT_NUM) ret=", status, " n_input=", counts.n_input,
                  " n_output=", counts.n_output);
        return false;
    }
    log.info("n_input=", counts.n_input, " n_output=", counts.n_output);
    outputs.resize(counts.n_output);
    rknn_tensor_attr attr{};
    attr.index = 0;
    status = rknn_query(context, RKNN_QUERY_INPUT_ATTR, &attr, sizeof(attr));
    if (status != 0) {
        log.error("rknn_query(INPUT_ATTR) ret=", status);
        return false;
    }
    inputSize = attr.size;
    log.info("input bytes=", inputSize);
    return true;
}

std::optional<double> RknnModel::infer(std::vector<char>& image, const char* dumpPath) {
    rknn_input input{};
    input.index = 0;
    input.type = RKNN_TENSOR_UINT8;
    input.fmt = RKNN_TENSOR_NHWC;
    input.size = static_cast<std::uint32_t>(image.size());
    input.buf = image.data();
    int status{rknn_inputs_set(context, 1, &input)};
    if (status != 0) {
        log.error("rknn_inputs_set ret=", status);
        return std::nullopt;
    }
    const std::chrono::steady_clock::time_point start{std::chrono::steady_clock::now()};
    status = rknn_run(context, nullptr);
    const std::chrono::steady_clock::time_point stop{std::chrono::steady_clock::now()};
    if (status != 0) {
        log.error("rknn_run ret=", status);
        return std::nullopt;
    }

    for (rknn_output& output : outputs) {
        output = rknn_output{};
        output.want_float = 1;
    }
    const std::uint32_t outputCount{static_cast<std::uint32_t>(outputs.size())};
    status = rknn_outputs_get(context, outputCount, outputs.data(), nullptr);
    if (status != 0) {
        log.error("rknn_outputs_get ret=", status);
        return std::nullopt;
    }
    const bool dumped{dumpPath == nullptr || dumpFirstOutput(dumpPath)};
    status = rknn_outputs_release(context, outputCount, outputs.data());
    if (status != 0) {
        log.error("rknn_outputs_release ret=", status);
        return std::nullopt;
    }
    if (!dumped) {
        return std::nullopt;
    }
    return std::chrono::duration<double, std::milli>{stop - start}.count();
}

bool RknnModel::dumpFirstOutput(const char* dumpPath) const {
    const rknn_output& first{outputs.front()};
    if (first.buf == nullptr) {
        log.error("missing output[0] buffer for dump");
        return false;
    }
    std::ofstream dump{dumpPath, std::ios::binary};
    dump.write(static_cast<const char*>(first.buf), static_cast<std::streamsize>(first.size));
    dump.close();
    if (!dump) {
        log.error("failed to write output dump: ", dumpPath);
        return false;
    }
    return true;
}

int RknnModel::destroy() noexcept {
    if (!loaded) {
        return 0;
    }
    loaded = false;
    return rknn_destroy(context);
}

void RknnModel::destroyAndLog() noexcept {
    const int status{destroy()};
    if (status != 0) {
        log.error("rknn_destroy ret=", status);
    }
}

// Runs `options.loops` inferences. The first `warmupRuns` are not timed.
Stats runLoops(RknnModel& model, std::vector<char>& image, const Options& options) {
    Stats stats{};
    for (int run{0}; run < options.loops && !stats.failed; ++run) {
        const std::optional<double> elapsedMs{
            model.infer(image, run == 0 ? options.dumpPath : nullptr)};
        if (!elapsedMs) {
            stats.failed = true;
            continue;
        }
        if (run >= warmupRuns) {
            stats.sumMs += *elapsedMs;
            stats.maxMs = *elapsedMs > stats.maxMs ? *elapsedMs : stats.maxMs;
            ++stats.timed;
        }
        if (run == rssProbeRun) {
            stats.rssEarlyKb = readRssKb();
        }
    }
    return stats;
}

void logStats(const Stats& stats, const Options& options, const Logger& log) {
    const int timed{stats.timed > 0 ? stats.timed : 1};
    const std::int64_t unknownRss{-1};
    log.info("loops=", options.loops, " failed=", stats.failed,
             " avg_run_ms=", Fixed{stats.sumMs / timed, timePrecision},
             " max_run_ms=", Fixed{stats.maxMs, timePrecision}, " rss_run", rssProbeRun, "=",
             stats.rssEarlyKb.value_or(unknownRss), "kB rss_end=", readRssKb().value_or(unknownRss),
             "kB");
}
} // namespace

int main(int argc, char** argv) {
    const std::vector<const char*> args{commandLine(argc, argv)};
    const Logger log{"PROBE_INFER", LogLevel::Debug};
    const std::optional<Options> options{parseArgs(args, log)};
    if (!options) {
        return exitUsage;
    }
    std::optional<std::vector<char>> blob{readFile(options->modelPath)};
    if (!blob) {
        log.error("cannot read model file: ", options->modelPath);
        return exitUsage;
    }
    std::optional<RknnModel> model{RknnModel::load(*blob, log)};
    if (!model) {
        return EXIT_FAILURE;
    }
    std::optional<std::vector<char>> image{makeImage(options->inputPath, model->inputBytes())};
    if (!image) {
        log.error("bad input file: it must have exactly ", model->inputBytes(), " bytes");
        return exitUsage;
    }
    const Stats stats{runLoops(*model, *image, *options)};
    logStats(stats, *options, log);
    const int destroyStatus{model->destroy()};
    if (destroyStatus != 0) {
        log.error("rknn_destroy ret=", destroyStatus);
    }
    return !stats.failed && destroyStatus == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
