// Stage 5 bench: the NPU inference of the application (NpuInfer, one model, the input read from a
// dma-buf, the outputs as float) run back to back, to measure what it costs the CPU. There is no
// CPU path to compare with: the point is the CPU time and the latency of one inference.
// Usage: bench_infer --model FILE.rknn [--cores auto|0|1|2|01|012] [--runs N] [--warmup N]
//                    [--json FILE]
// Defaults: cores auto (what the application sets), 300 timed inferences after 30 warmup ones.
// The run is meant for a board with the frequencies locked (sh/env/lock_freq.sh): the bench
// records their state in the JSON. Pin the process with taskset to choose the CPU core of the
// calling thread (the RKNN runtime threads follow); the placement is not part of this program.
// The input is a constant image in a dma-buf (the time of an inference does not depend on the
// values), attached once; every run() is timed alone. Per run: the latency (p50, p95, p99), the
// CPU time of the process per inference (all threads, waiting not counted) and the share of all
// the CPU cores that was busy over the timed part (/proc/stat: kernel work for the NPU driver
// and interrupts count there, in the process time only what the process does).
// Exit code 0 on success, 1 when the model or an inference fails, 2 on a bad command line.
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "arguments.h"
#include "cpu_usage.h"
#include "dmabuf.h"
#include "freq_state.h"
#include "json_writer.h"
#include "logger.h"
#include "npu_infer.h"
#include "timing.h"

