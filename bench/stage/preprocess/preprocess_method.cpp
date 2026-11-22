#include "preprocess_method.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>

#include <opencv2/core/utility.hpp>
#include <opencv2/imgproc.hpp>

#include "im2d.hpp"

namespace {
constexpr int paddingValue{114}; // rk3588_common.model_io, the value RgaLetterbox pads with
constexpr int cpuThreads{8};
constexpr int channels{3};
constexpr int strideAlignment{16};
constexpr int minSide{4};
constexpr int evenMask{1};
constexpr int nv12Numerator{3};
constexpr int nv12Denominator{2};

std::size_t targetBytes(int targetSize) noexcept {
    return static_cast<std::size_t>(targetSize) * static_cast<std::size_t>(targetSize) * channels;
}

// A refusal of the driver: every status of a failed improcess call except a lack of memory. An
// RGA2 call that the driver rejects (RgaBlit: Invalid argument) comes back as IM_STATUS_FAILED.
bool isRefusal(IM_STATUS status) noexcept {
    return status != IM_STATUS_OUT_OF_MEMORY;
}

// BT.601 full range NV12 to RGB, 16-bit fixed point: the coefficients 1.402, 0.344136, 0.714136 and
// 1.772 times 65536. The colour of a 2 x 2 block is repeated for its four pixels, as the NV12
// conversion of OpenCV does.
constexpr int fixedShift{16};
constexpr int fixedHalf{1 << (fixedShift - 1)};
constexpr int redFromCr{91881};
constexpr int greenFromCb{22554};
constexpr int greenFromCr{46802};
constexpr int blueFromCb{116130};
constexpr int chromaMid{128};
constexpr int byteMax{255};
constexpr int blockSide{2};

std::uint8_t toByte(int value) noexcept {
    return static_cast<std::uint8_t>(std::clamp(value >> fixedShift, 0, byteMax));
}

// Converts the rows of the range: `luma` is height x width, `chroma` is height / 2 rows of
// interleaved Cb Cr bytes, `rgb` is height x width x 3.
void convertRows(const cv::Mat& luma, const cv::Mat& chroma, cv::Mat& rgb,
                 const cv::Range& rows) noexcept {
    const int width{luma.cols};
    for (int row{rows.start}; row < rows.end; ++row) {
        const std::uint8_t* lumaRow{luma.ptr<std::uint8_t>(row)};
        const std::uint8_t* chromaRow{chroma.ptr<std::uint8_t>(row / blockSide)};
        std::uint8_t* out{rgb.ptr<std::uint8_t>(row)};
        for (int col{0}; col < width; col += blockSide) {
            const int blue{chromaRow[col] - chromaMid};
            const int red{chromaRow[col + 1] - chromaMid};
            const int redTerm{(redFromCr * red) + fixedHalf};
            const int greenTerm{fixedHalf - (greenFromCb * blue) - (greenFromCr * red)};
            const int blueTerm{(blueFromCb * blue) + fixedHalf};
            for (int pixel{0}; pixel < blockSide; ++pixel) {
                const int base{lumaRow[col + pixel] << fixedShift};
                std::uint8_t* rgbOut{out + (static_cast<std::ptrdiff_t>(col + pixel) * channels)};
                rgbOut[0] = toByte(base + redTerm);
                rgbOut[1] = toByte(base + greenTerm);
                rgbOut[2] = toByte(base + blueTerm);
            }
        }
    }
}

bool isEven(int value) noexcept {
    return (value & evenMask) == 0;
}

int nv12Rows(int height) noexcept {
    return height * nv12Numerator / nv12Denominator;
}

bool isCpu(MethodKind kind) noexcept {
    switch (kind) {
    case MethodKind::Cv1:
    case MethodKind::Cv8:
        return true;
    case MethodKind::Rga2:
    case MethodKind::Rga30:
    case MethodKind::Rga31:
    case MethodKind::RgaAuto:
        break;
    }
    return false;
}

int threadsOf(MethodKind kind) noexcept {
    return kind == MethodKind::Cv8 ? cpuThreads : 1;
}

// The core of an RGA method that is forced to one core.
RgaCore coreOf(MethodKind kind) noexcept {
    switch (kind) {
    case MethodKind::Rga30:
        return RgaCore::Rga30;
    case MethodKind::Rga31:
        return RgaCore::Rga31;
    default:
        break;
    }
    return RgaCore::Rga2;
}
} // namespace

