// RgaSnapshot on the board: the copy of an NV12 frame is exact, rotated as asked, and a camera
// buffer can be reused as soon as run() returns. The source is a synthetic 640x360 NV12 image whose
// left half is bright with one chroma pair and whose right half is dark with another. Needs
// /dev/dma_heap/system and the RGA. Exit code 0 when every check passes.
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <utility>

#include "dmabuf.h"
#include "logger.h"
#include "rga.h"

namespace {
constexpr int frameWidth{640};
constexpr int frameHeight{360};
constexpr std::uint8_t brightLuma{200};
constexpr std::uint8_t darkLuma{50};
constexpr std::uint8_t brightU{100};
constexpr std::uint8_t brightV{160};
constexpr std::uint8_t darkU{160};
constexpr std::uint8_t darkV{100};
constexpr int probeFraction{4}; // probe points at a quarter and three quarters of each side
// A decoder pads the height of a 360 line frame to 368: the chroma plane starts after 368 lines.
constexpr int paddedHeight{368};

// A band of 8 lines where the two halves swap. A chroma plane read from the wrong line puts the
// band 8 lines away, so a probe in the band catches a wrong vertical stride.
constexpr int bandTop{96};
constexpr int bandLines{8};
constexpr int bandProbe{bandTop + bandLines / 2};

// Left half bright, right half dark, each with its own chroma pair (swapped in the band), in a
// buffer of `rows` luma lines.
bool paintSource(DmaBuf& source, int rows) {
    auto* bytes{static_cast<std::uint8_t*>(source.data())};
    const std::size_t lumaSize{static_cast<std::size_t>(frameWidth) * rows};
    const auto inBand{[](int row) { return row >= bandTop && row < bandTop + bandLines; }};
    for (int row{0}; row < frameHeight; ++row) {
        for (int column{0}; column < frameWidth; ++column) {
            bytes[(static_cast<std::size_t>(row) * frameWidth) + static_cast<std::size_t>(column)] =
                (column < frameWidth / 2) != inBand(row) ? brightLuma : darkLuma;
        }
    }
    for (int row{0}; row < frameHeight / 2; ++row) {
        for (int pair{0}; pair < frameWidth / 2; ++pair) {
            const bool left{(pair < frameWidth / 4) != inBand(row * 2)};
            const std::size_t offset{lumaSize + (static_cast<std::size_t>(row) * frameWidth) +
                                     (static_cast<std::size_t>(pair) * 2)};
            bytes[offset] = left ? brightU : darkU;
            bytes[offset + 1] = left ? brightV : darkV;
        }
    }
    return source.endCpuAccess();
}

struct Expected {
    std::uint8_t luma;
    std::uint8_t chromaU;
    std::uint8_t chromaV;
};

// Luma and chroma pair of the copy at a pixel (row, column), NV12 with the given stride.
// Size and bytes per line of a copy.
struct CopyLayout {
    ImageSize size{};
    int stride{0};
};

// A pixel of the copy.
struct Pixel {
    int row;
    int column;
};

bool matches(const DmaBuf& copy, const CopyLayout& layout, const Pixel& pixel,
             const Expected& expected, const char* what, const Logger& log) {
    const ImageSize size{layout.size};
    const int stride{layout.stride};
    const int row{pixel.row};
    const int column{pixel.column};
    const auto* bytes{static_cast<const std::uint8_t*>(copy.data())};
    const std::size_t width{static_cast<std::size_t>(stride)};
    const std::size_t lumaSize{width * static_cast<std::size_t>(size.height)};
    const std::size_t luma{(static_cast<std::size_t>(row) * width) +
                           static_cast<std::size_t>(column)};
    const std::size_t chroma{lumaSize + (static_cast<std::size_t>(row / 2) * width) +
                             (static_cast<std::size_t>(column / 2) * 2)};
    const bool same{bytes[luma] == expected.luma && bytes[chroma] == expected.chromaU &&
                    bytes[chroma + 1] == expected.chromaV};
    if (!same) {
        log.error(what, ": pixel (", row, ",", column, ") has Y=", bytes[luma],
                  " U=", bytes[chroma], " V=", bytes[chroma + 1], ", expected Y=", expected.luma,
                  " U=", expected.chromaU, " V=", expected.chromaV);
    }
    return same;
}

constexpr Expected brightPixel{brightLuma, brightU, brightV};
constexpr Expected darkPixel{darkLuma, darkU, darkV};

// One rotation: the size of the copy and where the bright half went. `rows` is the vertical stride
// of the source buffer, 0 when it is as high as the image.
bool checkRotation(Rotation rotation, const char* name, const DmaBuf& source, int rows,
                   const Logger& log) {
    const std::optional<RgaSnapshot> snapshot{RgaSnapshot::create(
        SnapshotConfig{ImageSize{frameWidth, frameHeight}, frameWidth, rotation, ImageSize{}, rows},
        log)};
    if (!snapshot) {
        return false;
    }
    std::optional<DmaBuf> copy{DmaBuf::allocate(DmaBufConfig{defaultHeap, snapshot->bytes()}, log)};
    if (!copy) {
        return false;
    }
    const std::optional<RgaImage> sourceImage{RgaImage::import(source, log)};
    const std::optional<RgaImage> copyImage{RgaImage::import(*copy, log)};
    if (!sourceImage || !copyImage || !snapshot->run(*sourceImage, *copyImage) ||
        !copy->beginCpuAccess()) {
        return false;
    }
    const ImageSize size{snapshot->size()};
    const CopyLayout layout{size, snapshot->stride()};
    const bool turned{rotation == Rotation::Rotate90 || rotation == Rotation::Rotate270};
    bool passed{turned ? size.width == frameHeight && size.height == frameWidth
                       : size.width == frameWidth && size.height == frameHeight};
    const int rowA{size.height / probeFraction};
    const int rowB{size.height - rowA};
    const int columnA{size.width / probeFraction};
    const int columnB{size.width - columnA};
    if (turned) {
        // 90 clockwise puts the left half on top, 270 puts it at the bottom.
        const Expected& top{rotation == Rotation::Rotate90 ? brightPixel : darkPixel};
        const Expected& bottom{rotation == Rotation::Rotate90 ? darkPixel : brightPixel};
        passed = matches(*copy, layout, Pixel{rowA, columnA}, top, name, log) && passed;
        passed = matches(*copy, layout, Pixel{rowB, columnB}, bottom, name, log) && passed;
    } else {
        const Expected& left{rotation == Rotation::None ? brightPixel : darkPixel};
        const Expected& right{rotation == Rotation::None ? darkPixel : brightPixel};
        passed = matches(*copy, layout, Pixel{rowA, columnA}, left, name, log) && passed;
        passed = matches(*copy, layout, Pixel{rowB, columnB}, right, name, log) && passed;
        if (rotation == Rotation::None) {
            // In the band the halves are swapped.
            passed =
                matches(*copy, layout, Pixel{bandProbe, columnA}, darkPixel, name, log) && passed;
            passed =
                matches(*copy, layout, Pixel{bandProbe, columnB}, brightPixel, name, log) && passed;
        }
    }
    log.info("snapshot rotation ", name, passed ? " ok" : " FAIL");
    return passed;
}

bool checkBadConfig(const Logger& log) {
    const bool oddWidth{!RgaSnapshot::create(
        SnapshotConfig{ImageSize{frameWidth + 1, frameHeight}, frameWidth + 1, Rotation::None},
        log)};
    const bool shortStride{!RgaSnapshot::create(
        SnapshotConfig{ImageSize{frameWidth, frameHeight}, frameWidth - 2, Rotation::None}, log)};
    const bool shortRows{
        !RgaSnapshot::create(SnapshotConfig{ImageSize{frameWidth, frameHeight}, frameWidth,
                                            Rotation::None, ImageSize{}, frameHeight - 2},
                             log)};
    return oddWidth && shortStride && shortRows;
}
} // namespace

