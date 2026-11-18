#include "label_text.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

#include "label_font.h"

namespace {
constexpr int percentMax{100};
constexpr float percentScale{100.0F};
constexpr std::uint8_t opaque{0xFF};
constexpr unsigned byteMask{0xFFU};
constexpr unsigned byteBits{8U};
constexpr unsigned greenShift{8U};
constexpr unsigned blueShift{16U};
constexpr double lumaRed{0.299};
constexpr double lumaGreen{0.587};
constexpr double lumaBlue{0.114};
// A background brighter than this gets black text.
constexpr double lightLuma{128.0};
constexpr char unknownChar{'?'};
constexpr std::uint32_t blackBgr{0x000000U};
constexpr std::uint32_t whiteBgr{0xFFFFFFU};

struct Rgb {
    std::uint8_t red;
    std::uint8_t green;
    std::uint8_t blue;
};

// 0xBBGGRR as red, green, blue.
Rgb unpack(std::uint32_t bgr) noexcept {
    return Rgb{static_cast<std::uint8_t>(bgr & byteMask),
               static_cast<std::uint8_t>((bgr >> greenShift) & byteMask),
               static_cast<std::uint8_t>((bgr >> blueShift) & byteMask)};
}

void putPixel(std::uint8_t* row, int column, const Rgb& color) noexcept {
    std::uint8_t* pixel{row + (static_cast<std::size_t>(column) * labelPixelBytes)};
    pixel[0] = color.red;
    pixel[1] = color.green;
    pixel[2] = color.blue;
    pixel[3] = opaque;
}

// The row bytes of a character, or of '?' when the font has none.
const std::uint8_t* glyphOf(char value) noexcept {
    int code{static_cast<unsigned char>(value)};
    if (code < fontFirstChar || code > fontLastChar) {
        code = unknownChar;
    }
    return labelFont.data() + (static_cast<std::size_t>(code - fontFirstChar) * glyphHeight);
}

int clampInt(int value, int low, int high) noexcept {
    return std::min(std::max(value, low), std::max(low, high));
}

int alignedDown(int value) noexcept {
    return value / labelAlignment * labelAlignment;
}
} // namespace

LabelSize labelSize(std::size_t chars) noexcept {
    const int count{static_cast<int>(std::min(chars, maxLabelChars))};
    return LabelSize{(count * glyphWidth * labelScale) + (2 * labelPadding),
                     (glyphHeight * labelScale) + (2 * labelPadding)};
}

std::size_t formatLabel(const LabelInfo& info, char* out, std::size_t capacity) noexcept {
    if (capacity < maxLabelChars + 1) {
        return 0;
    }
    const int percent{
        clampInt(static_cast<int>(std::lround(info.score * percentScale)), 0, percentMax)};
    const std::string_view mark{info.violator ? "! " : ""};
    std::array<char, maxLabelChars * 2> text{};
    int length{0};
    if (info.trackId >= 0) {
        length = std::snprintf(text.data(), text.size(), "%.*s#%d %.*s %d%%",
                               static_cast<int>(mark.size()), mark.data(), info.trackId,
                               static_cast<int>(info.name.size()), info.name.data(), percent);
    } else {
        length = std::snprintf(text.data(), text.size(), "%.*s%.*s %d%%",
                               static_cast<int>(mark.size()), mark.data(),
                               static_cast<int>(info.name.size()), info.name.data(), percent);
    }
    const std::size_t kept{std::min(static_cast<std::size_t>(std::max(length, 0)), maxLabelChars)};
    std::copy_n(text.data(), kept, out);
    out[kept] = '\0';
    return kept;
}

std::uint32_t textColorFor(std::uint32_t background) noexcept {
    const Rgb color{unpack(background)};
    const double luma{(lumaRed * color.red) + (lumaGreen * color.green) + (lumaBlue * color.blue)};
    return luma > lightLuma ? blackBgr : whiteBgr;
}

void renderLabel(std::string_view text, const LabelColors& colors, std::uint8_t* pixels,
                 std::size_t strideBytes) noexcept {
    const std::size_t chars{std::min(text.size(), maxLabelChars)};
    const LabelSize size{labelSize(chars)};
    const Rgb background{unpack(colors.background)};
    const Rgb foreground{unpack(colors.text)};
    for (int row{0}; row < size.height; ++row) {
        std::uint8_t* line{pixels + (static_cast<std::size_t>(row) * strideBytes)};
        for (int column{0}; column < size.width; ++column) {
            putPixel(line, column, background);
        }
    }
    for (std::size_t index{0}; index < chars; ++index) {
        const std::uint8_t* glyph{glyphOf(text[index])};
        const int originX{labelPadding + (static_cast<int>(index) * glyphWidth * labelScale)};
        for (int glyphRow{0}; glyphRow < glyphHeight; ++glyphRow) {
            for (int bit{0}; bit < glyphWidth; ++bit) {
                const unsigned mask{1U << static_cast<unsigned>(glyphWidth - 1 - bit)};
                if ((glyph[glyphRow] & mask) == 0) {
                    continue;
                }
                for (int dy{0}; dy < labelScale; ++dy) {
                    std::uint8_t* line{pixels + (static_cast<std::size_t>(
                                                     labelPadding + (glyphRow * labelScale) + dy) *
                                                 strideBytes)};
                    for (int dx{0}; dx < labelScale; ++dx) {
                        putPixel(line, originX + (bit * labelScale) + dx, foreground);
                    }
                }
            }
        }
    }
}

LabelPosition placeLabel(const PixelBox& box, bool below, const LabelSize& size,
                         const ImageSize& image) noexcept {
    const int wantedX{static_cast<int>(std::floor(static_cast<double>(box.left)))};
    const int wantedY{below ? static_cast<int>(std::ceil(static_cast<double>(box.bottom)))
                            : static_cast<int>(std::floor(static_cast<double>(box.top))) -
                                  size.height};
    return LabelPosition{alignedDown(clampInt(wantedX, 0, image.width - size.width)),
                         alignedDown(clampInt(wantedY, 0, image.height - size.height))};
}
