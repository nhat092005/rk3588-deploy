// RgaLetterbox on the board: colour order, rotation direction, padding, geometry, the BT.601 and
// BT.709 matrices and a source buffer taller than its image. The source is a
// synthetic 640x360 NV12 image, left half red and right half blue, so the expected RGB output is
// known. Needs /dev/dma_heap/system and the RGA. Exit code 0 when every check passes.
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>

#include "dmabuf.h"
#include "logger.h"
#include "rga.h"

namespace {
constexpr int frameWidth{640};
constexpr int frameHeight{360};
constexpr int modelSize{640};
constexpr std::size_t rgbChannels{3};
constexpr int colorTolerance{8};
constexpr std::uint8_t paddingValue{114};
// The targets come from lowHeap: fillPadding is a colour fill, which only the RGA2 does, and the
// RGA2 maps only memory below 4 GB (see lowHeap in dmabuf.h). A target of the "system" heap fails
// now and then, in the first imfill, with "no core match".
// BT.601 limited range of pure red and pure blue.
constexpr std::uint8_t redLuma{81};
constexpr std::uint8_t redU{90};
constexpr std::uint8_t redV{240};
constexpr std::uint8_t blueLuma{41};
constexpr std::uint8_t blueU{240};
constexpr std::uint8_t blueV{110};
// Probe points, far from the edge between the colours and from the borders of the image area.
constexpr int nearPoint{160};
constexpr int middlePoint{320};
constexpr int farPoint{480};
constexpr int cornerPoint{10};

struct Rgb {
    int red;
    int green;
    int blue;
};

constexpr Rgb redColor{255, 0, 0};
constexpr Rgb blueColor{0, 0, 255};

// Left half red, right half blue.
bool paintSource(DmaBuf& source) {
    auto* bytes{static_cast<std::uint8_t*>(source.data())};
    const std::size_t lumaSize{static_cast<std::size_t>(frameWidth) * frameHeight};
    for (int row{0}; row < frameHeight; ++row) {
        for (int column{0}; column < frameWidth; ++column) {
            const bool left{column < frameWidth / 2};
            bytes[static_cast<std::size_t>(row) * frameWidth + static_cast<std::size_t>(column)] =
                left ? redLuma : blueLuma;
        }
    }
    for (int row{0}; row < frameHeight / 2; ++row) {
        for (int pair{0}; pair < frameWidth / 2; ++pair) {
            const bool left{pair < frameWidth / 4};
            const std::size_t offset{lumaSize + static_cast<std::size_t>(row) * frameWidth +
                                     static_cast<std::size_t>(pair) * 2};
            bytes[offset] = left ? redU : blueU;
            bytes[offset + 1] = left ? redV : blueV;
        }
    }
    return source.endCpuAccess();
}

Rgb pixelAt(const DmaBuf& target, int row, int column) {
    const auto* bytes{static_cast<const std::uint8_t*>(target.data())};
    const std::size_t offset{
        (static_cast<std::size_t>(row) * modelSize + static_cast<std::size_t>(column)) *
        rgbChannels};
    return Rgb{bytes[offset], bytes[offset + 1], bytes[offset + 2]};
}

bool near(const Rgb& actual, const Rgb& expected) {
    const auto close{[](int first, int second) {
        return first - second <= colorTolerance && second - first <= colorTolerance;
    }};
    return close(actual.red, expected.red) && close(actual.green, expected.green) &&
           close(actual.blue, expected.blue);
}

bool expectColor(const DmaBuf& target, int row, int column, const Rgb& expected, const char* what,
                 const Logger& log) {
    const Rgb actual{pixelAt(target, row, column)};
    if (!near(actual, expected)) {
        log.error(what, ": pixel (", row, ",", column, ") is ", actual.red, ",", actual.green, ",",
                  actual.blue, ", expected ", expected.red, ",", expected.green, ",",
                  expected.blue);
        return false;
    }
    return true;
}

// Runs one rotation and checks two points inside the image area, the padding and the layout.
bool checkRotation(Rotation rotation, const char* name, const DmaBuf& source, const Logger& log) {
    std::optional<DmaBuf> target{DmaBuf::allocate(
        DmaBufConfig{lowHeap, static_cast<std::size_t>(modelSize) * modelSize * rgbChannels}, log)};
    const std::optional<RgaLetterbox> letterbox{RgaLetterbox::create(
        LetterboxConfig{ImageSize{frameWidth, frameHeight}, frameWidth, modelSize, rotation, false},
        log)};
    if (!target || !letterbox) {
        return false;
    }
    const std::optional<RgaImage> sourceImage{RgaImage::import(source, log)};
    const std::optional<RgaImage> targetImage{RgaImage::import(*target, log)};
    if (!sourceImage || !targetImage || !letterbox->fillPadding(*targetImage) ||
        !letterbox->run(*sourceImage, *targetImage) || !target->beginCpuAccess()) {
        return false;
    }
    const bool turned{rotation == Rotation::Rotate90 || rotation == Rotation::Rotate270};
    bool passed{expectColor(*target, cornerPoint, cornerPoint,
                            Rgb{paddingValue, paddingValue, paddingValue}, name, log)};
    if (turned) {
        // The image is 360 wide and 640 high. 90 clockwise puts the left half on top.
        const Rgb top{rotation == Rotation::Rotate90 ? redColor : blueColor};
        const Rgb bottom{rotation == Rotation::Rotate90 ? blueColor : redColor};
        passed = expectColor(*target, nearPoint, middlePoint, top, name, log) && passed;
        passed = expectColor(*target, farPoint, middlePoint, bottom, name, log) && passed;
    } else {
        // The image is 640 wide and 360 high, centred: rows 140 to 499.
        const Rgb left{rotation == Rotation::None ? redColor : blueColor};
        const Rgb right{rotation == Rotation::None ? blueColor : redColor};
        passed = expectColor(*target, middlePoint, nearPoint, left, name, log) && passed;
        passed = expectColor(*target, middlePoint, farPoint, right, name, log) && passed;
    }
    log.info("rotation ", name, passed ? " ok" : " FAIL");
    return passed;
}
} // namespace

