#include "preprocess_method.h"

#include <cstddef>
#include <utility>

#include <opencv2/imgproc.hpp>

namespace {
constexpr int paddingValue{114}; // rk3588_common.model_io, the value RgaLetterbox pads with
constexpr int cpuThreads{8};
constexpr int channels{3};
constexpr int strideAlignment{16};
constexpr int minSide{4};
constexpr int evenMask{1};
constexpr int nv12Numerator{3};
constexpr int nv12Denominator{2};

constexpr std::size_t targetBytes{static_cast<std::size_t>(defaultModelSize) *
                                  static_cast<std::size_t>(defaultModelSize) * channels};

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
                                                         const Logger& log) {
    // The rotation by 180 degrees keeps the size, so the layout is that of the source.
    const std::optional<LetterboxLayout> layout{letterboxLayout(source.size, defaultModelSize)};
    if (!layout) {
        log.error("no letterbox layout for ", source.size.width, "x", source.size.height);
        return std::nullopt;
    }
    std::optional<DmaBuf> buffer{DmaBuf::allocate(DmaBufConfig{lowHeap, targetBytes}, log)};
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
    built.targetView = cv::Mat(defaultModelSize, defaultModelSize, CV_8UC3, built.target.data());
    if (isCpu(kind)) {
        const ImageSize scaled{layout->scaled};
        built.threads = threadsOf(kind);
        built.frameView =
            cv::Mat(nv12Rows(source.size.height), source.size.width, CV_8UC1, source.buffer.data(),
                    static_cast<std::size_t>(source.size.width));
        built.placed =
            built.targetView(cv::Rect{layout->left, layout->top, scaled.width, scaled.height});
        built.smallRgb = cv::Mat(scaled.height, scaled.width, CV_8UC3);
        built.fullRgb = cv::Mat(source.size.height, source.size.width, CV_8UC3);
        return std::optional<PreprocessMethod>{std::move(built)};
    }
    const LetterboxConfig config{source.size, source.size.width, defaultModelSize,
                                 Rotation::Rotate180, false};
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
    cv::cvtColor(frameView, fullRgb, cv::COLOR_YUV2RGB_NV12);
    cv::resize(fullRgb, smallRgb, smallRgb.size(), 0.0, 0.0, cv::INTER_LINEAR);
    // Same size and type as `placed`, so the rotation writes into the target.
    cv::rotate(smallRgb, placed, cv::ROTATE_180);
    // A device reads the target next: push the CPU writes out.
    return target.endCpuAccess();
}

bool PreprocessMethod::copyOutput(cv::Mat& out) const {
    if (!target.beginCpuAccess()) {
        return false;
    }
    targetView.copyTo(out);
    return true;
}
