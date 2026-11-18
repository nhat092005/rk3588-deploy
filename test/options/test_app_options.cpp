// parseOptions: defaults, every flag, the ranges, and the failures (unknown flag, missing value,
// missing --model, bad values). Needs only the command line, but links the board libraries whose
// constants are the defaults. Exit code 0 when every check passes.
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string_view>
#include <vector>

#include "app_options.h"
#include "logger.h"

namespace {
constexpr float confidenceTolerance{1e-6F};
// The values of the every-flag test, as numbers and as the text on the command line.
constexpr int customWidth{1280};
constexpr int customHeight{720};
constexpr int customKbps{8000};
constexpr int customQuality{75};
constexpr double customHold{2.5};
constexpr double customGap{0.25};
constexpr float customConfidence{0.4F};
constexpr std::int64_t customFrames{300};
constexpr double customStats{5.0};
constexpr int repeatedWidth{640};
constexpr double customStorage{1.5};
constexpr int customDays{7};
constexpr double customSegment{30.0};
constexpr double customRecordStorage{2.5};
constexpr int customRecordDays{3};
constexpr double customPre{2.5};
constexpr double customPost{7.0};
constexpr double realTolerance{1e-9};

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

bool near(double left, double right) {
    return left - right < realTolerance && right - left < realTolerance;
}

// The command line `program` followed by `rest`, parsed with a logger that stays quiet.
ParseResult parse(const std::vector<const char*>& rest) {
    const Logger quiet{"OPTIONS", LogLevel::None};
    std::vector<const char*> args{"program"};
    args.insert(args.end(), rest.begin(), rest.end());
    return parseOptions(args, quiet);
}

void checkDefaults(Check& check) {
    const ParseResult result{parse({"--model", "m.rknn"})};
    check.expect(result.status == ParseStatus::Run, "a model alone is enough");
    const AppOptions& options{result.options};
    check.expect(options.modelPath == "m.rknn", "the model path");
    check.expect(options.cameraNode == defaultCameraNode, "default node");
    check.expect(options.width == defaultCameraWidth && options.height == defaultCameraHeight,
                 "default size");
    check.expect(options.rotation == Rotation::None, "default rotation");
    check.expect(options.outputDir == defaultViolationRoot, "default output folder");
    check.expect(options.videoKbps == defaultVideoKbps, "default bitrate");
    check.expect(options.jpegQuality == defaultJpegQuality, "default quality");
    check.expect(near(options.holdSeconds, defaultHoldSeconds), "default hold");
    check.expect(near(options.gapSeconds, defaultGapSeconds), "default gap");
    check.expect(options.minConfidence > defaultViolationConfidence - confidenceTolerance &&
                     options.minConfidence < defaultViolationConfidence + confidenceTolerance,
                 "default confidence");
    check.expect(near(options.clipPreSeconds, defaultClipPreSeconds) &&
                     near(options.clipPostSeconds, defaultClipPostSeconds),
                 "default clip times");
    check.expect(near(options.maxStorageGb, defaultMaxStorageGb) &&
                     options.keepDays == defaultKeepDays,
                 "default retention");
    check.expect(!options.record && options.recordDir == defaultRecordRoot &&
                     near(options.recordSegmentSeconds, defaultSegmentSeconds) &&
                     near(options.recordMaxStorageGb, defaultRecordMaxStorageGb) &&
                     options.recordKeepDays == defaultRecordKeepDays,
                 "default recording: off, /data/records, 60 s, 5 GB, 7 days");
    check.expect(options.frames == 0, "default: no frame limit");
    check.expect(near(options.statsSeconds, defaultStatsSeconds), "default statistics period");
}

void checkEveryFlag(Check& check) {
    const ParseResult result{parse({"--model",
                                    "a.rknn",
                                    "--node",
                                    "/dev/video0",
                                    "--width",
                                    "1280",
                                    "--height",
                                    "720",
                                    "--rotation",
                                    "180",
                                    "--output-dir",
                                    "/tmp/out",
                                    "--video-kbps",
                                    "8000",
                                    "--jpeg-quality",
                                    "75",
                                    "--hold-seconds",
                                    "2.5",
                                    "--gap-seconds",
                                    "0.25",
                                    "--min-confidence",
                                    "0.4",
                                    "--clip-pre-seconds",
                                    "2.5",
                                    "--clip-post-seconds",
                                    "7",
                                    "--max-storage-gb",
                                    "1.5",
                                    "--keep-days",
                                    "7",
                                    "--record",
                                    "--record-dir",
                                    "/mnt/usb/records",
                                    "--record-segment-seconds",
                                    "30",
                                    "--record-max-storage-gb",
                                    "2.5",
                                    "--record-keep-days",
                                    "3",
                                    "--frames",
                                    "300",
                                    "--stats-seconds",
                                    "5"})};
    check.expect(result.status == ParseStatus::Run, "every flag together");
    const AppOptions& options{result.options};
    check.expect(options.cameraNode == "/dev/video0", "--node");
    check.expect(options.width == customWidth && options.height == customHeight,
                 "--width and --height");
    check.expect(options.rotation == Rotation::Rotate180, "--rotation");
    check.expect(options.outputDir == "/tmp/out", "--output-dir");
    check.expect(options.videoKbps == customKbps, "--video-kbps");
    check.expect(options.jpegQuality == customQuality, "--jpeg-quality");
    check.expect(near(options.holdSeconds, customHold), "--hold-seconds");
    check.expect(near(options.gapSeconds, customGap), "--gap-seconds");
    check.expect(options.minConfidence > customConfidence - confidenceTolerance &&
                     options.minConfidence < customConfidence + confidenceTolerance,
                 "--min-confidence");
    check.expect(near(options.clipPreSeconds, customPre) &&
                     near(options.clipPostSeconds, customPost),
                 "--clip-pre-seconds and --clip-post-seconds");
    check.expect(near(options.maxStorageGb, customStorage) && options.keepDays == customDays,
                 "--max-storage-gb and --keep-days");
    check.expect(options.record && options.recordDir == "/mnt/usb/records" &&
                     near(options.recordSegmentSeconds, customSegment) &&
                     near(options.recordMaxStorageGb, customRecordStorage) &&
                     options.recordKeepDays == customRecordDays,
                 "--record and its flags");
    check.expect(options.frames == customFrames, "--frames");
    check.expect(near(options.statsSeconds, customStats), "--stats-seconds");
}

struct Case {
    std::vector<const char*> args;
    ParseStatus status{ParseStatus::Invalid};
    const char* what{nullptr};
};

// Each of these must be refused, or accepted at its limit.
std::vector<Case> cases() {
    constexpr ParseStatus run{ParseStatus::Run};
    constexpr ParseStatus invalid{ParseStatus::Invalid};
    constexpr ParseStatus help{ParseStatus::Help};
    return {
        {{}, invalid, "no model"},
        {{"--help"}, help, "--help alone"},
        {{"--model", "m", "--help"}, help, "--help after a flag"},
        {{"--model", "m", "--bogus", "1"}, invalid, "unknown flag"},
        {{"--model"}, invalid, "a flag without a value"},
        {{"--model", "m", "--width"}, invalid, "the last flag without a value"},
        {{"--model", ""}, invalid, "an empty model path"},
        {{"--model", "m", "--node", ""}, invalid, "an empty node"},
        {{"--model", "m", "--output-dir", ""}, invalid, "an empty output folder"},
        {{"--model", "m", "--width", "0"}, invalid, "width 0"},
        {{"--model", "m", "--width", "1281"}, invalid, "odd width"},
        {{"--model", "m", "--width", "16386"}, invalid, "width above the limit"},
        {{"--model", "m", "--width", "16384"}, run, "width at the limit"},
        {{"--model", "m", "--width", "12x"}, invalid, "width with trailing text"},
        {{"--model", "m", "--width", "abc"}, invalid, "width that is not a number"},
        {{"--model", "m", "--height", "-2"}, invalid, "negative height"},
        {{"--model", "m", "--height", "719"}, invalid, "odd height"},
        {{"--model", "m", "--rotation", "45"}, invalid, "rotation 45"},
        {{"--model", "m", "--rotation", "270"}, run, "rotation 270"},
        {{"--model", "m", "--video-kbps", "0"}, invalid, "bitrate 0"},
        {{"--model", "m", "--video-kbps", "200001"}, invalid, "bitrate above the limit"},
        {{"--model", "m", "--video-kbps", "200000"}, run, "bitrate at the limit"},
        {{"--model", "m", "--jpeg-quality", "0"}, invalid, "quality 0"},
        {{"--model", "m", "--jpeg-quality", "100"}, invalid, "quality 100"},
        {{"--model", "m", "--jpeg-quality", "1"}, run, "quality 1"},
        {{"--model", "m", "--jpeg-quality", "99"}, run, "quality 99"},
        {{"--model", "m", "--hold-seconds", "0"}, invalid, "hold 0"},
        {{"--model", "m", "--hold-seconds", "-1"}, invalid, "negative hold"},
        {{"--model", "m", "--hold-seconds", "nan"}, invalid, "hold nan"},
        {{"--model", "m", "--hold-seconds", "inf"}, invalid, "hold inf"},
        {{"--model", "m", "--hold-seconds", "1.5x"}, invalid, "hold with trailing text"},
        {{"--model", "m", "--hold-seconds", "abc"}, invalid, "hold that is not a number"},
        {{"--model", "m", "--hold-seconds", "0.001"}, run, "a tiny hold"},
        {{"--model", "m", "--gap-seconds", "-0.1"}, invalid, "negative gap"},
        {{"--model", "m", "--gap-seconds", "0"}, run, "gap 0"},
        {{"--model", "m", "--min-confidence", "1.5"}, invalid, "confidence above 1"},
        {{"--model", "m", "--min-confidence", "-0.1"}, invalid, "negative confidence"},
        {{"--model", "m", "--min-confidence", "0"}, run, "confidence 0"},
        {{"--model", "m", "--min-confidence", "1"}, run, "confidence 1"},
        {{"--model", "m", "--clip-pre-seconds", "-1"}, invalid, "negative pre time"},
        {{"--model", "m", "--clip-pre-seconds", "61"}, invalid, "pre time above the limit"},
        {{"--model", "m", "--clip-pre-seconds", "60"}, run, "pre time at the limit"},
        {{"--model", "m", "--clip-pre-seconds", "0", "--clip-post-seconds", "0"}, run, "no clip"},
        {{"--model", "m", "--clip-post-seconds", "-0.5"}, invalid, "negative post time"},
        {{"--model", "m", "--clip-post-seconds", "60.5"}, invalid, "post time above the limit"},
        {{"--model", "m", "--clip-post-seconds", "x"}, invalid, "post time that is not a number"},
        {{"--model", "m", "--max-storage-gb", "0"}, invalid, "storage 0"},
        {{"--model", "m", "--max-storage-gb", "-1"}, invalid, "negative storage"},
        {{"--model", "m", "--max-storage-gb", "100001"}, invalid, "storage above the limit"},
        {{"--model", "m", "--max-storage-gb", "0.01"}, run, "a small storage"},
        {{"--model", "m", "--max-storage-gb", "inf"}, invalid, "storage inf"},
        {{"--model", "m", "--keep-days", "0"}, invalid, "keep 0 days"},
        {{"--model", "m", "--keep-days", "3651"}, invalid, "keep above the limit"},
        {{"--model", "m", "--keep-days", "3650"}, run, "keep at the limit"},
        {{"--model", "m", "--keep-days", "1.5"}, invalid, "a fraction of a day"},
        {{"--model", "m", "--record"}, run, "--record alone, last"},
        {{"--record", "--model", "m"}, run, "--record first"},
        {{"--model", "m", "--record", "--frames", "5"}, run, "--record between flags"},
        {{"--model", "m", "--record", "1"}, invalid, "--record takes no value"},
        {{"--model", "m", "--record-dir", ""}, invalid, "an empty record folder"},
        {{"--model", "m", "--record-segment-seconds", "0"}, invalid, "segment 0"},
        {{"--model", "m", "--record-segment-seconds", "3601"}, invalid, "segment above the limit"},
        {{"--model", "m", "--record-segment-seconds", "3600"}, run, "segment at the limit"},
        {{"--model", "m", "--record-segment-seconds", "0.5"}, run, "half a second segment"},
        {{"--model", "m", "--record-max-storage-gb", "0"}, invalid, "record storage 0"},
        {{"--model", "m", "--record-max-storage-gb", "100001"},
         invalid,
         "record storage above the limit"},
        {{"--model", "m", "--record-keep-days", "0"}, invalid, "record keep 0 days"},
        {{"--model", "m", "--record-keep-days", "3651"}, invalid, "record keep above the limit"},
        {{"--model", "m", "--frames", "-1"}, invalid, "negative frame count"},
        {{"--model", "m", "--frames", "0"}, run, "frame count 0"},
        {{"--model", "m", "--stats-seconds", "0"}, invalid, "statistics period 0"},
        {{"--model", "m", "--stats-seconds", "0.5"}, run, "statistics period 0.5"},
    };
}

void checkCases(Check& check) {
    for (const Case& item : cases()) {
        check.expect(parse(item.args).status == item.status, item.what);
    }
    const ParseResult twice{parse({"--model", "m", "--width", "1280", "--width", "640"})};
    check.expect(twice.status == ParseStatus::Run && twice.options.width == repeatedWidth,
                 "a flag given twice keeps the last value");
}
} // namespace

int main() {
    const Logger log{"TEST_OPTIONS", LogLevel::Debug};
    Check check{log};
    checkDefaults(check);
    checkEveryFlag(check);
    checkCases(check);
    if (check.ok()) {
        log.info("PASS");
        return EXIT_SUCCESS;
    }
    log.error("FAIL");
    return EXIT_FAILURE;
}
