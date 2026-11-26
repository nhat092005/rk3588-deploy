// Stage 3 bench, the hardware side: the H.265 encoder of the board (MPP, VideoEncoder as the
// application uses it) on a clip of NV12 frames. The CPU side (x265) is run with
// tools/bench/bench_encode_x265.sh, on the same clip.
// Usage: bench_encode --input CLIP.nv12 [--size WxH] [--frames N] [--kbps N] [--fps N] [--gop N]
//                     [--runs N] [--warmup N] [--out FILE.h265] [--json FILE]
// Defaults: 3840x2160, all the frames of the clip, 20000 kbps constant bitrate, 30 FPS, a key frame
// every 30 frames (the application settings), 3 timed runs after 1 warmup run, full range (the
// camera of the board reports it). The run is meant for a board with the frequencies locked
// (sh/env/lock_freq.sh): the bench does not lock them, it records their state in the JSON.
// Every run encodes the clip with a new encoder, frame after frame back to back. A frame is copied
// from the mapped file into one of three dma-bufs and synced for the device first (not timed): the
// encoder reads a buffer that a device-side writer has just filled, as with a camera frame. Per run
// it prints the encode-only frames per second (frames over the sum of the encode times), the
// latency of one encode (p50, p95, p99), the CPU time of the process per frame (all threads,
// waiting not counted, around the encode only), the bytes of the stream and its bitrate.
// --out writes the stream of the last run (Annex B), for the quality check of the x265 script.
// Exit code 0 on success, 1 when the encoder fails, 2 on a bad command line.
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
#include "json_writer.h"
#include "logger.h"
#include "mpp_source.h"
#include "timing.h"
#include "video_encoder.h"

