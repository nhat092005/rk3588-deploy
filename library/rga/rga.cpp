#include "rga.h"

#include <utility>

#include "im2d.hpp"

namespace {
constexpr int paddingGray{114};
constexpr int noFence{-1};
constexpr std::size_t rgbChannels{3};
constexpr int opaqueAlpha{0xFF};
constexpr int channelBits{8};

// The RGA transform flag of a rotation, 0 for none.
int rotationFlag(Rotation rotation) noexcept {
    switch (rotation) {
    case Rotation::Rotate90:
        return IM_HAL_TRANSFORM_ROT_90;
    case Rotation::Rotate180:
        return IM_HAL_TRANSFORM_ROT_180;
    case Rotation::Rotate270:
        return IM_HAL_TRANSFORM_ROT_270;
    case Rotation::None:
        break;
    }
    return 0;
}

// Rotating by 90 or 270 degrees swaps the width and the height.
ImageSize rotatedSize(ImageSize size, Rotation rotation) noexcept {
    const bool swaps{rotation == Rotation::Rotate90 || rotation == Rotation::Rotate270};
    return swaps ? ImageSize{size.height, size.width} : size;
}

// NV12 has one chroma byte pair per 2 x 2 pixels: a frame is 3 / 2 bytes per pixel.
constexpr std::size_t nv12Numerator{3};
constexpr std::size_t nv12Denominator{2};
constexpr int evenMask{1};
// The RGA wants the width stride of an NV12 image to be a multiple of 16.
constexpr int strideAlignment{16};

// 0xAARRGGBB with all colour channels equal.
constexpr int grayColor(int level) noexcept {
    return (opaqueAlpha << (3 * channelBits)) | (level << (2 * channelBits)) |
           (level << channelBits) | level;
}
} // namespace

std::optional<RgaImage> RgaImage::import(const DmaBuf& buffer, const Logger& log) {
    const rga_buffer_handle_t handle{importbuffer_fd(buffer.fd(), static_cast<int>(buffer.size()))};
    if (handle == 0) {
        log.error("importbuffer_fd rejected fd=", buffer.fd(), " size=", buffer.size());
        return std::nullopt;
    }
    return std::optional<RgaImage>{RgaImage{handle, log}};
}

RgaImage::RgaImage(RgaImage&& other) noexcept
    : imported{std::exchange(other.imported, 0)}, log{other.log} {}

RgaImage& RgaImage::operator=(RgaImage&& other) noexcept {
    if (this != &other) {
        release();
        imported = std::exchange(other.imported, 0);
        log = other.log;
    }
    return *this;
}

RgaImage::~RgaImage() {
    release();
}

void RgaImage::release() noexcept {
    if (imported == 0) {
        return;
    }
    const IM_STATUS status{releasebuffer_handle(std::exchange(imported, 0))};
    if (status != IM_STATUS_SUCCESS) {
        log.error("releasebuffer_handle: ", imStrError_t(status));
    }
}

std::optional<RgaLetterbox> RgaLetterbox::create(const LetterboxConfig& config, const Logger& log) {
    if (config.sourceStride < config.source.width) {
        log.error("stride ", config.sourceStride, " is shorter than the width ",
                  config.source.width);
        return std::nullopt;
    }
    const std::optional<LetterboxLayout> layout{
        letterboxLayout(rotatedSize(config.source, config.rotation), config.targetSize)};
    if (!layout) {
        log.error("bad letterbox size: source ", config.source.width, "x", config.source.height,
                  " target ", config.targetSize);
        return std::nullopt;
    }
    log.info("letterbox ", config.source.width, "x", config.source.height, " -> ",
             layout->scaled.width, "x", layout->scaled.height, " at left=", layout->left,
             " top=", layout->top, " in ", config.targetSize, "x", config.targetSize);
    std::optional<DmaBuf> staging;
    std::optional<RgaImage> stagingImage;
    if (config.fullRange) {
        const std::size_t bytes{static_cast<std::size_t>(config.source.width) *
                                static_cast<std::size_t>(config.source.height) * rgbChannels};
        staging = DmaBuf::allocate(DmaBufConfig{defaultHeap, bytes}, log);
        if (!staging) {
            return std::nullopt;
        }
        stagingImage = RgaImage::import(*staging, log);
        if (!stagingImage) {
            return std::nullopt;
        }
    }
    return std::optional<RgaLetterbox>{
        RgaLetterbox{config, *layout, log, std::move(staging), std::move(stagingImage)}};
}

bool RgaLetterbox::fillPadding(const RgaImage& target) const noexcept {
    const rga_buffer_t image{wrapbuffer_handle(target.handle(), settings.targetSize,
                                               settings.targetSize, RK_FORMAT_RGB_888)};
    const im_rect whole{0, 0, settings.targetSize, settings.targetSize};
    const IM_STATUS status{imfill_t(image, whole, grayColor(paddingGray), 1)};
    if (status != IM_STATUS_SUCCESS) {
        log.error("imfill: ", imStrError_t(status));
        return false;
    }
    return true;
}

bool RgaLetterbox::convertFullRange(const RgaImage& source, const RgaImage& staged) const noexcept {
    const rga_buffer_t input{wrapbuffer_handle(source.handle(), settings.source.width,
                                               settings.source.height, RK_FORMAT_YCbCr_420_SP,
                                               settings.sourceStride, settings.source.height)};
    const rga_buffer_t output{wrapbuffer_handle(staged.handle(), settings.source.width,
                                                settings.source.height, RK_FORMAT_RGB_888)};
    // Measured on the board: only imcvtcolor honours the colour space, improcess always decodes
    // YUV as BT.601 limited range.
    const IM_STATUS status{imcvtcolor(input, output, RK_FORMAT_YCbCr_420_SP, RK_FORMAT_RGB_888,
                                      IM_YUV_TO_RGB_BT601_FULL, 1)};
    if (status != IM_STATUS_SUCCESS) {
        log.error("imcvtcolor: ", imStrError_t(status));
        return false;
    }
    return true;
}

