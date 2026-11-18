// letterboxLayout against rk3588_common.model_io.preprocess. The table comes from that function
// (width, height, scaled width, scaled height, left, top, gain) and is checked on the PC and the
// board. Exit code 0 when every row matches.
#include <array>
#include <cmath>
#include <cstdlib>

#include "letterbox_layout.h"
#include "logger.h"

namespace {
constexpr int modelSize{640};
constexpr double gainTolerance{1e-12};
constexpr int gainPrecision{12};

struct Row {
    int width;
    int height;
    int scaledWidth;
    int scaledHeight;
    int left;
    int top;
    double gain;
};

constexpr std::array<Row, 16> rows{{
    {640, 360, 640, 360, 0, 140, 1.0},
    {1920, 1080, 640, 360, 0, 140, 0.3333333333333333},
    {3840, 2160, 640, 360, 0, 140, 0.16666666666666666},
    {1280, 720, 640, 360, 0, 140, 0.5},
    {640, 640, 640, 640, 0, 0, 1.0},
    {480, 640, 480, 640, 80, 0, 1.0},
    {1000, 333, 640, 214, 0, 213, 0.64},
    {641, 360, 640, 360, 0, 140, 0.9984399375975039},
    {639, 479, 640, 480, 0, 80, 1.001564945226917},
    {320, 240, 640, 480, 0, 80, 2.0},
    {2000, 2000, 640, 640, 0, 0, 0.32},
    {4224, 2376, 640, 360, 0, 140, 0.15151515151515152},
    {360, 640, 360, 640, 140, 0, 1.0},
    {640, 361, 640, 361, 0, 139, 1.0},
    {641, 640, 640, 640, 0, 0, 0.9984399375975039},
    {500, 641, 500, 640, 70, 0, 0.9984399375975039},
}};

bool checkRow(const Row& row, const Logger& log) {
    const std::optional<LetterboxLayout> layout{
        letterboxLayout(ImageSize{row.width, row.height}, modelSize)};
    if (!layout) {
        log.error(row.width, "x", row.height, ": no layout");
        return false;
    }
    const bool same{layout->scaled.width == row.scaledWidth &&
                    layout->scaled.height == row.scaledHeight && layout->left == row.left &&
                    layout->top == row.top && std::fabs(layout->gain - row.gain) < gainTolerance};
    if (!same) {
        log.error(row.width, "x", row.height, ": got ", layout->scaled.width, "x",
                  layout->scaled.height, " left=", layout->left, " top=", layout->top,
                  " gain=", Fixed{layout->gain, gainPrecision}, ", expected ", row.scaledWidth, "x",
                  row.scaledHeight, " left=", row.left, " top=", row.top,
                  " gain=", Fixed{row.gain, gainPrecision});
    }
    return same;
}

bool checkBadInput(const Logger& log) {
    const bool rejected{!letterboxLayout(ImageSize{0, 360}, modelSize) &&
                        !letterboxLayout(ImageSize{640, -1}, modelSize) &&
                        !letterboxLayout(ImageSize{640, 360}, 0)};
    if (!rejected) {
        log.error("a bad argument was accepted");
    }
    return rejected;
}
} // namespace

int main() {
    const Logger log{"TEST_LETTERBOX", LogLevel::Debug};
    bool passed{checkBadInput(log)};
    for (const Row& row : rows) {
        passed = checkRow(row, log) && passed;
    }
    if (passed) {
        log.info("PASS");
    } else {
        log.error("FAIL");
    }
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