namespace {
constexpr int exitFailure{1};
constexpr int exitUsage{2};
constexpr int defaultRuns{300};
constexpr int defaultWarmup{30};
constexpr int maxCount{100'000};
constexpr int idleValue{114}; // the padding value of the model input, any value would do
constexpr double millisPerSecond{1000.0};
constexpr int millisPrecision{3};
constexpr int schemaVersion{1};

struct Options {
    std::string_view modelPath;
    NpuCores cores{NpuCores::Auto};
    std::string_view coresName{"auto"};
    int runs{defaultRuns};
    int warmup{defaultWarmup};
    std::string_view jsonPath; // empty: no JSON file
};

struct Argument {
    std::string_view name;
    std::string_view value;
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

struct CoreName {
    std::string_view name;
    NpuCores cores;
};

constexpr std::array<CoreName, 6> coreNames{{{"auto", NpuCores::Auto},
                                             {"0", NpuCores::Core0},
                                             {"1", NpuCores::Core1},
                                             {"2", NpuCores::Core2},
                                             {"01", NpuCores::Core01},
                                             {"012", NpuCores::Core012}}};

bool applyCores(Options& options, std::string_view text) noexcept {
    for (const CoreName& entry : coreNames) {
        if (entry.name == text) {
            options.cores = entry.cores;
            options.coresName = entry.name;
            return true;
        }
    }
    return false;
}

bool applyCount(int& target, std::string_view text, int low) noexcept {
    const std::optional<int> value{readInt(text)};
    if (!value || *value < low || *value > maxCount) {
        return false;
    }
    target = *value;
    return true;
}

bool applyOption(Options& options, const Argument& argument) noexcept {
    const std::string_view name{argument.name};
    const std::string_view value{argument.value};
    if (name == "--model") {
        options.modelPath = value;
        return !value.empty();
    }
    if (name == "--cores") {
        return applyCores(options, value);
    }
    if (name == "--runs") {
        return applyCount(options.runs, value, 1);
    }
    if (name == "--warmup") {
        return applyCount(options.warmup, value, 0);
    }
    if (name == "--json") {
        options.jsonPath = value;
        return !value.empty();
    }
    return false;
}

std::optional<Options> parseOptions(const std::vector<const char*>& args, const Logger& log) {
    Options options{};
    for (std::size_t index{1}; index < args.size(); index += 2) {
        if (index + 1 >= args.size() ||
            !applyOption(options, Argument{args.at(index), args.at(index + 1)})) {
            log.error("usage: bench_infer --model FILE.rknn [--cores auto|0|1|2|01|012] ",
                      "[--runs N] [--warmup N] [--json FILE]");
            return std::nullopt;
        }
    }
    if (options.modelPath.empty()) {
        log.error("--model is required");
        return std::nullopt;
    }
    return options;
}

// What the timed part gave.
struct Result {
    std::vector<double> latencies;
    double cpuMs{0.0};
    double busyPercentValue{0.0};
};

void writeFreq(JsonWriter& json, const FreqState& state) {
    json.beginObject();
    json.key("locked");
    json.boolean(state.locked);
    json.key("entries");
    json.beginArray();
    for (const FreqEntry& entry : state.entries) {
        json.beginObject();
        json.key("name");
        json.text(entry.name);
        json.key("governor");
        json.text(entry.governor);
        json.key("frequency");
        json.text(entry.frequency);
        json.endObject();
    }
    json.endArray();
    json.endObject();
}

double meanOf(const std::vector<double>& values) noexcept {
    double sum{0.0};
    for (const double value : values) {
        sum += value;
    }
    return values.empty() ? 0.0 : sum / static_cast<double>(values.size());
}

std::string resultJson(const Options& options, const NpuInfer& model, Result& result,
                       const FreqState& before, const FreqState& after) {
    const double count{static_cast<double>(result.latencies.size())};
    const double mean{meanOf(result.latencies)};
    const Percentiles latency{percentilesOf(result.latencies)};
    JsonWriter json;
    json.beginObject();
    json.key("schema");
    json.integer(schemaVersion);
    json.key("stage");
    json.text("infer");
    json.key("model");
    json.text(options.modelPath);
    json.key("cores");
    json.text(options.coresName);
    json.key("input_side");
    json.integer(model.inputSide());
    json.key("outputs");
    json.beginArray();
    for (std::size_t index{0}; index < model.outputCount(); ++index) {
        json.beginArray();
        for (const std::uint32_t dim : model.outputDims(index)) {
            json.integer(dim);
        }
        json.endArray();
    }
    json.endArray();
    json.key("runs");
    json.integer(options.runs);
    json.key("warmup");
    json.integer(options.warmup);
    json.key("p50_ms");
    json.number(latency.p50);
    json.key("p95_ms");
    json.number(latency.p95);
    json.key("p99_ms");
    json.number(latency.p99);
    json.key("mean_ms");
    json.number(mean);
    json.key("inferences_per_second");
    json.number(millisPerSecond / mean);
    json.key("cpu_ms_per_inference");
    json.number(result.cpuMs / count);
    json.key("system_cpu_percent");
    json.number(result.busyPercentValue);
    json.key("frequency_before");
    writeFreq(json, before);
    json.key("frequency_after");
    writeFreq(json, after);
    json.endObject();
    return json.str();
}
} // namespace

int main(int argc, char** argv) {
    const Logger log{"BENCH", LogLevel::Info};
    const std::optional<Options> options{parseOptions(commandLine(argc, argv), log)};
    if (!options) {
        return exitUsage;
    }
    const std::string path{options->modelPath};
    std::optional<NpuInfer> model{NpuInfer::create(NpuConfig{path.c_str(), options->cores}, log)};
    if (!model) {
        return exitFailure;
    }
    std::optional<DmaBuf> input{DmaBuf::allocate(DmaBufConfig{lowHeap, model->inputBytes()}, log)};
    if (!input) {
        return exitFailure;
    }
    std::memset(input->data(), idleValue, input->size());
    if (!input->endCpuAccess()) {
        return exitFailure;
    }
    const std::optional<InputId> inputId{model->attachInput(*input)};
    if (!inputId) {
        return exitFailure;
    }
    const FreqState before{readFreqState()};
    log.info("model ", path, " input ", model->inputSide(), " cores ", options->coresName,
             " runs=", options->runs, " warmup=", options->warmup,
             " frequencies locked: ", before.locked ? "yes" : "no");
    for (int run{0}; run < options->warmup; ++run) {
        if (!model->run(*inputId)) {
            return exitFailure;
        }
    }
    Result result{};
    result.latencies.reserve(static_cast<std::size_t>(options->runs));
    const CpuTimes busyBegin{readCpuTimes()};
    const double cpuBegin{processCpuMs()};
    for (int run{0}; run < options->runs; ++run) {
        const double begin{wallMs()};
        if (!model->run(*inputId)) {
            return exitFailure;
        }
        result.latencies.push_back(wallMs() - begin);
    }
    result.cpuMs = processCpuMs() - cpuBegin;
    result.busyPercentValue = busyPercent(busyBegin, readCpuTimes());
    const FreqState after{readFreqState()};
    if (before.locked != after.locked) {
        log.warn("the frequency lock changed during the run");
    }
    std::vector<double> sorted{result.latencies};
    const Percentiles latency{percentilesOf(sorted)};
    log.info("p50=", Fixed{latency.p50, millisPrecision},
             " p95=", Fixed{latency.p95, millisPrecision},
             " p99=", Fixed{latency.p99, millisPrecision},
             " ms cpu=", Fixed{result.cpuMs / options->runs, millisPrecision},
             " ms/inference system busy=", Fixed{result.busyPercentValue, 1}, "%");
    if (!options->jsonPath.empty()) {
        return writeTextFile(std::string{options->jsonPath},
                             resultJson(*options, *model, result, before, after), log)
                   ? EXIT_SUCCESS
                   : exitFailure;
    }
    return EXIT_SUCCESS;
}
