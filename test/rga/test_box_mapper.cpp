// BoxMapper against rk3588_common.model_io.postprocess: the table holds, for several source sizes
// and model boxes, the source box that ((x - pad) / gain).clip(0, size) gives. The destination
// box is that box scaled to the destination image. Exit code 0 when every row matches.
#include <array>
#include <cmath>
#include <cstdlib>
#include <optional>

#include "box_mapper.h"
#include "letterbox_layout.h"
#include "logger.h"

namespace {
constexpr int modelSize{640};
constexpr double tolerance{1e-4};
constexpr float pixelTolerance{1e-2F};
constexpr ImageSize destinationSize{3840, 2160};
constexpr ImageSize tallDestination{2160, 3840};

struct Row {
    int width{0};
    int height{0};
    PixelBox model{};
    std::array<double, 4> source{}; // left, top, right, bottom
};

// Generated with rk3588_common.model_io.preprocess (gain, pad) and the clip of postprocess.
constexpr std::array<Row, 20> rows{{
    {640, 360, {100.0F, 240.0F, 200.0F, 340.0F}, {100.0, 100.0, 200.0, 200.0}},
    {640, 360, {0.0F, 0.0F, 640.0F, 640.0F}, {0.0, 0.0, 640.0, 360.0}},
    {640, 360, {320.5F, 320.25F, 400.75F, 500.125F}, {320.5, 180.25, 400.75, 360.0}},
    {640, 360, {10.0F, 5.0F, 300.0F, 150.0F}, {10.0, 0.0, 300.0, 10.0}},
    {1920, 1080, {100.0F, 240.0F, 200.0F, 340.0F}, {300.0, 300.0, 600.0, 600.0}},
    {1920, 1080, {0.0F, 0.0F, 640.0F, 640.0F}, {0.0, 0.0, 1920.0, 1080.0}},
    {1920, 1080, {320.5F, 320.25F, 400.75F, 500.125F}, {961.5, 540.75, 1202.25, 1080.0}},
    {1920, 1080, {10.0F, 5.0F, 300.0F, 150.0F}, {30.0, 0.0, 900.0, 30.0}},
    {1000, 333, {100.0F, 240.0F, 200.0F, 340.0F}, {156.25, 42.1875, 312.5, 198.4375}},
    {1000, 333, {0.0F, 0.0F, 640.0F, 640.0F}, {0.0, 0.0, 1000.0, 333.0}},
    {1000, 333, {320.5F, 320.25F, 400.75F, 500.125F}, {500.78125, 167.578125, 626.171875, 333.0}},
    {1000, 333, {10.0F, 5.0F, 300.0F, 150.0F}, {15.625, 0.0, 468.75, 0.0}},
    {641, 360, {100.0F, 240.0F, 200.0F, 340.0F}, {100.15625, 100.15625, 200.3125, 200.3125}},
    {641, 360, {0.0F, 0.0F, 640.0F, 640.0F}, {0.0, 0.0, 641.0, 360.0}},
    {641,
     360,
     {320.5F, 320.25F, 400.75F, 500.125F},
     {321.00078125, 180.531640625, 401.376171875, 360.0}},
    {641, 360, {10.0F, 5.0F, 300.0F, 150.0F}, {10.015625, 0.0, 300.46875, 10.015625}},
    {360, 640, {100.0F, 240.0F, 200.0F, 340.0F}, {0.0, 240.0, 60.0, 340.0}},
    {360, 640, {0.0F, 0.0F, 640.0F, 640.0F}, {0.0, 0.0, 360.0, 640.0}},
    {360, 640, {320.5F, 320.25F, 400.75F, 500.125F}, {180.5, 320.25, 260.75, 500.125}},
    {360, 640, {10.0F, 5.0F, 300.0F, 150.0F}, {0.0, 5.0, 160.0, 150.0}},
}};

bool close(float actual, double expected, double limit) {
    return std::fabs(static_cast<double>(actual) - expected) <= limit;
}

bool checkRow(const Row& row, const Logger& log) {
    const std::optional<LetterboxLayout> layout{
        letterboxLayout(ImageSize{row.width, row.height}, modelSize)};
    const std::optional<BoxMapper> mapper{layout ? BoxMapper::create(*layout, destinationSize)
                                                 : std::nullopt};
    if (!mapper) {
        log.error(row.width, "x", row.height, ": no mapper");
        return false;
    }
    const PixelBox source{mapper->toSource(row.model)};
    const bool sourceOk{close(source.left, row.source[0], tolerance) &&
                        close(source.top, row.source[1], tolerance) &&
                        close(source.right, row.source[2], tolerance) &&
                        close(source.bottom, row.source[3], tolerance)};
    const double scaleX{static_cast<double>(destinationSize.width) / row.width};
    const double scaleY{static_cast<double>(destinationSize.height) / row.height};
    const PixelBox destination{mapper->toDestination(row.model)};
    const bool destinationOk{close(destination.left, row.source[0] * scaleX, pixelTolerance) &&
                             close(destination.top, row.source[1] * scaleY, pixelTolerance) &&
                             close(destination.right, row.source[2] * scaleX, pixelTolerance) &&
                             close(destination.bottom, row.source[3] * scaleY, pixelTolerance)};
    if (!sourceOk || !destinationOk) {
        log.error(row.width, "x", row.height, " box ", row.model.left, ",", row.model.top, ",",
                  row.model.right, ",", row.model.bottom, ": source ", source.left, ",", source.top,
                  ",", source.right, ",", source.bottom, " expected ", row.source[0], ",",
                  row.source[1], ",", row.source[2], ",", row.source[3]);
    }
    return sourceOk && destinationOk;
}

// A quarter turn: the layout of the rotated source (360 wide) maps to a tall destination image.
bool checkTall(const Logger& log) {
    const std::optional<LetterboxLayout> layout{letterboxLayout(ImageSize{360, 640}, modelSize)};
    const std::optional<BoxMapper> mapper{layout ? BoxMapper::create(*layout, tallDestination)
                                                 : std::nullopt};
    if (!mapper) {
        return false;
    }
    const PixelBox box{mapper->toDestination(PixelBox{180.0F, 320.0F, 260.0F, 400.0F})};
    constexpr double scale{6.0};
    const bool matches{close(box.left, 40.0 * scale, pixelTolerance) &&
                       close(box.top, 320.0 * scale, pixelTolerance) &&
                       close(box.right, 120.0 * scale, pixelTolerance) &&
                       close(box.bottom, 400.0 * scale, pixelTolerance)};
    if (!matches) {
        log.error("tall destination: ", box.left, ",", box.top, ",", box.right, ",", box.bottom);
    }
    return matches;
}

bool checkRefusals(const Logger& log) {
    const std::optional<LetterboxLayout> layout{letterboxLayout(ImageSize{640, 360}, modelSize)};
    if (!layout) {
        return false;
    }
    LetterboxLayout noSource{*layout};
    noSource.source = ImageSize{};
    LetterboxLayout noGain{*layout};
    noGain.gain = 0.0;
    const bool refused{!BoxMapper::create(noSource, destinationSize) &&
                       !BoxMapper::create(noGain, destinationSize) &&
                       !BoxMapper::create(*layout, ImageSize{0, 2160}) &&
                       !BoxMapper::create(*layout, ImageSize{3840, -1})};
    if (!refused) {
        log.error("a bad argument was accepted");
    }
    return refused;
}
} // namespace

int main() {
    const Logger log{"TEST_MAPPER", LogLevel::Debug};
    bool passed{true};
    for (const Row& row : rows) {
        passed = checkRow(row, log) && passed;
    }
    passed = checkTall(log) && passed;
    passed = checkRefusals(log) && passed;
    if (passed) {
        log.info("PASS: ", rows.size(), " rows");
        return EXIT_SUCCESS;
    }
    log.error("FAIL");
    return EXIT_FAILURE;
}
