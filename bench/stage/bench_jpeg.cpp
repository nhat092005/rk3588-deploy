// Stage 4 bench: the JPEG encoder of the board (MPP, JpegEncoder as the application uses it for a
// violation snapshot) against libjpeg-turbo on one CPU thread, on NV12 images.
// Usage: bench_jpeg --input CLIP.nv12 [--size WxH] [--qualities 50,75,90,99] [--methods mpp,turbo]
//                   [--runs N] [--warmup N] [--frames N] [--step N] [--out-dir DIR] [--json FILE]
// Defaults: 3840x2160, the qualities above (90 is the application default), both methods, 300 timed
// encodes after 30 warmup encodes for every method and quality, `--frames` 8 images of the clip
// (image i is frame i x `--step`, step 11) taken in turn. The run is meant for a board with the
// frequencies locked (sh/env/lock_freq.sh): the bench records their state in the JSON.
// Every encode is one image, as in the application, with a new frame: the frame is copied from the
// mapped file into a dma-buf and synced for the device first (not timed), so the encoder reads a
// buffer that a device-side writer has just filled. The timed part of mpp is JpegEncoder::encode
// (the copy of the JPEG into its own buffer included). The timed part of turbo is the CPU access to
// the dma-buf (begin and end sync), the split of the CbCr plane and the libjpeg encode. Per case:
// the latency (p50, p95, p99), the CPU time of the process per image (all threads, waiting not
// counted), the bytes of the JPEG and, for turbo, the median split time.
// --out-dir writes the JPEG of each of the images for every case (<method>_q<Q>_f<I>.jpg), for the
// quality check of tools/accuracy/check_jpeg_bench.py.
// Exit code 0 on success, 1 when an encoder fails, 2 on a bad command line.
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "arguments.h"
#include "dmabuf.h"
#include "freq_state.h"
#include "jpeg_encoder.h"
#include "json_writer.h"
#include "logger.h"
#include "mpp_source.h"
#include "timing.h"
#include "turbo_encoder.h"

