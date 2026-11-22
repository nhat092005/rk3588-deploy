#ifndef BENCH_SYNTHETIC_FRAME_H
#define BENCH_SYNTHETIC_FRAME_H

#include <cstdint>
#include <optional>
#include <string_view>

#include <opencv2/core.hpp>

#include "letterbox_layout.h"

/**
 * @brief The picture of a synthetic frame. The time of every method here is data-independent, so
 *        the picture matters only for the pixel difference between methods.
 */
enum class Pattern : std::uint8_t {
    /** Smooth ramps: the lowest difference between interpolators, a lower bound. */
    Gradient,
    /** Seven full-height bars at 75% level (white, yellow, cyan, green, magenta, red, blue):
     *  sharp vertical edges, like the top band of the SMPTE bars. */
    ColorBars,
    /** Uniform random bytes: the highest difference between interpolators, an upper bound. */
    Noise
};

/**
 * @brief Reads the name of a pattern.
 * @param text One of "gradient", "bars" or "noise".
 * @return The pattern, or empty for any other text.
 */
std::optional<Pattern> parsePattern(std::string_view text) noexcept;

/**
 * @brief The name that parsePattern() reads.
 * @param pattern A pattern.
 * @return The name, never empty.
 */
std::string_view patternName(Pattern pattern) noexcept;

/**
 * @brief Draws a pattern into an NV12 frame: the luma plane, then the interleaved Cb Cr plane, in
 *        BT.601 limited range.
 * @param pattern What to draw. The same pattern and size give the same bytes.
 * @param frame View of the frame: `3 * size.height / 2` rows of `size.width` bytes, 8 bits, one
 *        channel. The step may be larger than the width.
 * @param size Size of the picture in pixels, both even and at least 4.
 */
void fillNv12(Pattern pattern, cv::Mat& frame, ImageSize size);

#endif // BENCH_SYNTHETIC_FRAME_H