namespace {
constexpr int exitFailure{1};
constexpr int exitUsage{2};
constexpr int defaultWidth{3840};
constexpr int defaultHeight{2160};
constexpr int defaultRuns{3};
constexpr int defaultWarmup{1};
constexpr int maxCount{1000};
constexpr int ringSize{3};
constexpr double millisPerSecond{1000.0};
constexpr double bitsPerByte{8.0};
constexpr double bitsPerKilobit{1000.0};
constexpr int millisPrecision{3};
constexpr int schemaVersion{1};
constexpr std::size_t chromaNumerator{3};
constexpr std::size_t chromaDenominator{2};

struct Options {
    std::string_view inputPath;
    int width{defaultWidth};
    int height{defaultHeight};
    int frames{0}; // 0: every frame of the file
    int kbps{defaultVideoKbps};
    int fps{defaultVideoFps};
    int gop{defaultVideoFps};
    int runs{defaultRuns};
    int warmup{defaultWarmup};
    std::string_view outPath;  // empty: no stream file
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

bool applyOption(Options& options, const Argument& argument) noexcept {
    const std::string_view name{argument.name};
    const std::string_view value{argument.value};
    if (name == "--input") {
        options.inputPath = value;
        return !value.empty();
    }
    if (name == "--size") {
        return applySize(options, value);
    }
    if (name == "--frames") {
        return applyCount(options.frames, value, 1, maxCount);
    }
    if (name == "--kbps") {
        return applyCount(options.kbps, value, 1, maxVideoKbps);
    }
    if (name == "--fps") {
        return applyCount(options.fps, value, 1, maxVideoFps);
    }
    if (name == "--gop") {
        return applyCount(options.gop, value, 1, maxCount);
    }
    if (name == "--runs") {
        return applyCount(options.runs, value, 1, maxCount);
    }
    if (name == "--warmup") {
        return applyCount(options.warmup, value, 0, maxCount);
    }
    if (name == "--out") {
        options.outPath = value;
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
            log.error("usage: bench_encode --input CLIP.nv12 [--size WxH] [--frames N] ",
                      "[--kbps N] [--fps N] [--gop N] [--runs N] [--warmup N] ",
                      "[--out FILE.h265] [--json FILE]");
            return std::nullopt;
        }
    }
    if (options.inputPath.empty()) {
        log.error("--input is required");
        return std::nullopt;
    }
    if (((options.width | options.height) & 1) != 0) {
        log.error("the width and the height must be even");
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

// One dma-buf of the ring and its import into MPP.
struct RingSlot {
    DmaBuf buffer;
    MppSource source;
};

std::optional<std::vector<RingSlot>> makeRing(std::size_t frameBytes, const Logger& log) {
    std::vector<RingSlot> ring;
    ring.reserve(ringSize);
    for (int slot{0}; slot < ringSize; ++slot) {
        std::optional<DmaBuf> buffer{DmaBuf::allocate(DmaBufConfig{defaultHeap, frameBytes}, log)};
        if (!buffer) {
            return std::nullopt;
        }
        std::optional<MppSource> source{MppSource::import(*buffer, log)};
        if (!source) {
            return std::nullopt;
        }
        ring.push_back(RingSlot{std::move(*buffer), std::move(*source)});
    }
    return ring;
}

// What one run gave.
struct RunResult {
    int frames{0};
    int keyFrames{0};
    std::size_t bytes{0};
    double encodeMs{0.0}; // sum of the encode times
    double cpuMs{0.0};    // process CPU time around the encodes
    std::vector<double> latencies;
    std::vector<std::uint8_t> stream;
};

// Encodes the first `frames` frames of the clip with a new encoder. Returns empty on a failure.
std::optional<RunResult> encodeClip(const Clip& clip, const Options& options, int frames,
                                    std::vector<RingSlot>& ring, const Logger& log) {
    const auto frameBytes{static_cast<std::size_t>(options.width) *
                          static_cast<std::size_t>(options.height) * chromaNumerator /
                          chromaDenominator};
    VideoConfig config;
    config.width = options.width;
    config.height = options.height;
    config.horStride = options.width;
    config.verStride = options.height;
    config.fullRange = true;
    config.kbps = options.kbps;
    config.framesPerSecond = options.fps;
    config.gopFrames = options.gop;
    std::optional<VideoEncoder> encoder{VideoEncoder::create(config, log)};
    if (!encoder) {
        return std::nullopt;
    }
    RunResult result{};
    result.latencies.reserve(static_cast<std::size_t>(frames));
    for (int index{0}; index < frames; ++index) {
        RingSlot& slot{ring.at(static_cast<std::size_t>(index) % ringSize)};
        // Not timed: the frame goes into the buffer and is synced for the device.
        if (!slot.buffer.beginCpuAccess()) {
            return std::nullopt;
        }
        std::memcpy(slot.buffer.data(), clip.frame(static_cast<std::size_t>(index), frameBytes),
                    frameBytes);
        if (!slot.buffer.endCpuAccess()) {
            return std::nullopt;
        }
        const double cpuBegin{processCpuMs()};
        const double begin{wallMs()};
        const std::optional<VideoPacket> packet{encoder->encode(slot.source)};
        const double elapsed{wallMs() - begin};
        result.cpuMs += processCpuMs() - cpuBegin;
        if (!packet) {
            return std::nullopt;
        }
        result.latencies.push_back(elapsed);
        result.encodeMs += elapsed;
        result.bytes += packet->size;
        result.keyFrames += packet->keyFrame ? 1 : 0;
        result.stream.insert(result.stream.end(), packet->data, packet->data + packet->size);
    }
    result.frames = frames;
    return result;
}

double kbpsOf(const RunResult& run, int fps) noexcept {
    const double seconds{static_cast<double>(run.frames) / fps};
    return static_cast<double>(run.bytes) * bitsPerByte / seconds / bitsPerKilobit;
}

double fpsOf(const RunResult& run) noexcept {
    return run.frames * millisPerSecond / run.encodeMs;
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

std::string resultJson(const Options& options, const std::vector<RunResult>& runs,
                       const FreqState& before, const FreqState& after) {
    std::vector<double> all;
    double cpuMs{0.0};
    int frames{0};
    for (const RunResult& run : runs) {
        all.insert(all.end(), run.latencies.begin(), run.latencies.end());
        cpuMs += run.cpuMs;
        frames += run.frames;
    }
    const Percentiles latency{percentilesOf(all)};
    JsonWriter json;
    json.beginObject();
    json.key("schema");
    json.integer(schemaVersion);
    json.key("stage");
    json.text("encode");
    json.key("method");
    json.text("mpp");
    json.key("width");
    json.integer(options.width);
    json.key("height");
    json.integer(options.height);
    json.key("target_kbps");
    json.integer(options.kbps);
    json.key("fps");
    json.integer(options.fps);
    json.key("gop");
    json.integer(options.gop);
    json.key("frames_per_run");
    json.integer(runs.front().frames);
    json.key("runs");
    json.integer(static_cast<long long>(runs.size()));
    json.key("warmup_runs");
    json.integer(options.warmup);
    json.key("p50_ms");
    json.number(latency.p50);
    json.key("p95_ms");
    json.number(latency.p95);
    json.key("p99_ms");
    json.number(latency.p99);
    json.key("cpu_ms_per_frame");
    json.number(cpuMs / frames);
    json.key("per_run");
    json.beginArray();
    for (const RunResult& run : runs) {
        json.beginObject();
        json.key("encode_fps");
        json.number(fpsOf(run));
        json.key("cpu_ms_per_frame");
        json.number(run.cpuMs / run.frames);
        json.key("bytes");
        json.integer(static_cast<long long>(run.bytes));
        json.key("kbps");
        json.number(kbpsOf(run, options.fps));
        json.key("key_frames");
        json.integer(run.keyFrames);
        json.endObject();
    }
    json.endArray();
    json.key("frequency_before");
    writeFreq(json, before);
    json.key("frequency_after");
    writeFreq(json, after);
    json.endObject();
    return json.str();
}

bool writeStream(std::string_view path, const std::vector<std::uint8_t>& stream,
                 const Logger& log) {
    std::ofstream file{std::string{path}, std::ios::binary};
    file.write(reinterpret_cast<const char*>(stream.data()),
               static_cast<std::streamsize>(stream.size()));
    if (!file) {
        log.error(path, ": cannot write the stream");
        return false;
    }
    return true;
}
} // namespace

int main(int argc, char** argv) {
    const Logger log{"BENCH", LogLevel::Info};
    const std::optional<Options> options{parseOptions(commandLine(argc, argv), log)};
    if (!options) {
        return exitUsage;
    }
    const auto frameBytes{static_cast<std::size_t>(options->width) *
                          static_cast<std::size_t>(options->height) * chromaNumerator /
                          chromaDenominator};
    const std::optional<Clip> clip{Clip::open(std::string{options->inputPath}, frameBytes, log)};
    if (!clip) {
        return exitUsage;
    }
    const int frames{options->frames > 0
                         ? std::min(options->frames, static_cast<int>(clip->frames()))
                         : static_cast<int>(clip->frames())};
    log.info("clip ", options->width, "x", options->height, " frames=", frames, " ", options->kbps,
             " kbps ", options->fps, " fps gop ", options->gop, " runs=", options->runs,
             " warmup=", options->warmup);
    std::optional<std::vector<RingSlot>> ring{makeRing(frameBytes, log)};
    if (!ring) {
        return exitFailure;
    }
    const FreqState before{readFreqState()};
    log.info("frequencies locked: ", before.locked ? "yes" : "no");
    std::vector<RunResult> runs;
    for (int run{0}; run < options->warmup + options->runs; ++run) {
        std::optional<RunResult> result{encodeClip(*clip, *options, frames, *ring, log)};
        if (!result) {
            return exitFailure;
        }
        const bool timed{run >= options->warmup};
        log.info(timed ? "run " : "warmup ", timed ? run - options->warmup : run,
                 " encode fps=", Fixed{fpsOf(*result), 1}, " bytes=", result->bytes,
                 " kbps=", Fixed{kbpsOf(*result, options->fps), 0}, " key=", result->keyFrames,
                 " cpu=", Fixed{result->cpuMs / frames, millisPrecision}, " ms/frame");
        if (timed) {
            runs.push_back(std::move(*result));
        }
    }
    const FreqState after{readFreqState()};
    if (before.locked != after.locked) {
        log.warn("the frequency lock changed during the run");
    }
    std::vector<double> all;
    for (const RunResult& run : runs) {
        all.insert(all.end(), run.latencies.begin(), run.latencies.end());
    }
    const Percentiles latency{percentilesOf(all)};
    log.info("encode latency p50=", Fixed{latency.p50, millisPrecision},
             " p95=", Fixed{latency.p95, millisPrecision},
             " p99=", Fixed{latency.p99, millisPrecision}, " ms");
    bool written{true};
    if (!options->outPath.empty()) {
        written = writeStream(options->outPath, runs.back().stream, log);
    }
    if (!options->jsonPath.empty()) {
        written = writeTextFile(std::string{options->jsonPath},
                                resultJson(*options, runs, before, after), log) &&
                  written;
    }
    return written ? EXIT_SUCCESS : exitFailure;
}
