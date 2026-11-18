#include "box_mapper.h"

#include <algorithm>

namespace {
// `value` limited to [0, limit].
double clip(double value, double limit) noexcept {
    return std::min(std::max(value, 0.0), limit);
}
} // namespace

std::optional<BoxMapper> BoxMapper::create(const LetterboxLayout& layout,
                                           ImageSize destination) noexcept {
    if (layout.source.width <= 0 || layout.source.height <= 0 || layout.gain <= 0.0 ||
        destination.width <= 0 || destination.height <= 0) {
        return std::nullopt;
    }
    return BoxMapper{layout, destination};
}

PixelBox BoxMapper::toSource(const PixelBox& model) const noexcept {
    const double width{static_cast<double>(layout.source.width)};
    const double height{static_cast<double>(layout.source.height)};
    const auto horizontal{[&](float value) {
        return clip((static_cast<double>(value) - layout.left) / layout.gain, width);
    }};
    const auto vertical{[&](float value) {
        return clip((static_cast<double>(value) - layout.top) / layout.gain, height);
    }};
    return PixelBox{
        static_cast<float>(horizontal(model.left)), static_cast<float>(vertical(model.top)),
        static_cast<float>(horizontal(model.right)), static_cast<float>(vertical(model.bottom))};
}

PixelBox BoxMapper::toDestination(const PixelBox& model) const noexcept {
    const PixelBox source{toSource(model)};
    const double scaleX{static_cast<double>(destination.width) /
                        static_cast<double>(layout.source.width)};
    const double scaleY{static_cast<double>(destination.height) /
                        static_cast<double>(layout.source.height)};
    return PixelBox{static_cast<float>(static_cast<double>(source.left) * scaleX),
                    static_cast<float>(static_cast<double>(source.top) * scaleY),
                    static_cast<float>(static_cast<double>(source.right) * scaleX),
                    static_cast<float>(static_cast<double>(source.bottom) * scaleY)};
}
