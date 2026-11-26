// RGA letterbox check on real camera frames: NV12 frame -> rotation, scale, RGB conversion and
// padding in one RGA call, output 640x640 RGB888 (the model input).
// Usage: probe_letterbox <node> <width> <height> <rotation> <out_prefix>
//   node     : capture node, e.g. /dev/video12
//   width     : requested frame width, height: requested frame height
//   rotation : 0, 90, 180 or 270 degrees clockwise
//   out_prefix : writes <out_prefix>.nv12 (the camera frame) and <out_prefix>.rgb (the RGA output,
//                640x640x3). tools/check/check_letterbox.py compares the two with
//                model_io.preprocess.
// Checks: every byte outside the image area is 114, the image area is not all 114, and the time of
// 100 repeated RGA calls is logged. Exit code 0 only if all pass, 1 otherwise, 2 on bad arguments.
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "arguments.h"
#include "camera.h"
#include "dmabuf.h"
#include "logger.h"
#include "rga.h"

namespace {
// The model input of the measured models (this probe has no model).
constexpr int modelSize{640};
constexpr long maxDimension{16'384};
constexpr std::size_t rgbChannels{3};
constexpr std::uint8_t paddingValue{114};
constexpr int warmupFrames{15};
constexpr int timedRuns{100};
constexpr int exitUsage{2};
constexpr int decimalBase{10};
constexpr int timePrecision{3};
constexpr std::size_t nodeArg{1};
constexpr std::size_t widthArg{2};
constexpr std::size_t heightArg{3};
constexpr std::size_t rotationArg{4};
constexpr std::size_t prefixArg{5};
constexpr std::size_t argsCount{6};

struct Options {
    const char* node{nullptr};
    int width{0};
    int height{0};
    Rotation rotation{Rotation::None};
    const char* prefix{nullptr};
};

// A positive int, or empty for junk, zero, negative or too large text.
std::optional<int> parsePositive(const char* text) noexcept {
    char* end{nullptr};
    const long value{std::strtol(text, &end, decimalBase)};
    if (end == text || *end != '\0' || value <= 0 || value > maxDimension) {
        return std::nullopt;
    }
    return static_cast<int>(value);
}

std::optional<Options> parseArgs(const std::vector<const char*>& args, const Logger& log) {
    if (args.size() != argsCount) {
        log.error("usage: ", args.empty() ? "probe_letterbox" : args.at(0),
                  " <node> <width> <height> <rotation 0|90|180|270> <out_prefix>");
        return std::nullopt;
    }
    const std::optional<int> width{parsePositive(args.at(widthArg))};
    const std::optional<int> height{parsePositive(args.at(heightArg))};
    const std::optional<Rotation> rotation{parseRotation(args.at(rotationArg))};
    if (!width || !height || !rotation) {
        log.error("bad size or rotation");
        return std::nullopt;
    }
    return Options{args.at(nodeArg), *width, *height, *rotation, args.at(prefixArg)};
}

// Number of bytes outside the image area that are not the padding value.
std::size_t wrongPaddingBytes(const DmaBuf& target, const LetterboxLayout& layout) {
    const auto* bytes{static_cast<const std::uint8_t*>(target.data())};
    std::size_t wrong{0};
    for (int row{0}; row < modelSize; ++row) {
        for (int column{0}; column < modelSize; ++column) {
            const bool inside{row >= layout.top && row < layout.top + layout.scaled.height &&
                              column >= layout.left && column < layout.left + layout.scaled.width};
            if (inside) {
                continue;
            }
            const std::size_t offset{
                (static_cast<std::size_t>(row) * modelSize + static_cast<std::size_t>(column)) *
                rgbChannels};
            for (std::size_t channel{0}; channel < rgbChannels; ++channel) {
                wrong += bytes[offset + channel] != paddingValue ? 1 : 0;
            }
        }
    }
    return wrong;
}

// Number of bytes inside the image area that differ from the padding value.
std::size_t imageBytes(const DmaBuf& target, const LetterboxLayout& layout) {
    const auto* bytes{static_cast<const std::uint8_t*>(target.data())};
    std::size_t count{0};
    for (int row{layout.top}; row < layout.top + layout.scaled.height; ++row) {
        const std::size_t start{
            (static_cast<std::size_t>(row) * modelSize + static_cast<std::size_t>(layout.left)) *
            rgbChannels};
        const std::size_t length{static_cast<std::size_t>(layout.scaled.width) * rgbChannels};
        count += static_cast<std::size_t>(
            std::count_if(bytes + start, bytes + start + length,
                          [](std::uint8_t byte) { return byte != paddingValue; }));
    }
    return count;
}

bool writeRgb(const DmaBuf& target, const std::string& path, const Logger& log) {
    std::ofstream file{path, std::ios::binary};
    const std::size_t length{static_cast<std::size_t>(modelSize) * modelSize * rgbChannels};
    file.write(static_cast<const char*>(target.data()), static_cast<std::streamsize>(length));
    file.close();
    if (!file) {
        log.error("writing ", path, " failed");
        return false;
    }
    return true;
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

// The RGA part, on the frame that is held: padding, one conversion, timing, checks and dumps.
bool convertFrame(const Camera& camera, const Frame& frame, const Options& options,
                  const Logger& log) {
    const CameraFormat& format{camera.format()};
    const std::optional<RgaLetterbox> letterbox{RgaLetterbox::create(
        LetterboxConfig{ImageSize{format.width, format.height}, format.bytesPerLine, modelSize,
                        options.rotation, format.fullRange},
        log)};
    std::optional<DmaBuf> target{DmaBuf::allocate(
        DmaBufConfig{defaultHeap, static_cast<std::size_t>(modelSize) * modelSize * rgbChannels},
        log)};
    if (!letterbox || !target) {
        return false;
    }
    const std::optional<RgaImage> sourceImage{
        RgaImage::import(camera.planeBuffer(frame.index, 0), log)};
    const std::optional<RgaImage> targetImage{RgaImage::import(*target, log)};
    if (!sourceImage || !targetImage || !letterbox->fillPadding(*targetImage)) {
        return false;
    }

    const std::chrono::steady_clock::time_point start{std::chrono::steady_clock::now()};
    for (int run{0}; run < timedRuns; ++run) {
        if (!letterbox->run(*sourceImage, *targetImage)) {
            return false;
        }
    }
    const std::chrono::duration<double, std::milli> elapsed{std::chrono::steady_clock::now() -
                                                            start};
    log.info("RGA letterbox: ", Fixed{elapsed.count() / timedRuns, timePrecision}, " ms per call");

    if (!target->beginCpuAccess()) {
        return false;
    }
    const std::size_t wrongPadding{wrongPaddingBytes(*target, letterbox->layout())};
    const std::size_t imageData{imageBytes(*target, letterbox->layout())};
    log.info("padding bytes wrong=", wrongPadding, ", image bytes not 114=", imageData);
    const std::string prefix{options.prefix};
    const bool dumped{camera.writeFrame(frame, (prefix + ".nv12").c_str()) &&
                      writeRgb(*target, prefix + ".rgb", log)};
    return dumped && wrongPadding == 0 && imageData > 0;
}
} // namespace

int main(int argc, char** argv) {
    const std::vector<const char*> args{commandLine(argc, argv)};
    const Logger log{"PROBE_LETTERBOX", LogLevel::Debug};
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
            passed = frame && convertFrame(*camera, *frame, *options, log);
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
