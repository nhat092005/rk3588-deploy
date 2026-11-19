// planStreams, scaledWidth, scaledKbps and downscaleProblem: which streams the options ask for and
// how big they are.
#include <cstdlib>
#include <string>
#include <vector>

#include "logger.h"
#include "output_plan.h"

namespace {
constexpr int fullWidth{3840};
constexpr int fullHeight{2160};
constexpr int heightHd{1080};
constexpr int heightSmall{720};
constexpr int heightTiny{240};
constexpr int widthHd{1920};
constexpr int widthSmall{1280};
constexpr int widthTiny{426};
constexpr int portraitWidth{1080};
constexpr int kbpsHd{5000};
constexpr int kbpsSmall{2222};
constexpr int kbpsFull{20'000};
constexpr std::size_t twoStreams{2};
constexpr std::size_t threeStreams{3};

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

void checkPlan(Check& check) {
    AppOptions options;
    const std::vector<StreamPlan> idle{planStreams(options, false, false, false)};
    check.expect(idle.size() == 1 && idle[0].height == 0 && idle[0].clip && !idle[0].archive,
                 "the clip alone is one full size stream");

    options.violation = false;
    check.expect(planStreams(options, false, false, false).empty(),
                 "nothing reads a stream: no stream is encoded");
    const std::vector<StreamPlan> viewOnly{planStreams(options, false, false, true)};
    check.expect(viewOnly.size() == 1 && viewOnly[0].stream && viewOnly[0].height == 0,
                 "the live stream is a full size stream");

    options = AppOptions{};
    options.archiveDownscale = true;
    options.sessionDownscale = true;
    const std::vector<StreamPlan> shared{planStreams(options, true, true, true)};
    check.expect(shared.size() == twoStreams && shared[0].height == 0 && shared[0].stream &&
                     shared[0].clip && shared[1].height == defaultVideoDownscaleHeight &&
                     shared[1].archive && shared[1].session,
                 "archive and session at the same height share one stream, after the full one");

    options.sessionHeight = heightSmall;
    options.violationClipDownscale = true;
    const std::vector<StreamPlan> three{planStreams(options, true, true, true)};
    check.expect(three.size() == threeStreams && three[0].height == 0 &&
                     three[1].height == heightHd && three[1].archive && three[1].clip &&
                     three[2].height == heightSmall && three[2].session,
                 "three heights: the full one first, then the larger");

    options.archiveDownscale = false;
    options.sessionDownscale = false;
    options.violationClipDownscale = false;
    const std::vector<StreamPlan> allFull{planStreams(options, true, true, false)};
    check.expect(allFull.size() == 1 && allFull[0].archive && allFull[0].session && allFull[0].clip,
                 "outputs that are not scaled all read the full size stream");

    options.archiveDownscale = true;
    options.sessionDownscale = true;
    options.violationClipDownscale = true;
    const std::vector<StreamPlan> noFull{planStreams(options, true, true, false)};
    check.expect(noFull.size() == twoStreams && noFull[0].height == heightHd &&
                     noFull[1].height == heightSmall,
                 "no full size stream when every output is scaled and there is no live view");

    options.sessionHeight = defaultVideoDownscaleHeight;
    check.expect(planStreams(options, true, false, false).size() == 1 &&
                     !planStreams(options, false, false, false)[0].archive,
                 "an output that is off takes no part");
}

void checkSizes(Check& check) {
    const ImageSize portrait{fullHeight, fullWidth};
    check.expect(scaledWidth(ImageSize{fullWidth, fullHeight}, heightHd) == widthHd,
                 "3840x2160 to 1080");
    check.expect(scaledWidth(ImageSize{fullWidth, fullHeight}, heightSmall) == widthSmall,
                 "3840x2160 to 720");
    check.expect(scaledWidth(ImageSize{fullWidth, fullHeight}, heightTiny) == widthTiny,
                 "3840x2160 to 240 is rounded down to an even width");
    check.expect(scaledWidth(portrait, widthHd) == portraitWidth,
                 "a portrait picture keeps its proportions");
    check.expect(scaledKbps(kbpsFull, ImageSize{fullWidth, fullHeight}, heightHd) == kbpsHd,
                 "a quarter of the pixels gets a quarter of the bitrate");
    check.expect(scaledKbps(kbpsFull, ImageSize{fullWidth, fullHeight}, heightSmall) == kbpsSmall,
                 "720 gets a ninth of the bitrate");
    check.expect(scaledKbps(kbpsFull, ImageSize{fullWidth, fullHeight}, heightTiny) ==
                     minScaledKbps,
                 "a tiny stream is held at the lowest bitrate");
    check.expect(scaledKbps(minScaledKbps - 1, ImageSize{fullWidth, fullHeight}, heightHd) ==
                     minScaledKbps - 1,
                 "the bitrate of a scaled stream never exceeds the full one");
}

void checkProblem(Check& check) {
    AppOptions options;
    check.expect(downscaleProblem(options, fullHeight, true, true).empty(), "nothing is scaled");
    options.violationImageDownscale = true;
    options.violationImageHeight = fullHeight;
    check.expect(downscaleProblem(options, fullHeight, false, false).find("violation.image") !=
                     std::string::npos,
                 "an image as high as the camera is refused");
    options.violationImageHeight = heightHd;
    check.expect(downscaleProblem(options, fullHeight, false, false).empty(), "a smaller image");
    options.archiveDownscale = true;
    options.archiveHeight = fullHeight + 2;
    check.expect(downscaleProblem(options, fullHeight, false, false).empty(),
                 "the archive is off: its height is not looked at");
    check.expect(downscaleProblem(options, fullHeight, true, false).find("archive") !=
                     std::string::npos,
                 "the archive is on and its height is above the camera");
    options.archiveDownscale = false;
    options.sessionDownscale = true;
    options.sessionHeight = fullHeight;
    check.expect(downscaleProblem(options, fullHeight, true, true).find("session") !=
                     std::string::npos,
                 "the session height is refused");
    check.expect(downscaleProblem(options, heightHd + 2, true, false).empty(),
                 "a smaller camera picture: only outputs that are on count");
    options.violation = false;
    options.violationImageDownscale = true;
    options.violationImageHeight = fullHeight;
    check.expect(downscaleProblem(options, fullHeight, false, false).empty(),
                 "no violations stored: image and clip are not looked at");
}
} // namespace

int main() {
    const Logger log{"TEST_OUTPUT_PLAN", LogLevel::Debug};
    Check check{log};
    checkPlan(check);
    checkSizes(check);
    checkProblem(check);
    if (check.ok()) {
        log.info("PASS");
        return EXIT_SUCCESS;
    }
    log.error("FAIL");
    return EXIT_FAILURE;
}