namespace {
constexpr int exitFailure{1};
constexpr int exitUsage{2};
constexpr int blockSize{16};
constexpr int defaultWidth{3840};
constexpr int defaultHeight{2160};
constexpr int defaultRuns{300};
constexpr int defaultWarmup{30};
constexpr int defaultFrames{8};
constexpr int defaultStep{11};
constexpr int maxCount{100'000};
constexpr double millisPerSecond{1000.0};
constexpr int millisPrecision{3};
constexpr int schemaVersion{1};
constexpr std::size_t chromaNumerator{3};
constexpr std::size_t chromaDenominator{2};
constexpr std::array<int, 4> defaultQualities{50, 75, 90, 99};

enum class Method : std::uint8_t { Mpp, Turbo };

std::string_view nameOf(Method method) noexcept {
    return method == Method::Mpp ? "mpp" : "turbo";
}

struct Options {
    std::string_view inputPath;
    int width{defaultWidth};
    int height{defaultHeight};
    std::vector<int> qualities{defaultQualities.begin(), defaultQualities.end()};
    std::vector<Method> methods{Method::Mpp, Method::Turbo};
    int runs{defaultRuns};
    int warmup{defaultWarmup};
    int frames{defaultFrames};
    int step{defaultStep};
    std::string_view outDir;   // empty: no JPEG files
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

// The pieces of a comma separated list.
std::vector<std::string_view> splitList(std::string_view text) {
    std::vector<std::string_view> pieces;
    while (true) {
        const std::size_t comma{text.find(',')};
        pieces.push_back(text.substr(0, comma));
        if (comma == std::string_view::npos) {
            return pieces;
        }
        text.remove_prefix(comma + 1);
    }
}

bool applySize(Options& options, std::string_view text) noexcept {
    const std::size_t cross{text.find('x')};
    if (cross == std::string_view::npos) {
        return false;
    }
    const std::optional<int> width{readInt(text.substr(0, cross))};
    const std::optional<int> height{readInt(text.substr(cross + 1))};
    if (!width || !height || *width <= 0 || *height <= 0) {
        return false;
    }
    options.width = *width;
    options.height = *height;
    return true;
}

// Sets an integer option; `target` receives the value when it is in [low, high].
bool applyCount(int& target, std::string_view text, int low, int high) noexcept {
    const std::optional<int> value{readInt(text)};
    if (!value || *value < low || *value > high) {
        return false;
    }
    target = *value;
    return true;
}

bool applyQualities(Options& options, std::string_view text) {
    options.qualities.clear();
    for (const std::string_view piece : splitList(text)) {
        int quality{0};
        // Both encoders take 1 to 99 (the MPP one stops there).
        if (!applyCount(quality, piece, minJpegQuality, maxJpegQuality)) {
            return false;
        }
        options.qualities.push_back(quality);
    }
    return true;
}

bool applyMethods(Options& options, std::string_view text) {
    options.methods.clear();
    for (const std::string_view piece : splitList(text)) {
        if (piece == "mpp") {
            options.methods.push_back(Method::Mpp);
        } else if (piece == "turbo") {
            options.methods.push_back(Method::Turbo);
        } else {
            return false;
        }
    }
    return true;
}

bool applyOption(Options& options, const Argument& argument) {
    const std::string_view name{argument.name};
    const std::string_view value{argument.value};
    if (name == "--input") {
        options.inputPath = value;
        return !value.empty();
    }
    if (name == "--size") {
        return applySize(options, value);
    }
    if (name == "--qualities") {
        return applyQualities(options, value);
    }
    if (name == "--methods") {
        return applyMethods(options, value);
    }
    if (name == "--runs") {
        return applyCount(options.runs, value, 1, maxCount);
    }
    if (name == "--warmup") {
        return applyCount(options.warmup, value, 0, maxCount);
    }
    if (name == "--frames") {
        return applyCount(options.frames, value, 1, maxCount);
    }
    if (name == "--step") {
        return applyCount(options.step, value, 1, maxCount);
    }
    if (name == "--out-dir") {
        options.outDir = value;
        return !value.empty();
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
            log.error("usage: bench_jpeg --input CLIP.nv12 [--size WxH] [--qualities Q,Q,...] ",
                      "[--methods mpp,turbo] [--runs N] [--warmup N] [--frames N] [--step N] ",
                      "[--out-dir DIR] [--json FILE]");
            return std::nullopt;
        }
    }
    if (options.inputPath.empty()) {
        log.error("--input is required");
        return std::nullopt;
    }
    return options;
}

// A mapped file: where it starts, its length in bytes and how many frames it holds.
struct Mapped {
    void* address;
    std::size_t length;
    std::size_t frames;
};

// The clip, mapped read-only from its file. Move-only.
class Clip {
public:
    static std::optional<Clip> open(const std::string& path, std::size_t frameBytes,
                                    const Logger& log) {
        const int descriptor{::open(path.c_str(), O_RDONLY)};
        struct stat info{};
        if (descriptor < 0 || fstat(descriptor, &info) != 0) {
            log.error(path, ": cannot open the file");
            return std::nullopt;
        }
        const auto length{static_cast<std::size_t>(info.st_size)};
        void* mapped{mmap(nullptr, length, PROT_READ, MAP_PRIVATE, descriptor, 0)};
        ::close(descriptor);
        if (mapped == MAP_FAILED) {
            log.error(path, ": cannot map the file");
            return std::nullopt;
        }
        if (length < frameBytes || length % frameBytes != 0) {
            log.error(path, ": ", length, " bytes is not a whole number of frames of ", frameBytes);
            munmap(mapped, length);
            return std::nullopt;
        }
        return Clip{Mapped{mapped, length, length / frameBytes}};
    }
    Clip(const Clip&) = delete;
    Clip& operator=(const Clip&) = delete;
    Clip(Clip&& other) noexcept
        : region{std::exchange(other.region.address, nullptr), other.region.length,
                 other.region.frames} {}
    Clip& operator=(Clip&&) = delete;
    ~Clip() {
        if (region.address != nullptr) {
            munmap(region.address, region.length);
        }
    }
    const std::uint8_t* frame(std::size_t index, std::size_t frameBytes) const noexcept {
        return static_cast<const std::uint8_t*>(region.address) + (index * frameBytes);
    }
    std::size_t frames() const noexcept {
        return region.frames;
    }

private:
    explicit Clip(const Mapped& mapped) : region{mapped} {}
    Mapped region;
};

// The dma-buf that the images go through and its import into MPP.
struct Slot {
    DmaBuf buffer;
    MppSource source;
};

std::optional<Slot> makeSlot(std::size_t frameBytes, const Logger& log) {
    std::optional<DmaBuf> buffer{DmaBuf::allocate(DmaBufConfig{defaultHeap, frameBytes}, log)};
    if (!buffer) {
        return std::nullopt;
    }
    std::optional<MppSource> source{MppSource::import(*buffer, log)};
    if (!source) {
        return std::nullopt;
    }
    return Slot{std::move(*buffer), std::move(*source)};
}

// What a case gave.
struct CaseResult {
    Method method{Method::Mpp};
    int quality{0};
    std::vector<double> latencies;
    std::vector<double> splits; // turbo only
    double cpuMs{0.0};
    double bytes{0.0};
    std::vector<std::size_t> imageBytes; // one per image of the cycle
};

// One encoder of either method, behind one call that returns the bytes of the JPEG.
class Codec {
public:
    static std::optional<Codec> create(Method method, const Options& options, int quality,
                                       const Logger& log) {
        Codec codec;
        codec.method = method;
        if (method == Method::Mpp) {
            const JpegConfig config{options.width,  options.height, options.width,
                                    options.height, true,           quality};
            codec.mpp = JpegEncoder::create(config, log);
            if (!codec.mpp) {
                return std::nullopt;
            }
        } else {
            codec.turbo =
                TurboEncoder::create(TurboConfig{options.width, options.height, quality}, log);
            if (!codec.turbo) {
                return std::nullopt;
            }
        }
        return codec;
    }

    // Encodes the image in the slot. `split` receives the split time of turbo, 0 for mpp.
    std::optional<std::size_t> encode(const Slot& slot, double& split) noexcept {
        split = 0.0;
        if (mpp) {
            return mpp->encode(slot.source);
        }
        if (!turbo || !slot.buffer.beginCpuAccess()) {
            return std::nullopt;
        }
        const TurboResult result{
            turbo->encode(static_cast<const std::uint8_t*>(slot.buffer.data()))};
        split = result.splitMs;
        if (!slot.buffer.endCpuAccess()) {
            return std::nullopt;
        }
        return result.bytes;
    }

    const std::uint8_t* data() const noexcept {
        if (mpp) {
            return mpp->data();
        }
        return turbo ? turbo->data() : nullptr;
    }

private:
    Codec() = default;
    Method method{Method::Mpp};
    std::optional<JpegEncoder> mpp;
    std::optional<TurboEncoder> turbo;
};

// Which image of the cycle to load and how many bytes a frame has.
struct Cycle {
    int index;
    std::size_t frameBytes;
};

// Copies image `index` of the cycle into the slot and syncs it for the device (not timed).
bool load(const Clip& clip, const Options& options, Slot& slot, const Cycle& cycle) {
    const std::size_t frame{static_cast<std::size_t>(cycle.index % options.frames) *
                            static_cast<std::size_t>(options.step)};
    if (!slot.buffer.beginCpuAccess()) {
        return false;
    }
    std::memcpy(slot.buffer.data(), clip.frame(frame, cycle.frameBytes), cycle.frameBytes);
    return slot.buffer.endCpuAccess();
}

bool writeFile(const std::string& path, const std::uint8_t* data, std::size_t size,
               const Logger& log) {
    std::ofstream file{path, std::ios::binary};
    file.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    if (!file) {
        log.error(path, ": cannot write the JPEG");
        return false;
    }
    return true;
}

// Times `runs` encodes after `warmup` ones, then encodes each image of the cycle once more to learn
// its size (and write it). Returns empty on a failure.
std::optional<CaseResult> runCase(const Clip& clip, const Options& options, Slot& slot,
                                  CaseResult initial, const Logger& log) {
    const auto frameBytes{static_cast<std::size_t>(options.width) *
                          static_cast<std::size_t>(options.height) * chromaNumerator /
                          chromaDenominator};
    std::optional<Codec> codec{Codec::create(initial.method, options, initial.quality, log)};
    if (!codec) {
        return std::nullopt;
    }
    CaseResult result{std::move(initial)};
    result.latencies.reserve(static_cast<std::size_t>(options.runs));
    for (int run{0}; run < options.warmup + options.runs; ++run) {
        if (!load(clip, options, slot, Cycle{run, frameBytes})) {
            return std::nullopt;
        }
        double split{0.0};
        const double cpuBegin{processCpuMs()};
        const double begin{wallMs()};
        const std::optional<std::size_t> bytes{codec->encode(slot, split)};
        const double elapsed{wallMs() - begin};
        const double cpu{processCpuMs() - cpuBegin};
        if (!bytes) {
            return std::nullopt;
        }
        if (run >= options.warmup) {
            result.latencies.push_back(elapsed);
            result.cpuMs += cpu;
            result.bytes += static_cast<double>(*bytes);
            if (result.method == Method::Turbo) {
                result.splits.push_back(split);
            }
        }
    }
    for (int index{0}; index < options.frames; ++index) {
        double split{0.0};
        if (!load(clip, options, slot, Cycle{index, frameBytes})) {
            return std::nullopt;
        }
        const std::optional<std::size_t> bytes{codec->encode(slot, split)};
        if (!bytes) {
            return std::nullopt;
        }
        result.imageBytes.push_back(*bytes);
        if (!options.outDir.empty()) {
            const std::string path{
                std::string{options.outDir} + "/" + std::string{nameOf(result.method)} + "_q" +
                std::to_string(result.quality) + "_f" + std::to_string(index) + ".jpg"};
            if (!writeFile(path, codec->data(), *bytes, log)) {
                return std::nullopt;
            }
        }
    }
    return result;
}

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

void writeCase(JsonWriter& json, CaseResult& run) {
    const double count{static_cast<double>(run.latencies.size())};
    const double mean{meanOf(run.latencies)};
    const Percentiles latency{percentilesOf(run.latencies)};
    json.beginObject();
    json.key("method");
    json.text(nameOf(run.method));
    json.key("quality");
    json.integer(run.quality);
    json.key("p50_ms");
    json.number(latency.p50);
    json.key("p95_ms");
    json.number(latency.p95);
    json.key("p99_ms");
    json.number(latency.p99);
    json.key("mean_ms");
    json.number(mean);
    json.key("images_per_second");
    json.number(millisPerSecond / mean);
    json.key("cpu_ms_per_image");
    json.number(run.cpuMs / count);
    json.key("mean_bytes");
    json.number(run.bytes / count);
    if (!run.splits.empty()) {
        json.key("split_p50_ms");
        json.number(percentilesOf(run.splits).p50);
    }
    json.key("image_bytes");
    json.beginArray();
    for (const std::size_t bytes : run.imageBytes) {
        json.integer(static_cast<long long>(bytes));
    }
    json.endArray();
    json.endObject();
}

std::string resultJson(const Options& options, std::vector<CaseResult>& cases,
                       const FreqState& before, const FreqState& after) {
    JsonWriter json;
    json.beginObject();
    json.key("schema");
    json.integer(schemaVersion);
    json.key("stage");
    json.text("jpeg");
    json.key("width");
    json.integer(options.width);
    json.key("height");
    json.integer(options.height);
    json.key("runs");
    json.integer(options.runs);
    json.key("warmup");
    json.integer(options.warmup);
    json.key("images_in_cycle");
    json.integer(options.frames);
    json.key("frame_step");
    json.integer(options.step);
    json.key("cases");
    json.beginArray();
    for (CaseResult& run : cases) {
        writeCase(json, run);
    }
    json.endArray();
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
    if (options->width % blockSize != 0 || options->height % blockSize != 0) {
        log.error("the width and the height must be multiples of 16");
        return exitUsage;
    }
    const auto frameBytes{static_cast<std::size_t>(options->width) *
                          static_cast<std::size_t>(options->height) * chromaNumerator /
                          chromaDenominator};
    const std::optional<Clip> clip{Clip::open(std::string{options->inputPath}, frameBytes, log)};
    if (!clip) {
        return exitUsage;
    }
    const auto lastFrame{static_cast<std::size_t>(options->frames - 1) *
                         static_cast<std::size_t>(options->step)};
    if (lastFrame >= clip->frames()) {
        log.error("image ", options->frames - 1, " is frame ", lastFrame, ", the clip has ",
                  clip->frames());
        return exitUsage;
    }
    log.info("images ", options->width, "x", options->height, " cycle of ", options->frames,
             " (step ", options->step, ") runs=", options->runs, " warmup=", options->warmup);
    std::optional<Slot> slot{makeSlot(frameBytes, log)};
    if (!slot) {
        return exitFailure;
    }
    const FreqState before{readFreqState()};
    log.info("frequencies locked: ", before.locked ? "yes" : "no");
    std::vector<CaseResult> cases;
    for (const int quality : options->qualities) {
        for (const Method method : options->methods) {
            CaseResult initial;
            initial.method = method;
            initial.quality = quality;
            std::optional<CaseResult> result{
                runCase(*clip, *options, *slot, std::move(initial), log)};
            if (!result) {
                return exitFailure;
            }
            std::vector<double> sorted{result->latencies};
            const Percentiles latency{percentilesOf(sorted)};
            const double count{static_cast<double>(result->latencies.size())};
            log.info(nameOf(method), " q", quality, " p50=", Fixed{latency.p50, millisPrecision},
                     " p95=", Fixed{latency.p95, millisPrecision},
                     " p99=", Fixed{latency.p99, millisPrecision},
                     " ms cpu=", Fixed{result->cpuMs / count, millisPrecision},
                     " ms bytes=", Fixed{result->bytes / count, 0});
            cases.push_back(std::move(*result));
        }
    }
    const FreqState after{readFreqState()};
    if (before.locked != after.locked) {
        log.warn("the frequency lock changed during the run");
    }
    if (!options->jsonPath.empty()) {
        return writeTextFile(std::string{options->jsonPath},
                             resultJson(*options, cases, before, after), log)
                   ? EXIT_SUCCESS
                   : exitFailure;
    }
    return EXIT_SUCCESS;
}
