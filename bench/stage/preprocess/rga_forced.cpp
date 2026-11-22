#include "rga_forced.h"

#include <cstddef>
#include <utility>

#include "im2d.hpp"

namespace {
constexpr int noFence{-1};
constexpr std::size_t rgbChannels{3};

// The scheduler mask of one core.
int coreMask(RgaCore core) noexcept {
    switch (core) {
    case RgaCore::Rga2:
        return IM_SCHEDULER_RGA2_CORE0;
    case RgaCore::Rga30:
        return IM_SCHEDULER_RGA3_CORE0;
    case RgaCore::Rga31:
        break;
    }
    return IM_SCHEDULER_RGA3_CORE1;
}
} // namespace

std::optional<RgaForcedLetterbox> RgaForcedLetterbox::create(const LetterboxConfig& config,
                                                             RgaCore core, const Logger& log) {
    if (config.rotation != Rotation::Rotate180) {
        log.error("the forced-core job covers 180 degrees only");
        return std::nullopt;
    }
    LetterboxConfig layoutConfig{config};
    layoutConfig.fullRange = false; // the base gives the layout and the padding, no staging buffer
    std::optional<RgaLetterbox> base{RgaLetterbox::create(layoutConfig, log)};
    if (!base) {
        return std::nullopt;
    }
    std::optional<DmaBuf> staging;
    std::optional<RgaImage> stagingImage;
    if (config.fullRange) {
        const std::size_t bytes{static_cast<std::size_t>(config.source.width) *
                                static_cast<std::size_t>(config.source.height) * rgbChannels};
        staging = DmaBuf::allocate(DmaBufConfig{lowHeap, bytes}, log);
        if (!staging) {
            return std::nullopt;
        }
        stagingImage = RgaImage::import(*staging, log);
        if (!stagingImage) {
            return std::nullopt;
        }
    }
    return std::optional<RgaForcedLetterbox>{RgaForcedLetterbox{
        std::move(*base), config, core, log, std::move(staging), std::move(stagingImage)}};
}

bool RgaForcedLetterbox::convertFullRange(const RgaImage& source, const RgaImage& staged,
                                          int mask) const noexcept {
    rga_buffer_t input{wrapbuffer_handle(source.handle(), settings.source.width,
                                         settings.source.height, RK_FORMAT_YCbCr_420_SP,
                                         settings.sourceStride, settings.source.height)};
    rga_buffer_t output{wrapbuffer_handle(staged.handle(), settings.source.width,
                                          settings.source.height, RK_FORMAT_RGB_888)};
    // imcvtcolor sets the colour space on the two buffers, but cannot take a core: the same call
    // is made here with improcess, which takes the options.
    input.color_space_mode = IM_YUV_TO_RGB_BT601_FULL;
    output.color_space_mode = IM_YUV_TO_RGB_BT601_FULL;
    const rga_buffer_t noPattern{};
    const im_rect whole{0, 0, settings.source.width, settings.source.height};
    const im_rect noRect{};
    im_opt_t options{};
    options.core = mask;
    const IM_STATUS status{improcess(input, output, noPattern, whole, whole, noRect, noFence,
                                     nullptr, &options, IM_SYNC)};
    if (status != IM_STATUS_SUCCESS) {
        log.error(rgaCoreName(core), " colour conversion: ", imStrError_t(status));
        return false;
    }
    return true;
}

bool RgaForcedLetterbox::run(const RgaImage& source, const RgaImage& target) const noexcept {
    const LetterboxLayout& layout{base.layout()};
    im_opt_t options{};
    options.core = coreMask(core);
    rga_buffer_t input{};
    if (stagingImage) {
        if (!convertFullRange(source, *stagingImage, options.core)) {
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
    const im_rect targetRect{layout.left, layout.top, layout.scaled.width, layout.scaled.height};
    const im_rect noRect{};
    const IM_STATUS status{improcess(input, output, noPattern, sourceRect, targetRect, noRect,
                                     noFence, nullptr, &options,
                                     IM_HAL_TRANSFORM_ROT_180 | IM_SYNC)};
    if (status != IM_STATUS_SUCCESS) {
        log.error(rgaCoreName(core), " improcess: ", imStrError_t(status));
        return false;
    }
    return true;
}

std::string_view rgaCoreName(RgaCore core) noexcept {
    switch (core) {
    case RgaCore::Rga2:
        return "RGA2";
    case RgaCore::Rga30:
        return "RGA3_0";
    case RgaCore::Rga31:
        break;
    }
    return "RGA3_1";
}
