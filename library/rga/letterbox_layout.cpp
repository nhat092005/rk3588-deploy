#include "letterbox_layout.h"

#include <algorithm>
#include <cmath>

namespace {
// The side of the source after the scale `ratio`, at most the target.
int scaledSide(int side, double ratio, int targetSize) noexcept {
    return std::min(static_cast<int>(std::ceil(static_cast<double>(side) * ratio)), targetSize);
}
} // namespace

std::optional<LetterboxLayout> letterboxLayout(ImageSize source, int targetSize) noexcept {
    if (source.width <= 0 || source.height <= 0 || targetSize <= 0) {
        return std::nullopt;
    }
    const double ratio{static_cast<double>(targetSize) /
                       static_cast<double>(std::max(source.width, source.height))};
    LetterboxLayout layout{};
    layout.source = source;
    layout.scaled = source;
    if (ratio != 1.0) {
        layout.scaled.width = scaledSide(source.width, ratio, targetSize);
        layout.scaled.height = scaledSide(source.height, ratio, targetSize);
    }
    layout.left = (targetSize - layout.scaled.width) / 2;
    layout.top = (targetSize - layout.scaled.height) / 2;
    layout.gain = std::min(static_cast<double>(layout.scaled.height) / source.height,
                           static_cast<double>(layout.scaled.width) / source.width);
    return layout;
}
