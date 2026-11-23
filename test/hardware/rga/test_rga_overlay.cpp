// RgaOverlay on the board: the buffer check, the colour of each style, the position of the lines,
// the violator on top, jobs of several styles, clipping, skipped boxes and the capacity. The target
// is a 1280x720 NV12 image from the lowHeap, dark gray. A pixel is judged by the nearest of the
// style colours, because the RGA turns the colour into limited range YUV and the result is a little
// pale. Needs /dev/dma_heap/reserved (see sh/env/install_udev_rules.sh) and the RGA2. Exit code 0
// when every check passes.
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <utility>
#include <vector>

#include "dmabuf.h"
#include "logger.h"
#include "rga_overlay.h"

namespace {
constexpr int imageWidth{1280};
constexpr int imageHeight{720};
constexpr std::size_t nv12Bytes{static_cast<std::size_t>(imageWidth) * imageHeight * 3 / 2};
constexpr std::uint8_t backgroundLuma{64};
constexpr std::uint8_t neutralChroma{128};
constexpr int boxWidth{100};
constexpr int boxHeight{80};
constexpr int firstX{40};
constexpr int firstY{40};
constexpr int columnStep{150};
constexpr int rowStep{110};
constexpr int columns{8};
constexpr int unknownClass{9};
constexpr int classCount{6};
constexpr int cornerMargin{5};
constexpr int overCapacityBoxes{6};
constexpr std::size_t smallCapacity{4};
constexpr int manyBoxes{30};
constexpr double lumaRed{1.402};
constexpr double chromaBlueToGreen{0.344136};
constexpr double chromaRedToGreen{0.714136};
constexpr double lumaBlue{1.772};
constexpr double chromaCenter{128.0};
constexpr double maxChannel{255.0};

// A position in the grid of boxes, or a pixel.
struct Cell {
    int column{0};
    int row{0};
};

// Pixels to look at in checkEdges: on the clipped box, where a 2 pixel box would be, on the box
// with odd coordinates.
constexpr Cell clippedProbe{100, 602};
constexpr Cell skippedProbe{500, 350};
constexpr Cell roundedProbe{302, 350};

struct Rgb {
    int red{0};
    int green{0};
    int blue{0};
};

// What each colour value of rga_overlay.h means, in RGB, to judge a pixel by the nearest.
constexpr Rgb backgroundRgb{64, 64, 64};
constexpr Rgb helmetRgb{0, 200, 0};
constexpr Rgb noHelmetRgb{255, 40, 40};
constexpr Rgb noVestRgb{255, 140, 0};
constexpr Rgb vestRgb{0, 190, 170};
constexpr Rgb regularHatRgb{160, 90, 220};
constexpr Rgb personRgb{255, 255, 255};
constexpr Rgb violatorRgb{255, 230, 0};
constexpr Rgb unknownRgb{128, 128, 128};
constexpr std::array<Rgb, 9> palette{backgroundRgb, helmetRgb, noHelmetRgb, noVestRgb, vestRgb,
                                     regularHatRgb, personRgb, violatorRgb, unknownRgb};
enum class Paint : std::uint8_t {
    Background,
    Helmet,
    NoHelmet,
    NoVest,
    Vest,
    RegularHat,
    Person,
    Violator,
    Unknown
};

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

int clampChannel(double value) {
    return static_cast<int>(std::min(std::max(value, 0.0), maxChannel));
}

// The pixel of an NV12 image as RGB, full range BT.601.
Rgb pixel(const DmaBuf& image, int column, int row) {
    const auto* bytes{static_cast<const std::uint8_t*>(image.data())};
    const std::size_t luma{(static_cast<std::size_t>(row) * imageWidth) +
                           static_cast<std::size_t>(column)};
    const std::size_t chroma{static_cast<std::size_t>(imageWidth) * imageHeight +
                             (static_cast<std::size_t>(row / 2) * imageWidth) +
                             (static_cast<std::size_t>(column / 2) * 2)};
    const double lumaValue{static_cast<double>(bytes[luma])};
    const double blueDiff{static_cast<double>(bytes[chroma]) - chromaCenter};
    const double redDiff{static_cast<double>(bytes[chroma + 1]) - chromaCenter};
    return Rgb{
        clampChannel(lumaValue + (lumaRed * redDiff)),
        clampChannel(lumaValue - (chromaBlueToGreen * blueDiff) - (chromaRedToGreen * redDiff)),
        clampChannel(lumaValue + (lumaBlue * blueDiff))};
}

int distance(const Rgb& first, const Rgb& second) {
    const int red{first.red - second.red};
    const int green{first.green - second.green};
    const int blue{first.blue - second.blue};
    return (red * red) + (green * green) + (blue * blue);
}

// The paint whose colour is the nearest to the pixel.
Paint classify(const DmaBuf& image, int column, int row) {
    const Rgb seen{pixel(image, column, row)};
    std::size_t best{0};
    for (std::size_t index{1}; index < palette.size(); ++index) {
        if (distance(seen, palette.at(index)) < distance(seen, palette.at(best))) {
            best = index;
        }
    }
    return static_cast<Paint>(best);
}

// Dark gray everywhere.
bool paintBackground(DmaBuf& image) {
    auto* bytes{static_cast<std::uint8_t*>(image.data())};
    const std::size_t lumaSize{static_cast<std::size_t>(imageWidth) * imageHeight};
    std::fill(bytes, bytes + lumaSize, backgroundLuma);
    std::fill(bytes + lumaSize, bytes + nv12Bytes, neutralChroma);
    return image.endCpuAccess();
}

// The target buffer, its RGA handle and the overlay. The handle is declared after the buffer so it
// is released first.
struct Scene {
    DmaBuf image;
    RgaImage handle;
    RgaOverlay overlay;
};

OverlayConfig configOf(std::size_t capacity = defaultOverlayCapacity) {
    OverlayConfig config;
    config.size = ImageSize{imageWidth, imageHeight};
    config.stride = imageWidth;
    config.capacity = capacity;
    return config;
}

std::optional<Scene> makeScene(const Logger& log, std::size_t capacity = defaultOverlayCapacity) {
    std::optional<DmaBuf> image{DmaBuf::allocate(DmaBufConfig{lowHeap, nv12Bytes}, log)};
    if (!image || !paintBackground(*image)) {
        return std::nullopt;
    }
    std::optional<RgaImage> handle{RgaImage::import(*image, log)};
    std::optional<RgaOverlay> overlay{RgaOverlay::create(configOf(capacity), log)};
    if (!handle || !overlay) {
        return std::nullopt;
    }
    return Scene{std::move(*image), std::move(*handle), std::move(*overlay)};
}

// A box of the standard size at a grid cell.
PixelBox cell(const Cell& position) {
    const float left{static_cast<float>(firstX + (position.column * columnStep))};
    const float top{static_cast<float>(firstY + (position.row * rowStep))};
    return PixelBox{left, top, left + static_cast<float>(boxWidth),
                    top + static_cast<float>(boxHeight)};
}

// The paint on the left line of a box, at its middle height, one pixel inside the left edge.
Paint leftEdge(const DmaBuf& image, const PixelBox& box) {
    return classify(image, static_cast<int>(box.left) + 1,
                    static_cast<int>((box.top + box.bottom) / 2));
}

Paint topEdge(const DmaBuf& image, const PixelBox& box) {
    return classify(image, static_cast<int>((box.left + box.right) / 2),
                    static_cast<int>(box.top) + 1);
}

Paint center(const DmaBuf& image, const PixelBox& box) {
    return classify(image, static_cast<int>((box.left + box.right) / 2),
                    static_cast<int>((box.top + box.bottom) / 2));
}

void checkConfig(Check& check, const Logger& log) {
    check.expect(RgaOverlay::create(configOf(), log).has_value(), "a good configuration");
    OverlayConfig bad{configOf()};
    bad.size.width = 0;
    check.expect(!RgaOverlay::create(bad, log), "zero width refused");
    bad = configOf();
    bad.size.width = imageWidth + 1;
    bad.stride = imageWidth + 1;
    check.expect(!RgaOverlay::create(bad, log), "odd width refused");
    bad = configOf();
    bad.stride = imageWidth - 2;
    check.expect(!RgaOverlay::create(bad, log), "stride below the width refused");
    bad = configOf();
    bad.capacity = 0;
    check.expect(!RgaOverlay::create(bad, log), "zero capacity refused");
    bad = configOf();
    bad.violatorStyle.thickness = 0;
    check.expect(!RgaOverlay::create(bad, log), "zero thickness refused");
}

void checkStyles(Check& check, const Logger& log) {
    std::optional<Scene> made{makeScene(log)};
    if (!made) {
        check.expect(false, "scene made");
        return;
    }
    Scene& scene{*made};
    check.expect(scene.overlay.check(scene.handle), "the RGA2 accepts a lowHeap buffer");
    check.expect(paintBackground(scene.image), "background repainted");
    const std::vector<OverlayBox> boxes{
        {cell(Cell{0, 0}), 0, false}, {cell(Cell{1, 0}), 1, false},
        {cell(Cell{2, 0}), 2, false}, {cell(Cell{3, 0}), 3, false},
        {cell(Cell{4, 0}), 4, false}, {cell(Cell{5, 0}), 5, false},
        {cell(Cell{6, 0}), unknownClass, false}, {cell(Cell{7, 0}), 5, true},
    };
    check.expect(scene.overlay.draw(scene.handle, boxes), "draw succeeds");
    check.expect(scene.image.beginCpuAccess(), "CPU access");
    const std::array<Paint, 8> expected{Paint::Helmet,     Paint::NoHelmet, Paint::NoVest,
                                        Paint::Vest,       Paint::RegularHat, Paint::Person,
                                        Paint::Unknown,    Paint::Violator};
    for (std::size_t index{0}; index < expected.size(); ++index) {
        check.expect(leftEdge(scene.image, boxes.at(index).box) == expected.at(index),
                     "the left line has the colour of its style");
        check.expect(topEdge(scene.image, boxes.at(index).box) == expected.at(index),
                     "the top line has the colour of its style");
        check.expect(center(scene.image, boxes.at(index).box) == Paint::Background,
                     "the inside of a box is not touched");
    }
    check.expect(classify(scene.image, cornerMargin, imageHeight - cornerMargin) ==
                     Paint::Background,
                 "outside every box is not touched");
}

void checkViolatorOnTop(Check& check, const Logger& log) {
    std::optional<Scene> made{makeScene(log)};
    if (!made) {
        check.expect(false, "scene made");
        return;
    }
    Scene& scene{*made};
    // The violator is listed first and the person last, yet the violator is on top.
    const std::vector<OverlayBox> boxes{{cell(Cell{0, 0}), 5, true}, {cell(Cell{0, 0}), 5, false}};
    check.expect(scene.overlay.draw(scene.handle, boxes), "draw succeeds");
    check.expect(scene.image.beginCpuAccess(), "CPU access");
    check.expect(leftEdge(scene.image, boxes.at(0).box) == Paint::Violator,
                 "the violator is on top of the person");
}

void checkJobs(Check& check, const Logger& log) {
    std::optional<Scene> made{makeScene(log)};
    if (!made) {
        check.expect(false, "scene made");
        return;
    }
    Scene& scene{*made};
    // 30 boxes of six classes in turn: three jobs, each with several styles.
    std::vector<OverlayBox> boxes;
    for (int index{0}; index < manyBoxes; ++index) {
        boxes.push_back(
            OverlayBox{cell(Cell{index % columns, index / columns}), index % classCount, false});
    }
    check.expect(scene.overlay.draw(scene.handle, boxes), "draw of 30 boxes succeeds");
    check.expect(scene.image.beginCpuAccess(), "CPU access");
    constexpr std::array<Paint, classCount> byClass{Paint::Helmet, Paint::NoHelmet, Paint::NoVest,
                                                    Paint::Vest, Paint::RegularHat, Paint::Person};
    bool allRight{true};
    for (std::size_t index{0}; index < boxes.size(); ++index) {
        allRight = allRight && topEdge(scene.image, boxes.at(index).box) ==
                                   byClass.at(static_cast<std::size_t>(boxes.at(index).classId));
    }
    check.expect(allRight, "every one of 30 boxes has the colour of its class");
}

void checkEdges(Check& check, const Logger& log) {
    std::optional<Scene> made{makeScene(log)};
    if (!made) {
        check.expect(false, "scene made");
        return;
    }
    Scene& scene{*made};
    check.expect(scene.overlay.draw(scene.handle, {}), "no box is a success");
    // Clipped: partly outside on the left and the bottom. Skipped: 2 pixels wide, and fully out.
    const std::vector<OverlayBox> boxes{
        {PixelBox{-50.0F, 600.0F, 150.0F, 900.0F}, 1, false},
        {PixelBox{500.0F, 300.0F, 502.0F, 400.0F}, 1, false},
        {PixelBox{2000.0F, 2000.0F, 2100.0F, 2100.0F}, 1, false},
        {PixelBox{301.0F, 301.0F, 403.0F, 403.0F}, 2, false},
    };
    check.expect(scene.overlay.draw(scene.handle, boxes), "clipped and skipped boxes succeed");
    check.expect(scene.image.beginCpuAccess(), "CPU access");
    check.expect(classify(scene.image, clippedProbe.column, clippedProbe.row) == Paint::NoHelmet,
                 "a clipped box is drawn inside the image");
    check.expect(classify(scene.image, skippedProbe.column, skippedProbe.row) == Paint::Background,
                 "a box of 2 pixels is skipped");
    check.expect(classify(scene.image, roundedProbe.column, roundedProbe.row) == Paint::NoVest,
                 "odd coordinates are rounded outwards to even ones");
}

void checkCapacity(Check& check, const Logger& log) {
    std::optional<Scene> made{makeScene(log, smallCapacity)};
    if (!made) {
        check.expect(false, "scene made");
        return;
    }
    Scene& scene{*made};
    std::vector<OverlayBox> boxes;
    for (int index{0}; index < overCapacityBoxes; ++index) {
        boxes.push_back(OverlayBox{cell(Cell{index, 0}), 1, false});
    }
    check.expect(scene.overlay.draw(scene.handle, boxes), "draw above the capacity succeeds");
    check.expect(scene.overlay.overflowCount() == 2, "boxes above the capacity are counted");
}
} // namespace

int main() {
    const Logger log{"TEST_OVERLAY", LogLevel::Debug};
    Check check{log};
    checkConfig(check, log);
    checkStyles(check, log);
    checkViolatorOnTop(check, log);
    checkJobs(check, log);
    checkEdges(check, log);
    checkCapacity(check, log);
    if (check.ok()) {
        log.info("PASS");
        return EXIT_SUCCESS;
    }
    log.error("FAIL");
    return EXIT_FAILURE;
}
