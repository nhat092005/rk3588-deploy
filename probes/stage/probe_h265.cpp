// H.265 video of real camera frames: for every frame the RGA copies it (rotated upright) into one
// of two buffers of the low heap, the camera buffer goes back at once, VideoEncoder turns the copy
// into H.265. Logs the stage times, the bitrate and the free low-heap space (CMA) at three points.
// Usage: probe_h265 <node> <width> <height> <rotation> <kbps> <frames> <out_prefix>
//   node      : camera capture node, for example /dev/video11
//   width     : frame width in pixels, for example 3840
//   height    : frame height in pixels, for example 2160
//   rotation  : 0, 90, 180 or 270 degrees clockwise
//   kbps      : target bitrate of the constant bitrate mode, in kilobits per second
//   frames    : number of frames to encode, 2 or more
//   out_prefix writes <out_prefix>.h265 (Annex B stream)
// Exit code 0 when every frame was encoded and the stream starts with a key frame, 2 on bad
// arguments. Measured on the board: rotating in the MPP (prep:rotation) is no faster than in the
// RGA copy, so VideoEncoder does not rotate.
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "arguments.h"
#include "camera.h"
#include "dmabuf.h"
#include "logger.h"
#include "mpp_source.h"
#include "rga.h"
#include "video_encoder.h"