bool RgaLetterbox::run(const RgaImage& source, const RgaImage& target) const noexcept {
    // A limited range source is converted by the improcess call below; a full range one is
    // converted into the staging buffer first and read from there.
    rga_buffer_t input{};
    if (stagingImage) {
        if (!convertFullRange(source, *stagingImage)) {
            return false;
        }
        input = wrapbuffer_handle(stagingImage->handle(), settings.source.width,
                                  settings.source.height, RK_FORMAT_RGB_888);
    } else {
        input = wrapbuffer_handle(source.handle(), settings.source.width, settings.source.height,
                                  RK_FORMAT_YCbCr_420_SP, settings.sourceStride,
                                  settings.source.height);
    }
    const rga_buffer_t output{wrapbuffer_handle(target.handle(), settings.targetSize,
                                                settings.targetSize, RK_FORMAT_RGB_888)};
    const rga_buffer_t noPattern{};
    const im_rect sourceRect{0, 0, settings.source.width, settings.source.height};
    const im_rect targetRect{placement.left, placement.top, placement.scaled.width,
                             placement.scaled.height};
    const im_rect noRect{};
    const IM_STATUS status{improcess(input, output, noPattern, sourceRect, targetRect, noRect,
                                     rotationFlag(settings.rotation) | IM_SYNC)};
    if (status != IM_STATUS_SUCCESS) {
        log.error("improcess: ", imStrError_t(status));
        return false;
    }
    return true;
}

std::optional<RgaSnapshot> RgaSnapshot::create(const SnapshotConfig& config, const Logger& log) {
    const ImageSize& size{config.source};
    if (size.width <= 0 || size.height <= 0 || config.sourceStride < size.width ||
        ((size.width | size.height) & evenMask) != 0) {
        log.error("bad snapshot source: ", size.width, "x", size.height, " stride ",
                  config.sourceStride);
        return std::nullopt;
    }
    const ImageSize rotated{rotatedSize(size, config.rotation)};
    log.info("snapshot ", size.width, "x", size.height, " -> ", rotated.width, "x", rotated.height);
    return std::optional<RgaSnapshot>{RgaSnapshot{config, rotated, log}};
}

int RgaSnapshot::stride() const noexcept {
    return (copySize.width + strideAlignment - 1) / strideAlignment * strideAlignment;
}

std::size_t RgaSnapshot::bytes() const noexcept {
    return static_cast<std::size_t>(stride()) * static_cast<std::size_t>(copySize.height) *
           nv12Numerator / nv12Denominator;
}

namespace {
// The improcess call of a snapshot, synchronous when `fence` is null, else started and the release
// fence stored there.
IM_STATUS copyFrame(const SnapshotConfig& settings, ImageSize copySize, int copyStride,
                    const RgaImage& source, const RgaImage& target, int* fence) noexcept {
    const rga_buffer_t input{wrapbuffer_handle(source.handle(), settings.source.width,
                                               settings.source.height, RK_FORMAT_YCbCr_420_SP,
                                               settings.sourceStride, settings.source.height)};
    const rga_buffer_t output{wrapbuffer_handle(target.handle(), copySize.width, copySize.height,
                                                RK_FORMAT_YCbCr_420_SP, copyStride,
                                                copySize.height)};
    const rga_buffer_t noPattern{};
    const im_rect noRect{};
    const int mode{rotationFlag(settings.rotation) | (fence == nullptr ? IM_SYNC : IM_ASYNC)};
    return improcess(input, output, noPattern, noRect, noRect, noRect, noFence, fence, nullptr,
                     mode);
}
} // namespace

std::optional<int> RgaSnapshot::start(const RgaImage& source,
                                      const RgaImage& target) const noexcept {
    int fence{noFence};
    const IM_STATUS status{copyFrame(settings, copySize, stride(), source, target, &fence)};
    if (status != IM_STATUS_SUCCESS || fence < 0) {
        log.error("improcess (async): ", imStrError_t(status), " fence ", fence);
        return std::nullopt;
    }
    return fence;
}

bool RgaSnapshot::finish(int fence) const noexcept {
    const IM_STATUS status{imsync(fence)};
    if (status != IM_STATUS_SUCCESS) {
        log.error("imsync: ", imStrError_t(status));
        return false;
    }
    return true;
}

bool RgaSnapshot::run(const RgaImage& source, const RgaImage& target) const noexcept {
    const rga_buffer_t input{wrapbuffer_handle(source.handle(), settings.source.width,
                                               settings.source.height, RK_FORMAT_YCbCr_420_SP,
                                               settings.sourceStride, settings.source.height)};
    const rga_buffer_t output{wrapbuffer_handle(target.handle(), copySize.width, copySize.height,
                                                RK_FORMAT_YCbCr_420_SP, stride(), copySize.height)};
    const rga_buffer_t noPattern{};
    const im_rect noRect{};
    const IM_STATUS status{improcess(input, output, noPattern, noRect, noRect, noRect,
                                     rotationFlag(settings.rotation) | IM_SYNC)};
    if (status != IM_STATUS_SUCCESS) {
        log.error("improcess: ", imStrError_t(status));
        return false;
    }
    return true;
}