namespace {
// Gray values that the two ranges convert differently: the limited range stretches 16 to 235 over
// 0 to 255, the full range leaves them as they are.
constexpr std::uint8_t darkLuma{32};
constexpr std::uint8_t brightLuma{224};
constexpr std::uint8_t neutralChroma{128};
constexpr int fullDark{32};
constexpr int fullBright{224};
constexpr int limitedDark{19};
constexpr int limitedBright{242};

// Left half dark gray, right half bright gray.
bool paintGray(DmaBuf& source) {
    auto* bytes{static_cast<std::uint8_t*>(source.data())};
    const std::size_t lumaSize{static_cast<std::size_t>(frameWidth) * frameHeight};
    for (int row{0}; row < frameHeight; ++row) {
        for (int column{0}; column < frameWidth; ++column) {
            bytes[static_cast<std::size_t>(row) * frameWidth + static_cast<std::size_t>(column)] =
                column < frameWidth / 2 ? darkLuma : brightLuma;
        }
    }
    std::fill(bytes + lumaSize, bytes + lumaSize * 3 / 2, neutralChroma);
    return source.endCpuAccess();
}

// The same gray image converted as full range and as limited range gives different values.
bool checkRange(bool fullRange, const DmaBuf& source, const Logger& log) {
    std::optional<DmaBuf> target{DmaBuf::allocate(
        DmaBufConfig{lowHeap, static_cast<std::size_t>(modelSize) * modelSize * rgbChannels}, log)};
    const std::optional<RgaLetterbox> letterbox{
        RgaLetterbox::create(LetterboxConfig{ImageSize{frameWidth, frameHeight}, frameWidth,
                                             modelSize, Rotation::None, fullRange},
                             log)};
    if (!target || !letterbox) {
        return false;
    }
    const std::optional<RgaImage> sourceImage{RgaImage::import(source, log)};
    const std::optional<RgaImage> targetImage{RgaImage::import(*target, log)};
    if (!sourceImage || !targetImage || !letterbox->fillPadding(*targetImage) ||
        !letterbox->run(*sourceImage, *targetImage) || !target->beginCpuAccess()) {
        return false;
    }
    const int dark{fullRange ? fullDark : limitedDark};
    const int bright{fullRange ? fullBright : limitedBright};
    const char* name{fullRange ? "full range" : "limited range"};
    const bool passed{
        expectColor(*target, middlePoint, nearPoint, Rgb{dark, dark, dark}, name, log) &&
        expectColor(*target, middlePoint, farPoint, Rgb{bright, bright, bright}, name, log)};
    log.info(name, passed ? " ok" : " FAIL");
    return passed;
}
} // namespace