int main() {
    const Logger log{"TEST_SNAPSHOT", LogLevel::Debug};
    std::optional<DmaBuf> source{DmaBuf::allocate(
        DmaBufConfig{defaultHeap, static_cast<std::size_t>(frameWidth) * frameHeight * 3 / 2},
        log)};
    std::optional<DmaBuf> padded{DmaBuf::allocate(
        DmaBufConfig{defaultHeap, static_cast<std::size_t>(frameWidth) * paddedHeight * 3 / 2},
        log)};
    if (!source || !padded || !paintSource(*source, frameHeight) ||
        !paintSource(*padded, paddedHeight)) {
        log.error("FAIL");
        return EXIT_FAILURE;
    }
    constexpr std::array<std::pair<Rotation, const char*>, 4> cases{{
        {Rotation::None, "0"},
        {Rotation::Rotate90, "90"},
        {Rotation::Rotate180, "180"},
        {Rotation::Rotate270, "270"},
    }};
    bool passed{checkBadConfig(log)};
    for (const auto& item : cases) {
        passed = checkRotation(item.first, item.second, *source, 0, log) && passed;
    }
    passed =
        checkRotation(Rotation::None, "0, padded height", *padded, paddedHeight, log) && passed;
    if (passed) {
        log.info("PASS");
    } else {
        log.error("FAIL");
    }
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