std::string_view statusName(MethodStatus status) noexcept {
    switch (status) {
    case MethodStatus::Ok:
        return "ok";
    case MethodStatus::Rejected:
        return "rejected";
    case MethodStatus::Failed:
        break;
    }
    return "failed";
}

std::string_view methodLabel(MethodKind kind) noexcept {
    switch (kind) {
    case MethodKind::Cv1:
        return "cv1";
    case MethodKind::Cv8:
        return "cv8";
    case MethodKind::Rga2:
        return "R2";
    case MethodKind::Rga30:
        return "R30";
    case MethodKind::Rga31:
        return "R31";
    case MethodKind::RgaAuto:
        break;
    }
    return "Ra";
}

cv::Mat nv12View(SourceFrame& frame) {
    return cv::Mat(nv12Rows(frame.size.height), frame.size.width, CV_8UC1, frame.buffer.data(),
                   static_cast<std::size_t>(frame.size.width));
}

std::optional<SourceFrame> makeSourceFrame(Pattern pattern, ImageSize size, std::string_view heap,
                                           const Logger& log) {
    if (size.width < minSide || size.height < minSide || !isEven(size.width) ||
        !isEven(size.height) || size.width % strideAlignment != 0) {
        log.error("bad frame size ", size.width, "x", size.height,
                  ": both sides even and at least ", minSide, ", the width a multiple of ",
                  strideAlignment);
        return std::nullopt;
    }
    const std::size_t bytes{static_cast<std::size_t>(size.width) *
                            static_cast<std::size_t>(nv12Rows(size.height))};
    std::optional<DmaBuf> buffer{DmaBuf::allocate(DmaBufConfig{heap, bytes}, log)};
    if (!buffer) {
        return std::nullopt;
    }
    std::optional<RgaImage> image{RgaImage::import(*buffer, log)};
    if (!image) {
        return std::nullopt;
    }
    cv::Mat view(nv12Rows(size.height), size.width, CV_8UC1, buffer->data(),
                 static_cast<std::size_t>(size.width));
    fillNv12(pattern, view, size);
    if (!buffer->endCpuAccess()) {
        return std::nullopt;
    }
    return SourceFrame{std::move(*buffer), std::move(*image), size};
}

std::optional<PreprocessMethod> PreprocessMethod::create(MethodKind kind, SourceFrame& source,
                                                         int targetSize, const Logger& log) {
    // The rotation by 180 degrees keeps the size, so the layout is that of the source.
    const std::optional<LetterboxLayout> layout{letterboxLayout(source.size, targetSize)};
    if (!layout) {
        log.error("no letterbox layout for ", source.size.width, "x", source.size.height);
        return std::nullopt;
    }
    std::optional<DmaBuf> buffer{
        DmaBuf::allocate(DmaBufConfig{lowHeap, targetBytes(targetSize)}, log)};
    if (!buffer) {
        return std::nullopt;
    }
    std::optional<RgaImage> image{RgaImage::import(*buffer, log)};
    if (!image) {
        return std::nullopt;
    }
    PreprocessMethod built{kind, source, std::move(*buffer), std::move(*image)};
    // cv::Mat is built with parentheses everywhere: with braces, three ints (rows, columns, type)
    // select the initializer_list constructor and make a 3 x 1 matrix of ints.
    built.targetView = cv::Mat(targetSize, targetSize, CV_8UC3, built.target.data());
    if (isCpu(kind)) {
        const ImageSize scaled{layout->scaled};
        built.threads = threadsOf(kind);
        built.frameView =
            cv::Mat(nv12Rows(source.size.height), source.size.width, CV_8UC1, source.buffer.data(),
                    static_cast<std::size_t>(source.size.width));
        built.lumaView = built.frameView.rowRange(0, source.size.height);
        built.chromaView = built.frameView.rowRange(source.size.height, built.frameView.rows);
        built.placed =
            built.targetView(cv::Rect{layout->left, layout->top, scaled.width, scaled.height});
        built.smallRgb = cv::Mat(scaled.height, scaled.width, CV_8UC3);
        built.fullRgb = cv::Mat(source.size.height, source.size.width, CV_8UC3);
        return std::optional<PreprocessMethod>{std::move(built)};
    }
    // The camera reports full range, so the application passes fullRange = true.
    const LetterboxConfig config{source.size, source.size.width, targetSize, Rotation::Rotate180,
                                 true};
    if (kind == MethodKind::RgaAuto) {
        built.kernelChoice = RgaLetterbox::create(config, log);
        if (!built.kernelChoice) {
            return std::nullopt;
        }
    } else {
        built.forcedCore = RgaForcedLetterbox::create(config, coreOf(kind), log);
        if (!built.forcedCore) {
            return std::nullopt;
        }
    }
    return std::optional<PreprocessMethod>{std::move(built)};
}