namespace {
// BT.709 limited range of pure red and pure blue.
constexpr std::uint8_t red709Luma{63};
constexpr std::uint8_t red709U{102};
constexpr std::uint8_t red709V{240};
constexpr std::uint8_t blue709Luma{32};
constexpr std::uint8_t blue709U{240};
constexpr std::uint8_t blue709V{118};
// A decoder pads the height of a 360 line frame to 368.
constexpr int paddedHeight{368};

struct Yuv {
    std::uint8_t luma;
    std::uint8_t u;
    std::uint8_t v;
};

// A band of 8 lines where the two halves swap. A chroma plane read from the wrong line puts the
// band 8 lines away, so a probe in the band catches a wrong vertical stride.
constexpr int bandTop{96};
constexpr int bandLines{8};
constexpr int bandProbe{bandTop + bandLines / 2};
// The row of the output where the image starts: 640x360 centred in 640 lines.
constexpr int imageTop{(modelSize - frameHeight) / 2};

// Left half `left`, right half `right` (swapped in the band), in a buffer of `rows` luma lines: the
// chroma plane starts after `rows` lines.
bool paintHalves(DmaBuf& source, int rows, const Yuv& left, const Yuv& right) {
    auto* bytes{static_cast<std::uint8_t*>(source.data())};
    const std::size_t lumaSize{static_cast<std::size_t>(frameWidth) * rows};
    std::fill(bytes, bytes + lumaSize * 3 / 2, neutralChroma);
    const auto inBand{[](int row) { return row >= bandTop && row < bandTop + bandLines; }};
    for (int row{0}; row < frameHeight; ++row) {
        for (int column{0}; column < frameWidth; ++column) {
            const bool first{(column < frameWidth / 2) != inBand(row)};
            bytes[static_cast<std::size_t>(row) * frameWidth + static_cast<std::size_t>(column)] =
                first ? left.luma : right.luma;
        }
    }
    for (int row{0}; row < frameHeight / 2; ++row) {
        for (int pair{0}; pair < frameWidth / 2; ++pair) {
            const bool first{(pair < frameWidth / 4) != inBand(row * 2)};
            const std::size_t offset{lumaSize + static_cast<std::size_t>(row) * frameWidth +
                                     static_cast<std::size_t>(pair) * 2};
            bytes[offset] = first ? left.u : right.u;
            bytes[offset + 1] = first ? left.v : right.v;
        }
    }
    return source.endCpuAccess();
}

// Letterboxes a red and blue image painted with the values of `matrix`, in a buffer of `rows`
// lines, and checks the two colours. The image is not rotated: red on the left, blue on the right.
bool checkMatrix(ColorMatrix matrix, int rows, const char* name, const Logger& log) {
    const bool bt709{matrix == ColorMatrix::Bt709};
    std::optional<DmaBuf> source{DmaBuf::allocate(
        DmaBufConfig{defaultHeap, static_cast<std::size_t>(frameWidth) * rows * 3 / 2}, log)};
    std::optional<DmaBuf> target{DmaBuf::allocate(
        DmaBufConfig{lowHeap, static_cast<std::size_t>(modelSize) * modelSize * rgbChannels}, log)};
    if (!source || !target ||
        !paintHalves(*source, rows,
                     bt709 ? Yuv{red709Luma, red709U, red709V} : Yuv{redLuma, redU, redV},
                     bt709 ? Yuv{blue709Luma, blue709U, blue709V} : Yuv{blueLuma, blueU, blueV})) {
        return false;
    }
    const std::optional<RgaLetterbox> letterbox{RgaLetterbox::create(
        LetterboxConfig{ImageSize{frameWidth, frameHeight}, frameWidth, modelSize, Rotation::None,
                        false, rows == frameHeight ? 0 : rows, matrix},
        log)};
    if (!letterbox) {
        return false;
    }
    const std::optional<RgaImage> sourceImage{RgaImage::import(*source, log)};
    const std::optional<RgaImage> targetImage{RgaImage::import(*target, log)};
    if (!sourceImage || !targetImage || !letterbox->fillPadding(*targetImage) ||
        !letterbox->run(*sourceImage, *targetImage) || !target->beginCpuAccess()) {
        return false;
    }
    const bool passed{expectColor(*target, middlePoint, nearPoint, redColor, name, log) &&
                      expectColor(*target, middlePoint, farPoint, blueColor, name, log) &&
                      expectColor(*target, imageTop + bandProbe, nearPoint, blueColor, name, log) &&
                      expectColor(*target, imageTop + bandProbe, farPoint, redColor, name, log)};
    log.info(name, passed ? " ok" : " FAIL");
    return passed;
}

// A full range source cannot take the BT.709 matrix: the RGA has none.
bool checkFullRangeBt709Refused(const Logger& log) {
    const bool refused{!RgaLetterbox::create(LetterboxConfig{ImageSize{frameWidth, frameHeight},
                                                             frameWidth, modelSize, Rotation::None,
                                                             true, 0, ColorMatrix::Bt709},
                                             log)
                            .has_value()};
    log.info("full range with BT.709 refused", refused ? " ok" : " FAIL");
    return refused;
}
} // namespace

int main() {
    const Logger log{"TEST_RGA", LogLevel::Debug};
    std::optional<DmaBuf> source{DmaBuf::allocate(
        DmaBufConfig{defaultHeap, static_cast<std::size_t>(frameWidth) * frameHeight * 3 / 2},
        log)};
    if (!source || !paintSource(*source)) {
        log.error("FAIL");
        return EXIT_FAILURE;
    }
    constexpr std::array<std::pair<Rotation, const char*>, 4> cases{{
        {Rotation::None, "0"},
        {Rotation::Rotate90, "90"},
        {Rotation::Rotate180, "180"},
        {Rotation::Rotate270, "270"},
    }};
    bool passed{true};
    for (const auto& item : cases) {
        passed = checkRotation(item.first, item.second, *source, log) && passed;
    }
    passed = paintGray(*source) && passed;
    passed = checkRange(false, *source, log) && passed;
    passed = checkRange(true, *source, log) && passed;
    passed = checkMatrix(ColorMatrix::Bt709, frameHeight, "BT.709 limited", log) && passed;
    passed = checkMatrix(ColorMatrix::Bt601, paddedHeight, "BT.601 padded height", log) && passed;
    passed = checkMatrix(ColorMatrix::Bt709, paddedHeight, "BT.709 padded height", log) && passed;
    passed = checkFullRangeBt709Refused(log) && passed;
    if (passed) {
        log.info("PASS");
    } else {
        log.error("FAIL");
    }
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
