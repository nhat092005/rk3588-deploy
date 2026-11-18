// Overlay on a real 4K camera frame: the RGA copies the frame upright into a buffer of the
// lowHeap, RgaOverlay draws made-up boxes (three people with a head and a vest box, one of them a
// violator, plus a crowd of other boxes) and RgaLabels their labels. Logs the time of each and
// writes the picture as a JPEG to look at.
// Usage: probe_overlay <node> <width> <height> <rotation> <out_prefix>
//   node     : camera capture node, for example /dev/video11
//   width    : frame width in pixels, for example 3840
//   height   : frame height in pixels, for example 2160
//   rotation : 0, 90, 180 or 270 degrees clockwise
//   out_prefix writes <out_prefix>.jpg
// Exit code 0 when the drawing worked, 2 on bad arguments.
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
#include "jpeg_encoder.h"
#include "logger.h"
#include "mpp_source.h"
#include "rga.h"
#include "rga_labels.h"
#include "rga_overlay.h"

namespace {
constexpr int exitUsage{2};
constexpr int decimalBase{10};
constexpr int timePrecision{3};
constexpr long maxDimension{16'384};
constexpr std::size_t nodeArg{1};
constexpr std::size_t widthArg{2};
constexpr std::size_t heightArg{3};
constexpr std::size_t rotationArg{4};
constexpr std::size_t prefixArg{5};
constexpr std::size_t argsCount{6};
constexpr int warmupFrames{5};
constexpr int timedRuns{20};
constexpr int crowdBoxes{24};
constexpr std::uint64_t seed{20261116};
constexpr unsigned shiftLeftFirst{13U};
constexpr unsigned shiftRight{7U};
constexpr unsigned shiftLeftSecond{17U};
constexpr int minDivisor{14};
constexpr int maxDivisor{8};
constexpr int classCount{5};
constexpr int helmetClass{0};
constexpr int noHelmetClass{1};
constexpr int noVestClass{2};
constexpr int vestClass{3};
constexpr int personClass{4};
constexpr float crowdScore{0.6F};

struct Options {
    const char* node{nullptr};
    int width{0};
    int height{0};
    Rotation rotation{Rotation::None};
    const char* prefix{nullptr};
};

std::optional<int> parseBounded(const char* text) noexcept {
    char* end{nullptr};
    const long value{std::strtol(text, &end, decimalBase)};
    if (end == text || *end != '\0' || value <= 0 || value > maxDimension) {
        return std::nullopt;
    }
    return static_cast<int>(value);
}

std::optional<Options> parseArgs(const std::vector<const char*>& args, const Logger& log) {
    if (args.size() != argsCount) {
        log.error("usage: ", args.empty() ? "probe_overlay" : args.at(0),
                  " <node> <width> <height> <rotation 0|90|180|270> <out_prefix>");
        return std::nullopt;
    }
    const std::optional<int> width{parseBounded(args.at(widthArg))};
    const std::optional<int> height{parseBounded(args.at(heightArg))};
    const std::optional<Rotation> rotation{parseRotation(args.at(rotationArg))};
    if (!width || !height || !rotation) {
        log.error("bad size or rotation");
        return std::nullopt;
    }
    return Options{args.at(nodeArg), *width, *height, *rotation, args.at(prefixArg)};
}

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

// xorshift64: the same boxes on every run.
class Random {
public:
    explicit Random(std::uint64_t start) : value{start} {}

    int next(int limit) {
        value ^= value << shiftLeftFirst;
        value ^= value >> shiftRight;
        value ^= value << shiftLeftSecond;
        return static_cast<int>(value % static_cast<std::uint64_t>(limit));
    }

private:
    std::uint64_t value;
};

// Where the made-up people stand and how they are built, in fractions of the image.
constexpr float personHalfWidth{0.07F};
constexpr float personTop{0.25F};
constexpr float personBottom{0.95F};
constexpr float headInset{0.03F};
constexpr float headTop{0.01F};
constexpr float headBottom{0.1F};
constexpr float vestInset{0.01F};
constexpr float vestTop{0.2F};
constexpr float vestBottom{0.45F};
constexpr float personScore{0.95F};
constexpr float headScore{0.87F};
constexpr float vestScore{0.78F};

// A made-up person.
struct PersonSpec {
    float centreX{0.0F};
    int trackId{0};
    bool violator{false};
};

constexpr std::array<PersonSpec, 3> people{{
    {0.2F, 1, false},
    {0.5F, 2, true},
    {0.8F, 3, false},
}};

// A person with a head and a vest box.
void addPerson(std::vector<OverlayBox>& boxes, const ImageSize& size, const PersonSpec& spec) {
    const float width{static_cast<float>(size.width)};
    const float height{static_cast<float>(size.height)};
    const float left{(spec.centreX - personHalfWidth) * width};
    const float right{(spec.centreX + personHalfWidth) * width};
    boxes.push_back(OverlayBox{PixelBox{left, personTop * height, right, personBottom * height},
                               personClass, spec.violator, personScore, spec.trackId});
    boxes.push_back(
        OverlayBox{PixelBox{left + (headInset * width), (personTop + headTop) * height,
                            right - (headInset * width), (personTop + headBottom) * height},
                   spec.violator ? noHelmetClass : helmetClass, false, headScore, -1});
    boxes.push_back(
        OverlayBox{PixelBox{left + (vestInset * width), (personTop + vestTop) * height,
                            right - (vestInset * width), (personTop + vestBottom) * height},
                   spec.violator ? noVestClass : vestClass, false, vestScore, -1});
}

std::vector<OverlayBox> makeScene(const ImageSize& size) {
    std::vector<OverlayBox> boxes;
    for (const PersonSpec& spec : people) {
        addPerson(boxes, size, spec);
    }
    Random random{seed};
    for (int index{0}; index < crowdBoxes; ++index) {
        const int boxWidth{(size.width / minDivisor) + random.next(size.width / maxDivisor)};
        const int boxHeight{(size.height / minDivisor) + random.next(size.height / maxDivisor)};
        const int left{random.next(size.width - boxWidth)};
        const int top{random.next(size.height - boxHeight)};
        boxes.push_back(OverlayBox{PixelBox{static_cast<float>(left), static_cast<float>(top),
                                            static_cast<float>(left + boxWidth),
                                            static_cast<float>(top + boxHeight)},
                                   index % classCount, false, crowdScore, -1});
    }
    return boxes;
}

double median(std::vector<double>& samples) {
    std::sort(samples.begin(), samples.end());
    return samples.at(samples.size() / 2);
}

// Draws the scene `timedRuns` times, logs the median time of the outlines and of the labels.
bool drawScene(RgaOverlay& overlay, RgaLabels& labels, const RgaImage& target,
               const std::vector<OverlayBox>& boxes, const Logger& log) {
    std::vector<double> outline;
    std::vector<double> text;
    for (int run{0}; run < timedRuns; ++run) {
        const auto start{std::chrono::steady_clock::now()};
        if (!overlay.draw(target, boxes)) {
            return false;
        }
        const auto middle{std::chrono::steady_clock::now()};
        if (!labels.draw(target, boxes)) {
            return false;
        }
        outline.push_back(milliseconds(middle - start));
        text.push_back(milliseconds(std::chrono::steady_clock::now() - middle));
    }
    log.info(boxes.size(), " boxes: outlines median ", Fixed{median(outline), timePrecision},
             " ms, labels median ", Fixed{median(text), timePrecision}, " ms");
    return true;
}

bool encodeAndWrite(DmaBuf& snapshot, const ImageSize& size, int stride, bool fullRange,
                    const std::string& path, const Logger& log) {
    std::optional<JpegEncoder> encoder{JpegEncoder::create(
        JpegConfig{size.width, size.height, stride, size.height, fullRange}, log)};
    const std::optional<MppSource> source{MppSource::import(snapshot, log)};
    if (!encoder || !source) {
        return false;
    }
    const std::optional<std::size_t> length{encoder->encode(*source)};
    if (!length) {
        return false;
    }
    std::ofstream file{path, std::ios::binary};
    file.write(reinterpret_cast<const char*>(encoder->data()),
               static_cast<std::streamsize>(*length));
    file.close();
    log.info("wrote ", path, " ", *length, " bytes");
    return static_cast<bool>(file);
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
    std::optional<DmaBuf> target{DmaBuf::allocate(DmaBufConfig{lowHeap, snapshot->bytes()}, log)};
    if (!target) {
        return false;
    }
    const std::optional<RgaImage> targetImage{RgaImage::import(*target, log)};
    {
        const std::optional<RgaImage> sourceImage{
            RgaImage::import(camera.planeBuffer(frame.index, 0), log)};
        if (!sourceImage || !targetImage || !snapshot->run(*sourceImage, *targetImage) ||
            !camera.queue(frame.index)) {
            return false;
        }
    }
    OverlayConfig config;
    config.size = snapshot->size();
    config.stride = snapshot->stride();
    std::optional<RgaOverlay> overlay{RgaOverlay::create(config, log)};
    std::optional<RgaLabels> labels{RgaLabels::create(config, LabelConfig{}, log)};
    if (!overlay || !labels || !overlay->check(*targetImage)) {
        return false;
    }
    const std::vector<OverlayBox> boxes{makeScene(snapshot->size())};
    return drawScene(*overlay, *labels, *targetImage, boxes, log) &&
           encodeAndWrite(*target, snapshot->size(), snapshot->stride(), format.fullRange,
                          std::string{options.prefix} + ".jpg", log);
}
} // namespace

int main(int argc, char** argv) {
    const std::vector<const char*> args{commandLine(argc, argv)};
    const Logger log{"PROBE_OVERLAY", LogLevel::Debug};
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