namespace {
constexpr int exitUsage{2};
constexpr int decimalBase{10};
constexpr int timePrecision{2};
constexpr long maxDimension{16'384};
constexpr long maxFrames{100'000};
constexpr std::size_t nodeArg{1};
constexpr std::size_t widthArg{2};
constexpr std::size_t heightArg{3};
constexpr std::size_t rotationArg{4};
constexpr std::size_t kbpsArg{5};
constexpr std::size_t framesArg{6};
constexpr std::size_t prefixArg{7};
constexpr std::size_t argsCount{8};
constexpr int warmupFrames{5};
constexpr int buffersInFlight{2};
constexpr std::size_t headroomChunk{4'194'304}; // 4 MiB
constexpr std::size_t headroomLimit{64};
constexpr double bytesPerMebibyte{1024.0 * 1024.0};
constexpr double msPerSecond{1000.0};
constexpr double bitsPerKilobit{1000.0};
constexpr std::size_t bitsPerByte{8};

struct Options {
    const char* node{nullptr};
    int width{0};
    int height{0};
    Rotation rotation{Rotation::None};
    int kbps{0};
    int frames{0};
    const char* prefix{nullptr};
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
    if (args.size() != argsCount) {
        log.error("usage: ", args.empty() ? "probe_h265" : args.at(0),
                  " <node> <width> <height> <rotation 0|90|180|270> <kbps> <frames> <out_prefix>");
        return std::nullopt;
    }
    const std::optional<int> width{parseBounded(args.at(widthArg), maxDimension)};
    const std::optional<int> height{parseBounded(args.at(heightArg), maxDimension)};
    const std::optional<Rotation> rotation{parseRotation(args.at(rotationArg))};
    const std::optional<int> kbps{parseBounded(args.at(kbpsArg), maxVideoKbps)};
    const std::optional<int> frames{parseBounded(args.at(framesArg), maxFrames)};
    if (!width || !height || !rotation || !kbps || !frames || *frames < 2) {
        log.error("bad size, rotation, kbps or frames");
        return std::nullopt;
    }
    return Options{args.at(nodeArg),  *width, *height, *rotation, *kbps, *frames,
                   args.at(prefixArg)};
}

double milliseconds(std::chrono::steady_clock::duration span) {
    return std::chrono::duration<double, std::milli>(span).count();
}

// Median of the samples, which are reordered.
double median(std::vector<double>& samples) {
    std::sort(samples.begin(), samples.end());
    return samples.at(samples.size() / 2);
}

// Median and largest of the samples.
void logStats(const Logger& log, const char* name, std::vector<double>& samples) {
    const double middle{median(samples)};
    log.info(name, ": median ", Fixed{middle, timePrecision}, " ms, max ",
             Fixed{samples.back(), timePrecision}, " ms");
}

// Megabytes of the low heap that can still be allocated, found by taking 4 MiB chunks until the
// heap refuses (the chunks are released when the function returns).
double lowHeapFreeMiB() {
    const Logger quiet{"HEADROOM", LogLevel::None};
    std::vector<DmaBuf> chunks;
    while (chunks.size() < headroomLimit) {
        std::optional<DmaBuf> chunk{DmaBuf::allocate(DmaBufConfig{lowHeap, headroomChunk}, quiet)};
        if (!chunk) {
            break;
        }
        chunks.push_back(std::move(*chunk));
    }
    return static_cast<double>(chunks.size() * headroomChunk) / bytesPerMebibyte;
}

void logHeadroom(const Logger& log, const char* when) {
    log.info("low heap free ", when, ": ", Fixed{lowHeapFreeMiB(), 0}, " MiB");
}

// Waits for a frame and dequeues it.
std::optional<Frame> nextFrame(const Camera& camera, const StopSignal& stop) {
    while (true) {
        if (camera.waitFrame(stop) != WaitStatus::Ready) {
            return std::nullopt;
        }
        const Dequeued dequeued{camera.dequeue()};
        if (dequeued.status == DequeueStatus::Failed) {
            return std::nullopt;
        }
        if (dequeued.status == DequeueStatus::Frame) {
            return dequeued.frame;
        }
    }
}

// What the loop needs: the RGA image of every camera buffer, the copy buffers with their RGA and
// MPP imports.
struct Pipeline {
    std::vector<RgaImage> cameraImages;
    std::vector<DmaBuf> copies;
    std::vector<RgaImage> copyImages;
    std::vector<MppSource> copySources;
};

std::optional<Pipeline> buildPipeline(const Camera& camera, const RgaSnapshot& snapshot,
                                      const Logger& log) {
    Pipeline pipeline;
    pipeline.cameraImages.reserve(camera.bufferCount());
    for (std::size_t index{0}; index < camera.bufferCount(); ++index) {
        std::optional<RgaImage> image{RgaImage::import(camera.planeBuffer(index, 0), log)};
        if (!image) {
            return std::nullopt;
        }
        pipeline.cameraImages.push_back(std::move(*image));
    }
    pipeline.copies.reserve(buffersInFlight);
    for (int index{0}; index < buffersInFlight; ++index) {
        std::optional<DmaBuf> copy{DmaBuf::allocate(DmaBufConfig{lowHeap, snapshot.bytes()}, log)};
        if (!copy) {
            return std::nullopt;
        }
        pipeline.copies.push_back(std::move(*copy));
    }
    for (DmaBuf& copy : pipeline.copies) {
        std::optional<RgaImage> image{RgaImage::import(copy, log)};
        std::optional<MppSource> source{MppSource::import(copy, log)};
        if (!image || !source) {
            return std::nullopt;
        }
        pipeline.copyImages.push_back(std::move(*image));
        pipeline.copySources.push_back(std::move(*source));
    }
    return pipeline;
}

struct Totals {
    std::vector<double> copyMs;
    std::vector<double> encodeMs;
    std::size_t bytes{0};
    std::size_t keyFrames{0};
    bool firstIsKey{false};
};

// Skips the first frames so that the exposure settles.
bool skipWarmup(const Camera& camera, const StopSignal& stop, const Logger& log) {
    for (int skipped{0}; skipped < warmupFrames; ++skipped) {
        const std::optional<Frame> frame{nextFrame(camera, stop)};
        if (!frame || !camera.queue(frame->index)) {
            log.error("no frame while warming up");
            return false;
        }
    }
    return true;
}

bool encodeLoop(const Camera& camera, const StopSignal& stop, const RgaSnapshot& snapshot,
                Pipeline& pipeline, VideoEncoder& encoder, const Options& options, Totals& totals,
                const Logger& log) {
    std::ofstream stream{std::string{options.prefix} + ".h265", std::ios::binary};
    for (int number{0}; number < options.frames; ++number) {
        const std::optional<Frame> frame{nextFrame(camera, stop)};
        if (!frame) {
            log.error("no frame");
            return false;
        }
        const auto slot{static_cast<std::size_t>(number % buffersInFlight)};
        const auto copyStart{std::chrono::steady_clock::now()};
        const bool copied{
            snapshot.run(pipeline.cameraImages.at(frame->index), pipeline.copyImages.at(slot))};
        totals.copyMs.push_back(milliseconds(std::chrono::steady_clock::now() - copyStart));
        if (!copied || !camera.queue(frame->index)) {
            return false;
        }
        const auto encodeStart{std::chrono::steady_clock::now()};
        const std::optional<VideoPacket> packet{encoder.encode(pipeline.copySources.at(slot))};
        totals.encodeMs.push_back(milliseconds(std::chrono::steady_clock::now() - encodeStart));
        if (!packet) {
            return false;
        }
        totals.firstIsKey = number == 0 ? packet->keyFrame : totals.firstIsKey;
        totals.keyFrames += packet->keyFrame ? 1U : 0U;
        totals.bytes += packet->size;
        stream.write(reinterpret_cast<const char*>(packet->data),
                     static_cast<std::streamsize>(packet->size));
    }
    stream.close();
    return static_cast<bool>(stream);
}

void report(Totals& totals, double seconds, const Options& options, const Logger& log) {
    const auto frames{static_cast<double>(options.frames)};
    const double kbps{static_cast<double>(totals.bytes * bitsPerByte) / (frames / defaultVideoFps) /
                      bitsPerKilobit};
    logStats(log, "RGA copy", totals.copyMs);
    logStats(log, "H.265 encode", totals.encodeMs);
    log.info("frames ", options.frames, ", key frames ", totals.keyFrames, ", first is key ",
             totals.firstIsKey, ", wall fps ", Fixed{frames / seconds, 1});
    log.info("stream ", totals.bytes, " bytes, ", Fixed{kbps, 0}, " kbps at ", defaultVideoFps,
             " fps, target ", options.kbps, " kbps");
}

bool run(const Camera& camera, const StopSignal& stop, const Options& options, const Logger& log) {
    const CameraFormat& format{camera.format()};
    const std::optional<RgaSnapshot> snapshot{
        RgaSnapshot::create(SnapshotConfig{ImageSize{format.width, format.height},
                                           format.bytesPerLine, options.rotation},
                            log)};
    if (!snapshot) {
        return false;
    }
    logHeadroom(log, "before buffers");
    std::optional<Pipeline> pipeline{buildPipeline(camera, *snapshot, log)};
    if (!pipeline) {
        return false;
    }
    logHeadroom(log, "with the two copy buffers");
    VideoConfig config;
    config.width = snapshot->size().width;
    config.height = snapshot->size().height;
    config.horStride = snapshot->stride();
    config.verStride = snapshot->size().height;
    config.fullRange = format.fullRange;
    config.kbps = options.kbps;
    std::optional<VideoEncoder> encoder{VideoEncoder::create(config, log)};
    if (!encoder || !skipWarmup(camera, stop, log)) {
        return false;
    }
    Totals totals;
    const auto start{std::chrono::steady_clock::now()};
    const bool encoded{
        encodeLoop(camera, stop, *snapshot, *pipeline, *encoder, options, totals, log)};
    const double seconds{milliseconds(std::chrono::steady_clock::now() - start) / msPerSecond};
    if (!encoded) {
        return false;
    }
    logHeadroom(log, "after the encoder ran");
    report(totals, seconds, options, log);
    return totals.firstIsKey;
}
} // namespace

int main(int argc, char** argv) {
    const std::vector<const char*> args{commandLine(argc, argv)};
    const Logger log{"PROBE_H265", LogLevel::Debug};
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
            passed = run(*camera, *stop, *options, log);
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
