// JPEG snapshot of a real camera frame: the RGA copies the frame (rotated upright) into a buffer of
// its own, the camera buffer goes back at once, the MPP encoder turns the copy into a JPEG.
// Usage: probe_jpeg <node> <width> <height> <rotation> <quality> <out_prefix> [store_root]
//   node     : camera capture node, for example /dev/video11
//   width    : frame width in pixels, for example 3840
//   height   : frame height in pixels, for example 2160
//   rotation : 0, 90, 180 or 270 degrees clockwise
//   quality  : JPEG quality factor, 1 to 99
//   out_prefix writes <out_prefix>.nv12 (the copy, NV12 without padding) and <out_prefix>.jpg
//   store_root : optional, a directory that exists (for example /data/violations): the JPEG is also
//                saved there through ViolationStore as a made-up violation of person 1
// Logs the RGA copy time, the encode time and the JPEG size. tools/check/check_jpeg.py compares the
// two files. Exit code 0 when the JPEG has a start and an end marker, 2 on bad arguments.
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <numeric>
#include <optional>
#include <string>
#include <vector>

#include "arguments.h"
#include "camera.h"
#include "dmabuf.h"
#include "jpeg_encoder.h"
#include "logger.h"
#include "mpp_source.h"
#include "rga.h"
#include "violation_store.h"

