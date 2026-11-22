#include "rga_forced.h"

#include <utility>

#include "im2d.hpp"

namespace {
constexpr int noFence{-1};

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
    if (config.fullRange || config.rotation != Rotation::Rotate180) {
        log.error("the forced-core job covers a limited range source at 180 degrees only");
        return std::nullopt;
    }
    std::optional<RgaLetterbox> base{RgaLetterbox::create(config, log)};
    if (!base) {
        return std::nullopt;
    }
    return std::optional<RgaForcedLetterbox>{
        RgaForcedLetterbox{std::move(*base), config, core, log}};
}

bool RgaForcedLetterbox::run(const RgaImage& source, const RgaImage& target) const noexcept {
    const LetterboxLayout& layout{base.layout()};
    const rga_buffer_t input{wrapbuffer_handle(source.handle(), settings.source.width,
                                               settings.source.height, RK_FORMAT_YCbCr_420_SP,
                                               settings.sourceStride, settings.source.height)};
    const rga_buffer_t output{wrapbuffer_handle(target.handle(), settings.targetSize,
                                                settings.targetSize, RK_FORMAT_RGB_888)};
    const rga_buffer_t noPattern{};
    const im_rect sourceRect{0, 0, settings.source.width, settings.source.height};
    const im_rect targetRect{layout.left, layout.top, layout.scaled.width, layout.scaled.height};
    const im_rect noRect{};
    im_opt_t options{};
    options.core = coreMask(core);
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
