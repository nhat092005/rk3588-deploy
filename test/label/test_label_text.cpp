// The label text: sizes, the text of a label, the contrast of the text colour, the bitmap that
// renderLabel draws (checked against the font data) and the placement. Plain C++, runs on the PC.
// Exit code 0 when every check passes.
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "label_font.h"
#include "label_text.h"
#include "logger.h"

namespace {
constexpr std::uint32_t helmetColor{0x00C800U};
constexpr std::uint32_t noHelmetColor{0x2828FFU};
constexpr std::uint32_t noVestColor{0x008CFFU};
constexpr std::uint32_t vestColor{0xAABE00U};
constexpr std::uint32_t whiteColor{0xFFFFFFU};
constexpr std::uint32_t yellowColor{0x00E6FFU};
constexpr std::uint32_t blackColor{0x000000U};
constexpr std::uint32_t redBackground{0x0000FFU}; // 0xBBGGRR: red
constexpr std::uint8_t sentinel{0xAB};
constexpr std::uint8_t fullByte{0xFF};
constexpr std::uint8_t zeroByte{0x00};
constexpr unsigned byteMask{0xFFU};
constexpr int imageWidth{3840};
constexpr int imageHeight{2160};

class Check {
public:
    explicit Check(const Logger& logger) : log{logger} {}

    void expect(bool condition, const char* what) {
        if (!condition) {
            log.error("FAIL: ", what);
            passed = false;
        }
    }

