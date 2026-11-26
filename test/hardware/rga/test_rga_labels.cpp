// RgaLabels on the board: the labels have the colour of their box, the text is drawn on it, a
// Person label goes under its box and the others above, a violator label is on top, labels stay
// inside the image, many labels go through several jobs, and the capacity is counted. The target is
// a 1280x720 NV12 image from the lowHeap, dark gray. A pixel is judged by the nearest colour of a
// small palette, because the RGA turns the colour into limited range YUV. Needs
// /dev/dma_heap/reserved and the RGA2. Exit code 0 when every check passes.
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "class_map.h"
#include "dmabuf.h"
#include "label_text.h"
#include "logger.h"
#include "rga_labels.h"

namespace {
constexpr int imageWidth{1280};
constexpr int imageHeight{720};
constexpr std::size_t nv12Bytes{static_cast<std::size_t>(imageWidth) * imageHeight * 3 / 2};
constexpr std::uint8_t backgroundLuma{64};
constexpr std::uint8_t neutralChroma{128};
constexpr int boxWidth{100};
constexpr int boxHeight{80};
constexpr int firstX{40};
constexpr int firstY{100};
constexpr int columnStep{150};
constexpr int rowStep{110};
constexpr int columns{8};
constexpr int classCount{6};
constexpr int personClass{5};
constexpr int manyLabels{48};
constexpr float testScore{0.5F};
constexpr float highScore{0.9F};
constexpr int topBoxLeft{200};
constexpr int manyColumns{16};
constexpr int manyColumnStep{56};
constexpr int manyRowStep{200};
constexpr int overCapacityLabels{6};
constexpr std::size_t smallCapacity{4};
constexpr int insideMargin{3};
constexpr int minTextPixels{20};
constexpr double lumaRed{1.402};
constexpr double chromaBlueToGreen{0.344136};
constexpr double chromaRedToGreen{0.714136};
constexpr double lumaBlue{1.772};
constexpr double chromaCenter{128.0};
constexpr double maxChannel{255.0};
constexpr std::uint32_t bgrBlack{0x000000U};

struct Rgb {
    int red{0};
    int green{0};
    int blue{0};
};

enum class Paint : std::uint8_t {
    Background,
    Helmet,
    NoHelmet,
    NoVest,
    Vest,
    RegularHat,
    Person,
    Violator,
    Black,
};

constexpr std::array<Rgb, 9> palette{{
    {64, 64, 64},
    {0, 200, 0},
    {255, 40, 40},
    {255, 140, 0},
    {0, 190, 170},
    {160, 90, 220},
    {255, 255, 255},
    {255, 230, 0},
    {0, 0, 0},
}};

constexpr std::array<Paint, classCount> byClass{Paint::Helmet, Paint::NoHelmet,   Paint::NoVest,
                                                Paint::Vest,   Paint::RegularHat, Paint::Person};

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

bool paintBackground(DmaBuf& image) {
    auto* bytes{static_cast<std::uint8_t*>(image.data())};
    const std::size_t lumaSize{static_cast<std::size_t>(imageWidth) * imageHeight};
    std::fill(bytes, bytes + lumaSize, backgroundLuma);
    std::fill(bytes + lumaSize, bytes + nv12Bytes, neutralChroma);
    return image.endCpuAccess();
}

OverlayConfig overlayConfig() {
    OverlayConfig config;
    config.size = ImageSize{imageWidth, imageHeight};
    config.stride = imageWidth;
    return config;
}

// The target buffer, its RGA handle and the labels. The handle is declared after the buffer so it
// is released first.
struct Scene {
    DmaBuf image;
    RgaImage handle;
    RgaLabels labels;
};

std::optional<Scene> makeScene(const Logger& log, std::size_t capacity = defaultLabelCapacity) {
    std::optional<DmaBuf> image{DmaBuf::allocate(DmaBufConfig{lowHeap, nv12Bytes}, log)};
    if (!image || !paintBackground(*image)) {
        return std::nullopt;
    }
    std::optional<RgaImage> handle{RgaImage::import(*image, log)};
    LabelConfig config;
    config.capacity = capacity;
    std::optional<RgaLabels> labels{RgaLabels::create(overlayConfig(), config, log)};
    if (!handle || !labels) {
        return std::nullopt;
    }
    return Scene{std::move(*image), std::move(*handle), std::move(*labels)};
}

// A position in the grid of boxes.
struct Cell {
    int column{0};
    int row{0};
};

PixelBox cell(const Cell& position) {
    const float left{static_cast<float>(firstX + (position.column * columnStep))};
    const float top{static_cast<float>(firstY + (position.row * rowStep))};
    return PixelBox{left, top, left + static_cast<float>(boxWidth),
                    top + static_cast<float>(boxHeight)};
}

// Where the label of a box goes, with the text of this scene's rules.
LabelPosition positionOf(const OverlayBox& box, std::size_t textLength) {
    const bool below{box.violator || box.classId == personClass};
    return placeLabel(box.box, below, labelSize(textLength), ImageSize{imageWidth, imageHeight});
}

// The paint of the label background of a box, just inside its top-left corner.
Paint labelBackground(const DmaBuf& image, const OverlayBox& box, std::size_t textLength) {
    const LabelPosition position{positionOf(box, textLength)};
    return classify(image, position.x + insideMargin, position.y + insideMargin);
}

// Pixels of one paint inside the label rectangle.
int countInLabel(const DmaBuf& image, const OverlayBox& box, std::size_t textLength, Paint paint) {
    const LabelPosition position{positionOf(box, textLength)};
    const LabelSize size{labelSize(textLength)};
    int count{0};
    for (int row{position.y}; row < position.y + size.height; ++row) {
        for (int column{position.x}; column < position.x + size.width; ++column) {
            count += classify(image, column, row) == paint ? 1 : 0;
        }
    }
    return count;
}

void checkConfig(Check& check, const Logger& log) {
    LabelConfig config;
    check.expect(RgaLabels::create(overlayConfig(), config, log).has_value(),
                 "a good configuration");
    config.capacity = 0;
    check.expect(!RgaLabels::create(overlayConfig(), config, log), "zero capacity refused");
    config = LabelConfig{};
    config.classNames.clear();
    check.expect(!RgaLabels::create(overlayConfig(), config, log), "no names refused");
    OverlayConfig bad{overlayConfig()};
    bad.size.height = 0;
    check.expect(!RgaLabels::create(bad, LabelConfig{}, log), "zero height refused");
}

void checkColours(Check& check, const Logger& log) {
    std::optional<Scene> made{makeScene(log)};
    if (!made) {
        check.expect(false, "scene made");
        return;
    }
    Scene& scene{*made};
    const std::vector<OverlayBox> boxes{
        {cell(Cell{0, 0}), 1, false, 0.87F, -1}, // NoHelmet: red, above
        {cell(Cell{1, 0}), 2, false, 0.78F, -1}, // NoVest: orange, above
        {cell(Cell{2, 0}), 5, false, 0.95F, 12}, // Person: white, under
        {cell(Cell{3, 0}), 5, true, 0.95F, 13},  // violator: yellow, under
        {cell(Cell{4, 0}), 3, false, 0.91F, -1}, // Vest: teal, above
    };
    check.expect(scene.labels.draw(scene.handle, boxes), "draw succeeds");
    check.expect(scene.image.beginCpuAccess(), "CPU access");
    // Texts of the labels: NoHelmet 87%, NoVest 78%, #12 Person 95%, ! #13 Person 95%, Vest 91%.
    constexpr std::array<std::size_t, 5> lengths{12, 10, 14, 16, 8};
    constexpr std::array<Paint, 5> expected{Paint::NoHelmet, Paint::NoVest, Paint::Person,
                                            Paint::Violator, Paint::Vest};
    for (std::size_t index{0}; index < boxes.size(); ++index) {
        check.expect(labelBackground(scene.image, boxes.at(index), lengths.at(index)) ==
                         expected.at(index),
                     "the label has the colour of its box");
        const Paint text{expected.at(index) == Paint::NoHelmet ? Paint::Person : Paint::Black};
        check.expect(countInLabel(scene.image, boxes.at(index), lengths.at(index), text) >
                         minTextPixels,
                     "the text is drawn on the label");
    }
    // Person and violator labels are under the box, the others above.
    check.expect(positionOf(boxes.at(2), lengths.at(2)).y >=
                     static_cast<int>(boxes.at(2).box.bottom),
                 "the Person label is under its box");
    check.expect(positionOf(boxes.at(0), lengths.at(0)).y + labelSize(lengths.at(0)).height <=
                     static_cast<int>(boxes.at(0).box.top),
                 "the NoHelmet label is above its box");
}

void checkEdgesAndOrder(Check& check, const Logger& log) {
    std::optional<Scene> made{makeScene(log)};
    if (!made) {
        check.expect(false, "scene made");
        return;
    }
    Scene& scene{*made};
    // A box at the top edge: its label cannot go above, so it is kept inside at y = 0. The two
    // boxes at the same place: the violator label is drawn last, on top of the Person label.
    const PixelBox top{PixelBox{static_cast<float>(topBoxLeft), 10.0F, 300.0F, 90.0F}};
    const PixelBox shared{PixelBox{600.0F, 300.0F, 700.0F, 380.0F}};
    const std::vector<OverlayBox> boxes{
        {top, 1, false, testScore, -1},
        {shared, 5, true, highScore, 7},
        {shared, 5, false, highScore, 7},
    };
    check.expect(scene.labels.draw(scene.handle, boxes), "draw succeeds");
    check.expect(scene.image.beginCpuAccess(), "CPU access");
    check.expect(classify(scene.image, topBoxLeft + insideMargin, insideMargin) == Paint::NoHelmet,
                 "a label that does not fit above is kept inside the image");
    const LabelPosition shown{positionOf(boxes.at(1), 14)};
    check.expect(classify(scene.image, shown.x + insideMargin, shown.y + insideMargin) ==
                     Paint::Violator,
                 "the violator label is on top");
    check.expect(scene.labels.draw(scene.handle, {}), "no boxes is a success");
}

void checkMany(Check& check, const Logger& log) {
    std::optional<Scene> made{makeScene(log)};
    if (!made) {
        check.expect(false, "scene made");
        return;
    }
    Scene& scene{*made};
    std::vector<OverlayBox> boxes;
    for (int index{0}; index < manyLabels; ++index) {
        // 48 labels need two jobs. Rows far enough apart that a label under a box and a label above
        // the next row do not meet; a label may cover the end of its left neighbour, which is drawn
        // first.
        const int column{index % manyColumns};
        const int row{index / manyColumns};
        const float left{static_cast<float>(firstX + (column * manyColumnStep))};
        const float top{static_cast<float>(firstY + (row * manyRowStep))};
        boxes.push_back(OverlayBox{PixelBox{left, top, left + static_cast<float>(boxWidth),
                                            top + static_cast<float>(boxHeight)},
                                   index % classCount, false, testScore, -1});
    }
    check.expect(scene.labels.draw(scene.handle, boxes), "48 labels succeed");
    check.expect(scene.image.beginCpuAccess(), "CPU access");
    bool allRight{true};
    for (const OverlayBox& box : boxes) {
        // "Helmet 50%" is 10 characters, "NoHelmet 50%" 12, "NoVest 50%" 10, "Vest 50%" 8,
        // "RegularHat 50%" 14, "Person 50%" 10.
        constexpr std::array<std::size_t, classCount> lengths{10, 12, 10, 8, 14, 10};
        allRight = allRight && labelBackground(scene.image, box,
                                               lengths.at(static_cast<std::size_t>(box.classId))) ==
                                   byClass.at(static_cast<std::size_t>(box.classId));
    }
    check.expect(allRight, "every one of 48 labels has the colour of its box");
}

// The default names and Person index of a LabelConfig are the default class list of the project.
void checkDefaultsMatchClassMap(Check& check) {
    const LabelConfig config;
    const ClassMap map{defaultClassMap()};
    check.expect(config.classNames == map.names && config.personClass == map.person,
                 "the default labels are the default classes");
}

void checkCapacity(Check& check, const Logger& log) {
    std::optional<Scene> made{makeScene(log, smallCapacity)};
    if (!made) {
        check.expect(false, "scene made");
        return;
    }
    Scene& scene{*made};
    std::vector<OverlayBox> boxes;
    for (int index{0}; index < overCapacityLabels; ++index) {
        boxes.push_back(OverlayBox{cell(Cell{index, 0}), 1, false, testScore, -1});
    }
    check.expect(scene.labels.draw(scene.handle, boxes), "draw above the capacity succeeds");
    check.expect(scene.labels.overflowCount() == 2, "labels above the capacity are counted");
}
} // namespace

int main() {
    const Logger log{"TEST_LABELS", LogLevel::Debug};
    Check check{log};
    checkConfig(check, log);
    checkColours(check, log);
    checkEdgesAndOrder(check, log);
    checkMany(check, log);
    checkCapacity(check, log);
    checkDefaultsMatchClassMap(check);
    if (check.ok()) {
        log.info("PASS");
        return EXIT_SUCCESS;
    }
    log.error("FAIL");
    return EXIT_FAILURE;
}