namespace {
constexpr int exitUsage{2};
constexpr int decimalBase{10};
constexpr int timePrecision{3};
constexpr long maxDimension{16'384};
constexpr std::size_t nodeArg{1};
constexpr std::size_t widthArg{2};
constexpr std::size_t heightArg{3};
constexpr std::size_t rotationArg{4};
constexpr std::size_t qualityArg{5};
constexpr std::size_t prefixArg{6};
constexpr std::size_t storeArg{7};
constexpr std::size_t argsCount{7};
constexpr std::size_t argsCountWithStore{8};
constexpr int warmupFrames{5};
constexpr int timedRuns{10};
constexpr std::uint8_t markerPrefix{0xFF};
constexpr std::uint8_t startMarker{0xD8};
constexpr std::uint8_t endMarker{0xD9};
constexpr std::size_t markerBytes{2};
constexpr float quarter{4.0F};
constexpr float threeTimes{3.0F};

struct Options {
    const char* node{nullptr};
    int width{0};
    int height{0};
    Rotation rotation{Rotation::None};
    int quality{0};
    const char* prefix{nullptr};
    const char* storeRoot{nullptr};
};

// A positive int up to `limit`, or empty for junk, zero, negative or too large text.
std::optional<int> parseBounded(const char* text, long limit) noexcept {
    char* end{nullptr};
    const long value{std::strtol(text, &end, decimalBase)};
    if (end == text || *end != '\0' || value <= 0 || value > limit) {
        return std::nullopt;
    }
    return static_cast<int>(value);
}

std::optional<Options> parseArgs(const std::vector<const char*>& args, const Logger& log) {
    if (args.size() != argsCount && args.size() != argsCountWithStore) {
        log.error("usage: ", args.empty() ? "probe_jpeg" : args.at(0),
                  " <node> <width> <height> <rotation 0|90|180|270> <quality 1-99> <out_prefix>"
                  " [store_root]");
        return std::nullopt;
    }
    const std::optional<int> width{parseBounded(args.at(widthArg), maxDimension)};
    const std::optional<int> height{parseBounded(args.at(heightArg), maxDimension)};
    const std::optional<Rotation> rotation{parseRotation(args.at(rotationArg))};
    const std::optional<int> quality{parseBounded(args.at(qualityArg), maxJpegQuality)};
    if (!width || !height || !rotation || !quality) {
        log.error("bad size, rotation or quality");
        return std::nullopt;
    }
    return Options{args.at(nodeArg),
                   *width,
                   *height,
                   *rotation,
                   *quality,
                   args.at(prefixArg),
                   args.size() > storeArg ? args.at(storeArg) : nullptr};
}

// Waits for a frame and dequeues it, skipping `warmupFrames` frames so that exposure settles.
std::optional<Frame> grabFrame(const Camera& camera, const StopSignal& stop, const Logger& log) {
    int skipped{0};
    while (true) {
        if (camera.waitFrame(stop) != WaitStatus::Ready) {
            log.error("no frame");
            return std::nullopt;
        }
        const Dequeued dequeued{camera.dequeue()};
        if (dequeued.status == DequeueStatus::Failed) {
            return std::nullopt;
        }
        if (dequeued.status == DequeueStatus::NotReady) {
            continue;
        }
        if (skipped >= warmupFrames) {
            return dequeued.frame;
        }
        ++skipped;
        if (!camera.queue(dequeued.frame.index)) {
            return std::nullopt;
        }
    }
}

double milliseconds(std::chrono::steady_clock::duration span) {
    return std::chrono::duration<double, std::milli>(span).count();
}

// Median of the samples, which are reordered.
double median(std::vector<double>& samples) {
    std::sort(samples.begin(), samples.end());
    return samples.at(samples.size() / 2);
}

bool writeFile(const std::string& path, const void* data, std::size_t length, const Logger& log) {
    std::ofstream file{path, std::ios::binary};
    file.write(static_cast<const char*>(data), static_cast<std::streamsize>(length));
    file.close();
    if (!file) {
        log.error("writing ", path, " failed");
        return false;
    }
    return true;
}

// The snapshot: copy the frame, give the camera buffer back, time `timedRuns` copies first.
bool takeSnapshot(const Camera& camera, const Frame& frame, const RgaSnapshot& snapshot,
                  DmaBuf& target, const Logger& log) {
    const std::optional<RgaImage> sourceImage{
        RgaImage::import(camera.planeBuffer(frame.index, 0), log)};
    const std::optional<RgaImage> targetImage{RgaImage::import(target, log)};
    if (!sourceImage || !targetImage) {
        return false;
    }
    std::vector<double> times;
    for (int run{0}; run < timedRuns; ++run) {
        const auto start{std::chrono::steady_clock::now()};
        if (!snapshot.run(*sourceImage, *targetImage)) {
            return false;
        }
        times.push_back(milliseconds(std::chrono::steady_clock::now() - start));
    }
    log.info("RGA snapshot: median ", Fixed{median(times), timePrecision}, " ms, min ",
             Fixed{times.front(), timePrecision}, " ms, max ", Fixed{times.back(), timePrecision},
             " ms");
    // The copy is done: the camera gets its buffer back before anything slow starts.
    return camera.queue(frame.index);
}

// A made-up violation of person 1 in the middle of the image, saved through ViolationStore when
// a root was given.
bool storeViolation(const std::uint8_t* data, std::size_t length, const ImageSize& size,
                    const Options& options, const Logger& log) {
    if (options.storeRoot == nullptr) {
        return true;
    }
    const std::optional<ViolationStore> store{
        ViolationStore::create(StoreConfig{options.storeRoot, defaultFileMode}, log)};
    if (!store) {
        return false;
    }
    ViolationRecord record;
    record.event.trackId = 1;
    record.event.noHelmet = true;
    record.event.startSeconds = 0.0;
    record.event.nowSeconds = defaultHoldSeconds;
    record.event.left = static_cast<float>(size.width) / quarter;
    record.event.top = static_cast<float>(size.height) / quarter;
    record.event.right = record.event.left * threeTimes;
    record.event.bottom = record.event.top * threeTimes;
    record.wallTime = std::time(nullptr);
    record.imageWidth = size.width;
    record.imageHeight = size.height;
    record.original = StoredBytes{data, length};
    const std::optional<std::string> path{store->save(record)};
    if (path) {
        log.info("saved ", *path);
    }
    return path.has_value();
}

bool encodeSnapshot(DmaBuf& target, const ImageSize& size, int stride, bool fullRange,
                    const Options& options, const Logger& log) {
    std::optional<JpegEncoder> encoder{JpegEncoder::create(
        JpegConfig{size.width, size.height, stride, size.height, fullRange, options.quality}, log)};
    const std::optional<MppSource> source{MppSource::import(target, log)};
    if (!encoder || !source) {
        return false;
    }
    std::vector<double> times;
    std::size_t length{0};
    for (int run{0}; run < timedRuns; ++run) {
        const auto start{std::chrono::steady_clock::now()};
        const std::optional<std::size_t> bytes{encoder->encode(*source)};
        if (!bytes) {
            return false;
        }
        times.push_back(milliseconds(std::chrono::steady_clock::now() - start));
        length = *bytes;
    }
    log.info("JPEG encode: median ", Fixed{median(times), timePrecision}, " ms, min ",
             Fixed{times.front(), timePrecision}, " ms, max ", Fixed{times.back(), timePrecision},
             " ms, ", length, " bytes at quality ", options.quality);
    const std::uint8_t* data{encoder->data()};
    const bool markers{length > markerBytes && data[0] == markerPrefix && data[1] == startMarker &&
                       data[length - 2] == markerPrefix && data[length - 1] == endMarker};
    log.info("JPEG markers ", markers ? "ok" : "BAD");
    const std::string prefix{options.prefix};
    const bool saved{writeFile(prefix + ".jpg", data, length, log) && markers};
    return saved && storeViolation(data, length, size, options, log);
}

bool run(const Camera& camera, const Frame& frame, const Options& options, const Logger& log) {
    const CameraFormat& format{camera.format()};
    const std::optional<RgaSnapshot> snapshot{
        RgaSnapshot::create(SnapshotConfig{ImageSize{format.width, format.height},
                                           format.bytesPerLine, options.rotation},
                            log)};
    if (!snapshot) {
        return false;
    }
    std::optional<DmaBuf> target{
        DmaBuf::allocate(DmaBufConfig{defaultHeap, snapshot->bytes()}, log)};
    if (!target || !takeSnapshot(camera, frame, *snapshot, *target, log)) {
        return false;
    }
    if (!target->beginCpuAccess()) {
        return false;
    }
    const std::string prefix{options.prefix};
    const bool dumped{writeFile(prefix + ".nv12", target->data(), snapshot->bytes(), log)};
    if (!target->endCpuAccess()) {
        return false;
    }
    log.info("copy ", snapshot->size().width, "x", snapshot->size().height);
    return dumped && encodeSnapshot(*target, snapshot->size(), snapshot->stride(), format.fullRange,
                                    options, log);
}
} // namespace

int main(int argc, char** argv) {
    const std::vector<const char*> args{commandLine(argc, argv)};
    const Logger log{"PROBE_JPEG", LogLevel::Debug};
    const std::optional<Options> options{parseArgs(args, log)};
    if (!options) {
        return exitUsage;
    }

    bool passed{false};
    const std::optional<StopSignal> stop{StopSignal::create(log)};
    if (stop) {
        std::optional<Camera> camera{Camera::open(
            CameraConfig{options->node, options->width, options->height, defaultHeap}, log)};
        if (camera) {
            const std::optional<Frame> frame{grabFrame(*camera, *stop, log)};
            passed = frame && run(*camera, *frame, *options, log);
            passed = camera->close() && passed;
        }
    }
    if (passed) {
        log.info("PASS");
    } else {
        log.error("FAIL");
    }
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