    bool ok() const {
        return passed;
    }

private:
    Logger log;
    bool passed{true};
};

std::string textOf(const LabelInfo& info) {
    std::array<char, maxLabelChars + 1> buffer{};
    const std::size_t length{formatLabel(info, buffer.data(), buffer.size())};
    return std::string{buffer.data(), length};
}

struct SizeCase {
    std::size_t chars;
    int width;
    int height;
};

// width = chars x 8 x 3 + 12, height = 16 x 3 + 12, at most 24 characters.
constexpr std::array<SizeCase, 4> sizeCases{{
    {0, 12, 60},
    {12, 300, 60},
    {maxLabelChars, 588, 60},
    {maxLabelChars + 10, 588, 60},
}};

struct TextCase {
    LabelInfo info{};
    const char* expected{nullptr};
    const char* what{nullptr};
};

constexpr std::array<TextCase, 8> textCases{{
    {{"NoHelmet", -1, 0.87F, false}, "NoHelmet 87%", "class and percent"},
    {{"Person", 12, 0.95F, false}, "#12 Person 95%", "tracked object"},
    {{"Person", 13, 0.95F, true}, "! #13 Person 95%", "violator"},
    {{"Vest", -1, 1.0F, false}, "Vest 100%", "full score"},
    {{"Vest", -1, 1.7F, false}, "Vest 100%", "a score above 1 is cut"},
    {{"Vest", -1, -0.2F, false}, "Vest 0%", "a score below 0 is cut"},
    {{"Helmet", -1, 0.005F, false}, "Helmet 1%", "the percent is rounded"},
    {{"Person", 7, 0.5F, true}, "! #7 Person 50%", "a short track number"},
}};

void checkSizeAndText(Check& check) {
    for (const SizeCase& item : sizeCases) {
        const LabelSize size{labelSize(item.chars)};
        check.expect(size.width == item.width && size.height == item.height,
                     "the size of a label of this many characters");
    }
    for (const TextCase& item : textCases) {
        check.expect(textOf(item.info) == item.expected, item.what);
    }
    constexpr LabelInfo longInfo{"AVeryLongClassNameThatDoesNotFit", 123456, 0.5F, true};
    check.expect(textOf(longInfo).size() == maxLabelChars, "a long text is cut");
    std::array<char, maxLabelChars> small{};
    check.expect(formatLabel(longInfo, small.data(), small.size()) == 0,
                 "a buffer that is too small gives nothing");
}

void checkContrast(Check& check) {
    check.expect(textColorFor(helmetColor) == whiteColor, "white on green");
    check.expect(textColorFor(noHelmetColor) == whiteColor, "white on red");
    check.expect(textColorFor(noVestColor) == blackColor, "black on orange");
    check.expect(textColorFor(vestColor) == blackColor, "black on teal");
    check.expect(textColorFor(whiteColor) == blackColor, "black on white");
    check.expect(textColorFor(yellowColor) == blackColor, "black on yellow");
}

// Number of set bits of a glyph.
int setBits(char value) {
    const auto index{static_cast<std::size_t>(value - fontFirstChar)};
    int count{0};
    for (int row{0}; row < glyphHeight; ++row) {
        for (unsigned bit{0}; bit < static_cast<unsigned>(glyphWidth); ++bit) {
            const unsigned rowBits{
                labelFont.at((index * glyphHeight) + static_cast<std::size_t>(row))};
            count += static_cast<int>((rowBits >> bit) & 1U);
        }
    }
    return count;
}

struct Bitmap {
    LabelSize size;
    std::size_t stride{0};
    std::vector<std::uint8_t> bytes;
};

Bitmap render(std::string_view text, const LabelColors& colors, std::size_t extraStride = 0) {
    Bitmap bitmap;
    bitmap.size = labelSize(text.size());
    bitmap.stride = (static_cast<std::size_t>(bitmap.size.width) * labelPixelBytes) + extraStride;
    bitmap.bytes.assign(bitmap.stride * static_cast<std::size_t>(bitmap.size.height), sentinel);
    renderLabel(text, colors, bitmap.bytes.data(), bitmap.stride);
    return bitmap;
}

const std::uint8_t* pixelAt(const Bitmap& bitmap, int column, int row) {
    return bitmap.bytes.data() + (static_cast<std::size_t>(row) * bitmap.stride) +
           (static_cast<std::size_t>(column) * labelPixelBytes);
}

int foregroundPixels(const Bitmap& bitmap, std::uint32_t background) {
    int count{0};
    const std::array<std::uint8_t, 3> back{
        static_cast<std::uint8_t>(background & byteMask),
        static_cast<std::uint8_t>((background >> 8U) & byteMask),
        static_cast<std::uint8_t>((background >> 16U) & byteMask)};
    for (int row{0}; row < bitmap.size.height; ++row) {
        for (int column{0}; column < bitmap.size.width; ++column) {
            const std::uint8_t* pixel{pixelAt(bitmap, column, row)};
            count += (pixel[0] != back[0] || pixel[1] != back[1] || pixel[2] != back[2]) ? 1 : 0;
        }
    }
    return count;
}

void checkRender(Check& check) {
    const LabelColors colors{redBackground, whiteColor};
    const Bitmap letter{render("A", colors)};
    check.expect(letter.size.width == labelSize(1).width &&
                     letter.size.height == labelSize(1).height,
                 "the bitmap has the label size");
    const std::uint8_t* corner{pixelAt(letter, 0, 0)};
    check.expect(corner[0] == fullByte && corner[1] == zeroByte && corner[2] == zeroByte &&
                     corner[3] == fullByte,
                 "the background is R, G, B, A");
    check.expect(foregroundPixels(letter, redBackground) == setBits('A') * labelScale * labelScale,
                 "a character has its glyph bits scaled 3 x 3");
    check.expect(foregroundPixels(render("   ", colors), redBackground) == 0,
                 "spaces have no pixels");
    // The first set bit of the glyph is drawn as a 3 x 3 block of the text colour.
    bool blockChecked{false};
    for (int row{0}; row < glyphHeight && !blockChecked; ++row) {
        for (int bit{0}; bit < glyphWidth && !blockChecked; ++bit) {
            const auto index{static_cast<std::size_t>('A' - fontFirstChar)};
            const unsigned mask{1U << static_cast<unsigned>(glyphWidth - 1 - bit)};
            if ((labelFont.at((index * glyphHeight) + static_cast<std::size_t>(row)) & mask) == 0) {
                continue;
            }
            const int column{labelPadding + (bit * labelScale)};
            const int line{labelPadding + (row * labelScale)};
            check.expect(pixelAt(letter, column, line)[0] == fullByte &&
                             pixelAt(letter, column, line)[1] == fullByte &&
                             pixelAt(letter, column + 2, line + 2)[2] == fullByte,
                         "a set bit is a 3 x 3 block of the text colour");
            check.expect(pixelAt(letter, column - 1, line)[1] == zeroByte,
                         "the pixel left of the block is the background");
            blockChecked = true;
        }
    }
    check.expect(blockChecked, "the glyph has a set bit");
    // Other characters: the same bitmap as '?', and the second character is one cell to the right.
    check.expect(render("\x01", colors).bytes == render("?", colors).bytes,
                 "an unknown character is '?'");
    const Bitmap pair{render("AA", colors)};
    check.expect(foregroundPixels(pair, redBackground) ==
                     2 * foregroundPixels(letter, redBackground),
                 "two characters have twice the pixels");
    // A stride wider than the label: the extra bytes are left alone.
    const Bitmap wide{render("A", colors, 16)};
    bool untouched{true};
    for (int row{0}; row < wide.size.height; ++row) {
        const std::size_t end{wide.stride * static_cast<std::size_t>(row) + wide.stride - 1};
        untouched = untouched && wide.bytes.at(end) == sentinel;
    }
    check.expect(untouched, "bytes beyond the width are not written");
}

struct PlaceCase {
    PixelBox box{};
    bool below{false};
    LabelPosition expected{};
    const char* what{nullptr};
};

// For a label of 12 characters (300 x 60) in a 3840 x 2160 image.
constexpr std::array<PlaceCase, 7> placeCases{{
    {{100.0F, 300.0F, 400.0F, 700.0F}, false, {100, 240}, "above the box at its left edge"},
    {{100.0F, 300.0F, 400.0F, 700.0F}, true, {100, 700}, "under the box at its left edge"},
    {{101.0F, 301.0F, 400.0F, 700.0F}, false, {100, 240}, "on a multiple of 4"},
    {{100.0F, 20.0F, 400.0F, 700.0F}, false, {100, 0}, "kept inside at the top"},
    {{100.0F, 300.0F, 400.0F, 2150.0F}, true, {100, 2100}, "kept inside at the bottom"},
    {{3800.0F, 300.0F, 3830.0F, 700.0F}, false, {3540, 240}, "kept inside at the right"},
    {{-40.0F, 300.0F, 100.0F, 700.0F}, false, {0, 240}, "kept inside at the left"},
}};

void checkPlacement(Check& check) {
    const LabelSize size{labelSize(12)};
    const ImageSize image{imageWidth, imageHeight};
    for (const PlaceCase& item : placeCases) {
        const LabelPosition got{placeLabel(item.box, item.below, size, image)};
        check.expect(got.x == item.expected.x && got.y == item.expected.y, item.what);
    }
}
} // namespace

int main() {
    const Logger log{"TEST_LABEL", LogLevel::Debug};
    Check check{log};
    checkSizeAndText(check);
    checkContrast(check);
    checkRender(check);
    checkPlacement(check);
    if (check.ok()) {
        log.info("PASS");
        return EXIT_SUCCESS;
    }
    log.error("FAIL");
    return EXIT_FAILURE;
}
