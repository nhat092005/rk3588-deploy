#ifndef LETTERBOX_LAYOUT_H
#define LETTERBOX_LAYOUT_H

#include <optional>

/** @brief A size in pixels. */
struct ImageSize {
    int width{0};
    int height{0};
};

/** @brief Where a letterboxed image sits inside the square model input. */
struct LetterboxLayout {
    /** Size of the image after scaling, not larger than the target on either side. */
    ImageSize scaled{};
    /** Padding to the left of the image, in pixels. The right padding is the rest. */
    int left{0};
    /** Padding above the image, in pixels. The bottom padding is the rest. */
    int top{0};
    /** Scale factor from the source to the model input, the `gain` of rk3588_common.model_io. */
    double gain{1.0};
    /** Size of the image that was letterboxed (after any rotation), `w0` and `h0` of
     *  rk3588_common.model_io. */
    ImageSize source{};
};

/**
 * @brief The letterbox of a source image into a square target, the same numbers as
 *        rk3588_common.model_io.preprocess (Ultralytics val LetterBox, centred).
 * @param source Width and height of the image, both above 0.
 * @param targetSize Side of the square model input in pixels, above 0.
 * @return The layout, or empty when an argument is not above 0.
 *
 * The image is scaled by `targetSize / max(width, height)`, with the scaled sides rounded up, and
 * not scaled when that factor is 1. The padding is split evenly, and an odd rest goes to the
 * right and the bottom.
 */
std::optional<LetterboxLayout> letterboxLayout(ImageSize source, int targetSize) noexcept;

#endif // LETTERBOX_LAYOUT_H
