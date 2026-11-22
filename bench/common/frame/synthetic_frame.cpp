#include "synthetic_frame.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace {
constexpr int lumaMin{0};
constexpr int lumaRange{255};
constexpr int chromaMin{0};
constexpr int chromaRange{255};
constexpr int chromaMid{128};
constexpr int barCount{7};
constexpr int barLevel{191}; // 75% of 255
constexpr std::uint64_t noiseSeed{42};
constexpr int byteLimit{256};

struct Rgb {
    int red{0};
    int green{0};
    int blue{0};
};

struct Yuv {
    int luma{0};
    int chromaBlue{0};
    int chromaRed{0};
};

constexpr std::array<Rgb, barCount> bars{{{barLevel, barLevel, barLevel},
                                          {barLevel, barLevel, 0},
                                          {0, barLevel, barLevel},
                                          {0, barLevel, 0},
                                          {barLevel, 0, barLevel},
                                          {barLevel, 0, 0},
                                          {0, 0, barLevel}}};

// ITU-R BT.601, 8 bits, full range (what the camera reports): the matrix that the colour conversion
// of the pipeline inverts.
Yuv toYuv(const Rgb& rgb) noexcept {
    const double red{static_cast<double>(rgb.red)};
    const double green{static_cast<double>(rgb.green)};
    const double blue{static_cast<double>(rgb.blue)};
    const double luma{(0.299 * red) + (0.587 * green) + (0.114 * blue)};
    const double chromaBlue{chromaMid + (-0.168736 * red) - (0.331264 * green) + (0.5 * blue)};
    const double chromaRed{chromaMid + (0.5 * red) - (0.418688 * green) - (0.081312 * blue)};
    return {static_cast<int>(std::lround(luma)), static_cast<int>(std::lround(chromaBlue)),
            static_cast<int>(std::lround(chromaRed))};
}

// Ramps: luma grows along the diagonal, Cb along the width, Cr along the height.
void fillGradient(cv::Mat& frame, ImageSize size) {
    const int diagonal{size.width + size.height - 2};
    for (int row{0}; row < size.height; ++row) {
        for (int col{0}; col < size.width; ++col) {
            frame.at<std::uint8_t>(row, col) =
                static_cast<std::uint8_t>(lumaMin + ((lumaRange * (col + row)) / diagonal));
        }
    }
    const int chromaCols{size.width / 2};
    const int chromaRows{size.height / 2};
    for (int row{0}; row < chromaRows; ++row) {
        for (int col{0}; col < chromaCols; ++col) {
            const int chromaBlue{chromaMin + ((chromaRange * col) / std::max(1, chromaCols - 1))};
            const int chromaRed{chromaMin + ((chromaRange * row) / std::max(1, chromaRows - 1))};
            frame.at<std::uint8_t>(size.height + row, 2 * col) =
                static_cast<std::uint8_t>(chromaBlue);
            frame.at<std::uint8_t>(size.height + row, (2 * col) + 1) =
                static_cast<std::uint8_t>(chromaRed);
        }
    }
}

// The bar that covers a luma column.
int barOf(int col, int width) noexcept {
    return (col * barCount) / width;
}

void fillBars(cv::Mat& frame, ImageSize size) {
    std::array<Yuv, barCount> colours{};
    for (std::size_t index{0}; index < bars.size(); ++index) {
        colours.at(index) = toYuv(bars.at(index));
    }
    for (int row{0}; row < size.height; ++row) {
        for (int col{0}; col < size.width; ++col) {
            const Yuv& colour{colours.at(static_cast<std::size_t>(barOf(col, size.width)))};
            frame.at<std::uint8_t>(row, col) = static_cast<std::uint8_t>(colour.luma);
        }
    }
    for (int row{0}; row < size.height / 2; ++row) {
        for (int col{0}; col < size.width / 2; ++col) {
            const Yuv& colour{colours.at(static_cast<std::size_t>(barOf(2 * col, size.width)))};
            frame.at<std::uint8_t>(size.height + row, 2 * col) =
                static_cast<std::uint8_t>(colour.chromaBlue);
            frame.at<std::uint8_t>(size.height + row, (2 * col) + 1) =
                static_cast<std::uint8_t>(colour.chromaRed);
        }
    }
}
} // namespace

std::optional<Pattern> parsePattern(std::string_view text) noexcept {
    for (const Pattern pattern : {Pattern::Gradient, Pattern::ColorBars, Pattern::Noise}) {
        if (text == patternName(pattern)) {
            return pattern;
        }
    }
    return std::nullopt;
}

std::string_view patternName(Pattern pattern) noexcept {
    switch (pattern) {
    case Pattern::Gradient:
        return "gradient";
    case Pattern::ColorBars:
        return "bars";
    case Pattern::Noise:
        break;
    }
    return "noise";
}

void fillNv12(Pattern pattern, cv::Mat& frame, ImageSize size) {
    switch (pattern) {
    case Pattern::Gradient:
        fillGradient(frame, size);
        return;
    case Pattern::ColorBars:
        fillBars(frame, size);
        return;
    case Pattern::Noise:
        break;
    }
    cv::RNG generator{noiseSeed}; // fill() changes the state
    generator.fill(frame, cv::RNG::UNIFORM, 0, byteLimit);
}
