// RgaLetterbox on the board: colour order, rotation direction, padding, geometry. The source is a
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
        DmaBufConfig{defaultHeap, static_cast<std::size_t>(modelSize) * modelSize * rgbChannels},
        log)};
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
        DmaBufConfig{defaultHeap, static_cast<std::size_t>(modelSize) * modelSize * rgbChannels},
        log)};
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
    if (passed) {
        log.info("PASS");
    } else {
        log.error("FAIL");
    }
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