std::optional<int> PreprocessMethod::prepare() noexcept {
    if (isCpu(method)) {
        cv::setNumThreads(threads);
        const cv::Scalar padding{paddingValue, paddingValue, paddingValue};
        targetView.setTo(padding);
        if (!target.endCpuAccess()) {
            return std::nullopt;
        }
        return cv::getNumThreads();
    }
    bool padded{false};
    if (kernelChoice) {
        padded = kernelChoice->fillPadding(targetImage);
    } else if (forcedCore) {
        padded = forcedCore->fillPadding(targetImage);
    }
    if (!padded) {
        return std::nullopt;
    }
    return 1;
}

bool PreprocessMethod::run() noexcept {
    if (isCpu(method)) {
        return runCpu();
    }
    if (kernelChoice) {
        return kernelChoice->run(source->image, targetImage);
    }
    // An RGA method always has one of the two converters: create() fails without it.
    return forcedCore && forcedCore->run(source->image, targetImage);
}

bool PreprocessMethod::runCpu() {
    // A device wrote the frame: the CPU cache is stale until this sync.
    if (!source->buffer.beginCpuAccess()) {
        return false;
    }
    // The order of rk3588_common.model_io.preprocess: convert at full size, then scale.
    // The frame is BT.601 full range. cv::COLOR_YUV2RGB_NV12 decodes limited range only, so one
    // pass over the planes does the conversion, on the threads that OpenCV is set to.
    cv::parallel_for_(cv::Range(0, fullRgb.rows), [this](const cv::Range& rows) {
        convertRows(lumaView, chromaView, fullRgb, rows);
    });
    cv::resize(fullRgb, smallRgb, smallRgb.size(), 0.0, 0.0, cv::INTER_LINEAR);
    // Same size and type as `placed`, so the rotation writes into the target.
    cv::rotate(smallRgb, placed, cv::ROTATE_180);
    // A device reads the target next: push the CPU writes out.
    return target.endCpuAccess();
}

Failure PreprocessMethod::failure() const {
    if (forcedCore) {
        const IM_STATUS status{forcedCore->lastStatus()};
        return Failure{isRefusal(status) ? MethodStatus::Rejected : MethodStatus::Failed,
                       forcedCore->lastMessage()};
    }
    if (kernelChoice) {
        return Failure{MethodStatus::Failed,
                       "RgaLetterbox::run returned false, the library logged the reason"};
    }
    return Failure{MethodStatus::Failed, "CPU path: padding or cache sync failed"};
}

bool PreprocessMethod::copyOutput(cv::Mat& out) const {
    if (!target.beginCpuAccess()) {
        return false;
    }
    targetView.copyTo(out);
    return true;
}
