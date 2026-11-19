// parseOptions: defaults, every flag, the ranges, and the failures (unknown flag, missing value,
// missing --model, bad values), and the config file (keys, order, paths, refusals). Needs only the
// command line, but links the board libraries whose constants are the defaults. Exit code 0 when
// every check passes.
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
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
constexpr double expectedMinFree{2.0};
constexpr double expectedArchiveMinFree{3.0};
constexpr double customMinFree{1.0};
constexpr double expectedSessionMinFree{3.0};
constexpr double customSessionMinFree{0.5};
constexpr double customArchiveMinFree{4.5};
constexpr double customSegment{30.0};
constexpr double customArchiveStorage{2.5};
constexpr int customArchiveDays{3};
constexpr double customPre{2.5};
constexpr double customPost{7.0};
constexpr double fileClipPost{8.0};
constexpr double realTolerance{1e-9};
constexpr int otherWidth{1000};
constexpr std::size_t oversizedConfigBytes{71680U};

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
    return parseOptions(args, "", quiet);
}

namespace fs = std::filesystem;

// The same, with the application folder `baseDir` (it decides where the default config file is).
ParseResult parseIn(const std::string& baseDir, const std::vector<const char*>& rest) {
    const Logger quiet{"OPTIONS", LogLevel::None};
    std::vector<const char*> args{"program"};
    args.insert(args.end(), rest.begin(), rest.end());
    return parseOptions(args, baseDir, quiet);
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
    check.expect(options.violation && options.violationImage && options.violationClip,
                 "default violations: stored with images and clip");
    check.expect(near(options.maxStorageGb, defaultMaxStorageGb) &&
                     options.keepDays == defaultKeepDays,
                 "default retention");
    check.expect(near(options.minFreeGb, expectedMinFree) &&
                     near(options.archiveMinFreeGb, expectedArchiveMinFree),
                 "default free-space floors: 2 GB violations, 3 GB archive");
    check.expect(!options.archive && options.archiveDir == defaultArchiveRoot &&
                     near(options.archiveSegmentSeconds, defaultSegmentSeconds) &&
                     near(options.archiveMaxStorageGb, defaultArchiveMaxStorageGb) &&
                     options.archiveKeepDays == defaultArchiveKeepDays,
                 "default archive: off, /data/archive, 60 s, 5 GB, 7 days");
    check.expect(!options.session && options.sessionDir == defaultSessionRoot &&
                     near(options.sessionMinFreeGb, expectedSessionMinFree),
                 "default session: off, /data/sessions, 3 GB floor");
    check.expect(!options.stream && options.streamUrl == defaultStreamUrl,
                 "default stream: off, rtsp://127.0.0.1:8554/live");
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
                                    "--violation",
                                    "false",
                                    "--violation-output-dir",
                                    "/tmp/out",
                                    "--violation-image",
                                    "false",
                                    "--violation-clip",
                                    "false",
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
                                    "--violation-clip-pre-seconds",
                                    "2.5",
                                    "--violation-clip-post-seconds",
                                    "7",
                                    "--max-storage-gb",
                                    "1.5",
                                    "--keep-days",
                                    "7",
                                    "--violation-min-free-gb",
                                    "1",
                                    "--archive",
                                    "true",
                                    "--archive-output-dir",
                                    "/mnt/usb/archive",
                                    "--archive-segment-seconds",
                                    "30",
                                    "--archive-max-storage-gb",
                                    "2.5",
                                    "--archive-keep-days",
                                    "3",
                                    "--archive-min-free-gb",
                                    "4.5",
                                    "--session",
                                    "true",
                                    "--session-output-dir",
                                    "/mnt/usb/sessions",
                                    "--session-min-free-gb",
                                    "0.5",
                                    "--stream",
                                    "true",
                                    "--stream-url",
                                    "rtsp://10.0.0.2:8554/cam",
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
    check.expect(options.outputDir == "/tmp/out", "--violation-output-dir");
    check.expect(!options.violation && !options.violationImage && !options.violationClip,
                 "--violation, --violation-image and --violation-clip");
    check.expect(options.videoKbps == customKbps, "--video-kbps");
    check.expect(options.jpegQuality == customQuality, "--jpeg-quality");
    check.expect(near(options.holdSeconds, customHold), "--hold-seconds");
    check.expect(near(options.gapSeconds, customGap), "--gap-seconds");
    check.expect(options.minConfidence > customConfidence - confidenceTolerance &&
                     options.minConfidence < customConfidence + confidenceTolerance,
                 "--min-confidence");
    check.expect(near(options.clipPreSeconds, customPre) &&
                     near(options.clipPostSeconds, customPost),
                 "--violation-clip-pre-seconds and --violation-clip-post-seconds");
    check.expect(near(options.maxStorageGb, customStorage) && options.keepDays == customDays,
                 "--max-storage-gb and --keep-days");
    check.expect(near(options.minFreeGb, customMinFree) &&
                     near(options.archiveMinFreeGb, customArchiveMinFree),
                 "--violation-min-free-gb and --archive-min-free-gb");
    check.expect(options.archive && options.archiveDir == "/mnt/usb/archive" &&
                     near(options.archiveSegmentSeconds, customSegment) &&
                     near(options.archiveMaxStorageGb, customArchiveStorage) &&
                     options.archiveKeepDays == customArchiveDays,
                 "--archive and its flags");
    check.expect(options.session && options.sessionDir == "/mnt/usb/sessions" &&
                     near(options.sessionMinFreeGb, customSessionMinFree),
                 "--session and its flags");
    check.expect(options.stream && options.streamUrl == "rtsp://10.0.0.2:8554/cam",
                 "--stream and --stream-url");
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
        {{"--model", "m", "--violation-output-dir", ""}, invalid, "an empty output folder"},
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
        {{"--model", "m", "--violation-clip-pre-seconds", "-1"}, invalid, "negative pre time"},
        {{"--model", "m", "--violation-clip-pre-seconds", "61"}, invalid, "pre time above limit"},
        {{"--model", "m", "--violation-clip-pre-seconds", "60"}, run, "pre time at the limit"},
        {{"--model", "m", "--violation-clip-pre-seconds", "0", "--violation-clip-post-seconds",
          "0"},
         invalid,
         "a clip that is on with no time"},
        {{"--model", "m", "--violation-clip-pre-seconds", "0"}, run, "pre 0, post stays 5"},
        {{"--model", "m", "--violation-clip", "false", "--violation-clip-pre-seconds", "0",
          "--violation-clip-post-seconds", "0"},
         run,
         "no clip: off, with no time"},
        {{"--model", "m", "--violation", "false", "--violation-clip-pre-seconds", "0",
          "--violation-clip-post-seconds", "0"},
         run,
         "no violations stored: the clip times do not matter"},
        {{"--model", "m", "--violation-clip-post-seconds", "-0.5"}, invalid, "negative post time"},
        {{"--model", "m", "--violation-clip-post-seconds", "60.5"}, invalid, "post above limit"},
        {{"--model", "m", "--violation-clip-post-seconds", "x"}, invalid, "post time not a number"},
        {{"--model", "m", "--clip-pre-seconds", "5"}, invalid, "the old --clip-pre-seconds"},
        {{"--model", "m", "--clip-post-seconds", "5"}, invalid, "the old --clip-post-seconds"},
        {{"--model", "m", "--violation", "true"}, run, "--violation true"},
        {{"--model", "m", "--violation", "false"}, run, "--violation false"},
        {{"--model", "m", "--violation", "1"}, invalid, "--violation 1"},
        {{"--model", "m", "--violation", "TRUE"}, invalid, "--violation in capitals"},
        {{"--model", "m", "--violation"}, invalid, "--violation without a value"},
        {{"--model", "m", "--violation-image", "false"}, run, "--violation-image false"},
        {{"--model", "m", "--violation-image", "yes"}, invalid, "--violation-image yes"},
        {{"--model", "m", "--violation-clip", "false"}, run, "--violation-clip false"},
        {{"--model", "m", "--violation-clip", ""}, invalid, "--violation-clip empty"},
        {{"--model", "m", "--max-storage-gb", "0"}, invalid, "storage 0"},
        {{"--model", "m", "--max-storage-gb", "-1"}, invalid, "negative storage"},
        {{"--model", "m", "--max-storage-gb", "100001"}, invalid, "storage above the limit"},
        {{"--model", "m", "--max-storage-gb", "0.01"}, run, "a small storage"},
        {{"--model", "m", "--max-storage-gb", "inf"}, invalid, "storage inf"},
        {{"--model", "m", "--keep-days", "0"}, invalid, "keep 0 days"},
        {{"--model", "m", "--keep-days", "3651"}, invalid, "keep above the limit"},
        {{"--model", "m", "--keep-days", "3650"}, run, "keep at the limit"},
        {{"--model", "m", "--keep-days", "1.5"}, invalid, "a fraction of a day"},
        {{"--model", "m", "--archive", "true"}, run, "--archive true"},
        {{"--model", "m", "--archive", "false"}, run, "--archive false"},
        {{"--model", "m", "--archive"}, invalid, "--archive without a value"},
        {{"--archive", "--model", "m"}, invalid, "--archive followed by a flag"},
        {{"--model", "m", "--archive", "1"}, invalid, "--archive 1"},
        {{"--model", "m", "--archive", "yes"}, invalid, "--archive yes"},
        {{"--model", "m", "--archive", "TRUE"}, invalid, "--archive in capitals"},
        {{"--model", "m", "--archive", ""}, invalid, "--archive with an empty value"},
        {{"--model", "m", "--output-dir", "/x"}, invalid, "the old --output-dir"},
        {{"--model", "m", "--stream", "true"}, run, "--stream true"},
        {{"--model", "m", "--stream", "false"}, run, "--stream false"},
        {{"--model", "m", "--stream"}, invalid, "--stream without a value"},
        {{"--model", "m", "--stream", "1"}, invalid, "--stream 1"},
        {{"--model", "m", "--stream", "TRUE"}, invalid, "--stream in capitals"},
        {{"--model", "m", "--stream-url", ""}, invalid, "an empty stream address"},
        {{"--model", "m", "--record-dir", "/x"}, invalid, "the old --record-dir"},
        {{"--model", "m", "--record", "true"}, invalid, "the old --record flag"},
        {{"--model", "m", "--archive-output-dir", ""}, invalid, "an empty archive folder"},
        {{"--model", "m", "--archive-segment-seconds", "0"}, invalid, "segment 0"},
        {{"--model", "m", "--archive-segment-seconds", "3601"}, invalid, "segment above the limit"},
        {{"--model", "m", "--archive-segment-seconds", "3600"}, run, "segment at the limit"},
        {{"--model", "m", "--archive-segment-seconds", "0.5"}, run, "half a second segment"},
        {{"--model", "m", "--archive-max-storage-gb", "0"}, invalid, "archive storage 0"},
        {{"--model", "m", "--archive-max-storage-gb", "100001"},
         invalid,
         "archive storage above the limit"},
        {{"--model", "m", "--archive-keep-days", "0"}, invalid, "archive keep 0 days"},
        {{"--model", "m", "--archive-keep-days", "3651"}, invalid, "archive keep above the limit"},
        {{"--model", "m", "--violation-min-free-gb", "-1"}, invalid, "violation floor below 0"},
        {{"--model", "m", "--violation-min-free-gb", "100001"}, invalid, "violation floor above"},
        {{"--model", "m", "--violation-min-free-gb", "0"}, run, "violation floor 0 (off)"},
        {{"--model", "m", "--violation-min-free-gb", "abc"}, invalid, "violation floor text"},
        {{"--model", "m", "--archive-min-free-gb", "-0.5"}, invalid, "archive floor below 0"},
        {{"--model", "m", "--archive-min-free-gb", "100001"}, invalid, "archive floor above"},
        {{"--model", "m", "--archive-min-free-gb", "0"}, run, "archive floor 0 (off)"},
        {{"--model", "m", "--session", "true"}, run, "--session true"},
        {{"--model", "m", "--session", "false"}, run, "--session false"},
        {{"--model", "m", "--session"}, invalid, "--session without a value"},
        {{"--session", "--model", "m"}, invalid, "--session followed by a flag"},
        {{"--model", "m", "--session", "1"}, invalid, "--session 1"},
        {{"--model", "m", "--session", "TRUE"}, invalid, "--session in capitals"},
        {{"--model", "m", "--session-output-dir", ""}, invalid, "an empty session folder"},
        {{"--model", "m", "--session-min-free-gb", "-1"}, invalid, "session floor below 0"},
        {{"--model", "m", "--session-min-free-gb", "100001"}, invalid, "session floor above"},
        {{"--model", "m", "--session-min-free-gb", "0"}, run, "session floor 0 (off)"},
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
std::optional<fs::path> makeRoot() {
    std::error_code error;
    std::string pattern{(fs::temp_directory_path(error) / "app_options_XXXXXX").string()};
    if (error || mkdtemp(pattern.data()) == nullptr) {
        return std::nullopt;
    }
    return fs::path{pattern};
}

// Writes `text` as <root>/resource/config/app_config.json and returns the path of the file.
std::string writeDefaultConfig(const fs::path& root, const std::string& text) {
    std::error_code error;
    fs::create_directories(root / "resource" / "config", error);
    const fs::path file{root / "resource" / "config" / "app_config.json"};
    std::ofstream out{file, std::ios::binary};
    out << text;
    return file.string();
}

std::string writeFile(const fs::path& path, const std::string& text) {
    std::ofstream out{path, std::ios::binary};
    out << text;
    return path.string();
}

// Every key of the config file, with the values of the every-flag test.
constexpr const char* everyKeyConfig{R"({
  "model": "/models/a.rknn",
  "camera": {"node": "/dev/video0", "width": 1280, "height": 720, "rotation": 180},
  "encode": {"video_kbps": 8000, "jpeg_quality": 75},
  "rule": {"hold_seconds": 2.5, "gap_seconds": 0.25, "min_confidence": 0.4},
  "violation": {
    "enabled": false,
    "output_dir": "/tmp/out",
    "max_storage_gb": 1.5,
    "keep_days": 7,
    "min_free_gb": 1,
    "image": {"enabled": false},
    "clip": {"enabled": false, "pre_seconds": 2.5, "post_seconds": 7}
  },
  "archive": {
    "enabled": true,
    "output_dir": "/tmp/rec",
    "segment_seconds": 30,
    "max_storage_gb": 2.5,
    "keep_days": 3,
    "min_free_gb": 4.5
  },
  "session": {"enabled": true, "output_dir": "/tmp/sess", "min_free_gb": 0.5},
  "stream": {"enabled": true, "url": "rtsp://10.0.0.2:8554/cam"},
  "run": {"frames": 300, "stats_seconds": 5}
})"};

// Whether `text` is the text of one of the strings that the result keeps (not a copy that moved).
bool keptByResult(const ParseResult& result, std::string_view text) {
    return std::any_of(result.storage.begin(), result.storage.end(),
                       [text](const std::string& kept) { return text.data() == kept.c_str(); });
}

void checkConfigEveryKey(Check& check, const fs::path& root) {
    writeDefaultConfig(root, everyKeyConfig);
    const ParseResult result{parseIn(root.string(), {})};
    check.expect(result.status == ParseStatus::Run, "the default config file gives every option");
    const AppOptions& options{result.options};
    check.expect(options.modelPath == "/models/a.rknn" && options.cameraNode == "/dev/video0" &&
                     options.width == customWidth && options.height == customHeight &&
                     options.rotation == Rotation::Rotate180,
                 "config: model and camera");
    check.expect(options.videoKbps == customKbps && options.jpegQuality == customQuality &&
                     near(options.holdSeconds, customHold) && near(options.gapSeconds, customGap) &&
                     near(options.minConfidence, customConfidence),
                 "config: encode and rule");
    check.expect(!options.violation && !options.violationImage && !options.violationClip,
                 "config: violation.enabled, violation.image and violation.clip");
    check.expect(options.outputDir == "/tmp/out" && near(options.clipPreSeconds, customPre) &&
                     near(options.clipPostSeconds, customPost) &&
                     near(options.maxStorageGb, customStorage) && options.keepDays == customDays &&
                     near(options.minFreeGb, customMinFree),
                 "config: violation");
    check.expect(options.archive && options.archiveDir == "/tmp/rec" &&
                     near(options.archiveSegmentSeconds, customSegment) &&
                     near(options.archiveMaxStorageGb, customArchiveStorage) &&
                     options.archiveKeepDays == customArchiveDays &&
                     near(options.archiveMinFreeGb, customArchiveMinFree),
                 "config: archive");
    check.expect(options.session && options.sessionDir == "/tmp/sess" &&
                     near(options.sessionMinFreeGb, customSessionMinFree),
                 "config: session");
    check.expect(options.stream && options.streamUrl == "rtsp://10.0.0.2:8554/cam",
                 "config: stream");
    check.expect(options.frames == customFrames && near(options.statsSeconds, customStats),
                 "config: run");
    check.expect(
        keptByResult(result, options.modelPath) && keptByResult(result, options.cameraNode) &&
            keptByResult(result, options.outputDir) && keptByResult(result, options.archiveDir),
        "the text options point at the strings the result keeps");

    // The result moves without losing its strings (short strings live inside their element).
    ParseResult moved{parseIn(root.string(), {})};
    const ParseResult later{std::move(moved)};
    check.expect(later.options.modelPath == "/models/a.rknn" &&
                     later.options.archiveDir == "/tmp/rec",
                 "the strings survive a move of the result");
}

void checkConfigOrder(Check& check, const fs::path& root) {
    writeDefaultConfig(
        root,
        R"({"model": "m", "camera": {"width": 1280, "node": "/a"}, "archive": {"enabled": true}})");
    const ParseResult fromFile{parseIn(root.string(), {})};
    check.expect(fromFile.status == ParseStatus::Run && fromFile.options.width == customWidth &&
                     fromFile.options.archive && fromFile.options.cameraNode == "/a",
                 "the file gives the values it names");
    check.expect(near(fromFile.options.holdSeconds, defaultHoldSeconds) &&
                     fromFile.options.height == defaultCameraHeight,
                 "the rest stay at the defaults");

    const ParseResult overridden{
        parseIn(root.string(), {"--width", "640", "--model", "other", "--node", "/b"})};
    check.expect(
        overridden.status == ParseStatus::Run && overridden.options.width == repeatedWidth &&
            overridden.options.modelPath == "other" && overridden.options.cameraNode == "/b",
        "the command line wins over the file");
    check.expect(overridden.options.archive, "a key the command line does not give stays");
    check.expect(!parseIn(root.string(), {"--archive", "false"}).options.archive,
                 "--archive false turns off what the file turned on");

    const ParseResult twice{parseIn(root.string(), {"--width", "800", "--width", "640"})};
    check.expect(twice.options.width == repeatedWidth, "the last of two command line values wins");

    writeDefaultConfig(root, R"({"model": "m", "camera": {"width": 1280, "width": 640}})");
    const ParseResult repeated{parseIn(root.string(), {})};
    check.expect(repeated.status == ParseStatus::Run && repeated.options.width == repeatedWidth,
                 "the last of two keys wins");
    writeDefaultConfig(root,
                       R"({"model": "m", "camera": {"width": 1280}, "camera": {"height": 720}})");
    const ParseResult split{parseIn(root.string(), {})};
    check.expect(split.status == ParseStatus::Run && split.options.width == customWidth &&
                     split.options.height == customHeight,
                 "a group given in two parts");

    writeDefaultConfig(root, R"({"model": "m", "archive": {"enabled": false}})");
    check.expect(!parseIn(root.string(), {}).options.archive, "archive false stays off");
    check.expect(parseIn(root.string(), {"--archive", "true"}).options.archive,
                 "--archive true turns on what the file left off");
    writeDefaultConfig(root, R"({"model": "m", "violation": {"clip": {"enabled": false}}})");
    check.expect(!parseIn(root.string(), {}).options.violationClip, "clip false from the file");
    check.expect(parseIn(root.string(), {"--violation-clip", "true"}).options.violationClip,
                 "--violation-clip true turns on what the file left off");
    writeDefaultConfig(root, R"({"model": "m", "violation": {"clip": {"post_seconds": 8}}})");
    check.expect(near(parseIn(root.string(), {}).options.clipPostSeconds, fileClipPost),
                 "a nested key sets its option");
    writeDefaultConfig(root, R"({"model": "m", "stream": {"enabled": true}})");
    check.expect(parseIn(root.string(), {}).options.stream, "stream true from the file");
    check.expect(!parseIn(root.string(), {"--stream", "false"}).options.stream,
                 "--stream false turns off what the file turned on");
    writeDefaultConfig(root, R"({"model": "m", "camera": {}, "archive": {}})");
    check.expect(parseIn(root.string(), {}).status == ParseStatus::Run, "empty groups");
}

void checkConfigModel(Check& check, const fs::path& root) {
    const std::string base{root.string()};
    writeDefaultConfig(root, R"({"model": "resource/rknn/best-rk3588.rknn"})");
    const ParseResult relative{parseIn(base, {})};
    check.expect(relative.status == ParseStatus::Run &&
                     relative.options.modelPath == base + "/resource/rknn/best-rk3588.rknn",
                 "a relative model is relative to the application folder");

    writeDefaultConfig(root, R"({"model": "/abs/m.rknn"})");
    check.expect(parseIn(base, {}).options.modelPath == "/abs/m.rknn", "an absolute model stays");

    writeDefaultConfig(root, R"({"model": "rel.rknn"})");
    check.expect(parseIn("", {"--config", (root / "resource/config/app_config.json").c_str()})
                         .options.modelPath == "rel.rknn",
                 "without an application folder a relative model stays as written");
    check.expect(parseIn(base, {"--model", "cli.rknn"}).options.modelPath == "cli.rknn",
                 "a model of the command line is not joined");

    writeDefaultConfig(root, R"({"camera": {"width": 1280}})");
    check.expect(parseIn(base, {}).status == ParseStatus::Invalid, "no model anywhere");
    check.expect(parseIn(base, {"--model", "m"}).status == ParseStatus::Run,
                 "the model of the command line completes the file");
}

void checkConfigFiles(Check& check, const fs::path& root) {
    const std::string base{root.string()};
    std::error_code error;
    fs::remove_all(root / "resource", error);
    check.expect(parseIn(base, {"--model", "m"}).status == ParseStatus::Run,
                 "a missing default file is not an error");
    check.expect(parseIn(base, {}).status == ParseStatus::Invalid,
                 "a missing default file and no model");

    const std::string other{
        writeFile(root / "other.json", R"({"model": "o", "camera": {"width": 1000}})")};
    const ParseResult explicitFile{parseIn(base, {"--config", other.c_str()})};
    check.expect(explicitFile.status == ParseStatus::Run &&
                     explicitFile.options.width == otherWidth &&
                     explicitFile.options.modelPath == base + "/o",
                 "--config reads another file");
    writeDefaultConfig(root, R"({"model": "d", "camera": {"width": 1280}})");
    const ParseResult instead{parseIn(base, {"--config", other.c_str()})};
    check.expect(instead.options.width == otherWidth, "--config replaces the default file");

    check.expect(
        parseIn(base, {"--config", (root / "absent.json").c_str(), "--model", "m"}).status ==
            ParseStatus::Invalid,
        "--config with a missing file");
    check.expect(parseIn(base, {"--config"}).status == ParseStatus::Invalid,
                 "--config without a value");
    check.expect(parseIn(base, {"--config", ""}).status == ParseStatus::Invalid,
                 "--config with an empty path");
    check.expect(parseIn(base, {"--config", root.c_str(), "--model", "m"}).status ==
                     ParseStatus::Invalid,
                 "--config with a folder");
}

void checkConfigRefusals(Check& check, const fs::path& root) {
    const std::string base{root.string()};
    struct Bad {
        const char* text;
        const char* what;
    };
    const std::vector<Bad> bad{
        {R"({"model": "m", "bogus": 1})", "an unknown key"},
        {R"({"model": "m", "camera": {"bogus": 1}})", "an unknown key in a group"},
        {R"({"model": "m", "bogus": {"width": 1280}})", "an unknown group"},
        {R"({"model": "m", "bogus": {}})", "an unknown empty group"},
        {R"({"model": "m", "mod": {}})", "a group named like the start of a key"},
        {R"({"model": "m", "cam": {}})", "a group named like the start of a group"},
        {R"({"model": "m", "model": {}})", "the model as an empty group"},
        {R"({"model": "m", "width": 1280})", "a key outside its group"},
        {R"({"model": "m", "encode": {"width": 1280}})", "a key in the wrong group"},
        {R"({"model": "m", "camera": {"model": "x"}})", "the model inside a group"},
        {R"({"model": {"path": "x"}})", "the model as a group"},
        {R"({"model": "m", "camera": 5})", "a group that is a number"},
        {R"({"model": "m", "camera": "x"})", "a group that is a string"},
        {R"({"model": "m", "camera": {"video": {"width": 1}}})", "a group inside a group"},
        {R"({"model": "m", "config": "x.json"})", "config is not a key"},
        {R"({"model": "m", "help": true})", "help is not a key"},
        {R"({"model": "m", "camera": {"--width": 1280}})", "a key with its dashes"},
        {R"({"model": "m", "encode": {"video-kbps": 8000}})", "a key with - instead of _"},
        {R"({"model": "m", "violation": {"dir": "/x"}})", "the old dir key"},
        {R"({"model": "m", "violation": {"clip_pre_seconds": 5}})", "the old clip_pre_seconds"},
        {R"({"model": "m", "violation": {"clip_post_seconds": 5}})", "the old clip_post_seconds"},
        {R"({"model": "m", "violation": {"clip": {"seconds": 5}}})", "an unknown clip key"},
        {R"({"model": "m", "violation": {"image": {"quality": 90}}})", "an unknown image key"},
        {R"({"model": "m", "violation": {"video": {"enabled": true}}})", "an unknown subgroup"},
        {R"({"model": "m", "violation": {"clip": {"enabled": "true"}}})", "text for clip enabled"},
        {R"({"model": "m", "violation": {"image": {"enabled": 1}}})", "a number for image enabled"},
        {R"({"model": "m", "violation": {"enabled": "false"}})", "text for violation enabled"},
        {R"({"model": "m", "violation": {"clip": 5}})", "a number for the clip group"},
        {R"({"model": "m", "violation": {"clip": {"pre_seconds": 0, "post_seconds": 0}}})",
         "a clip that is on with no time"},
        {R"({"model": "m", "violation": {"clip": {"pre_seconds": 61}}})", "clip pre above limit"},
        {R"({"model": "m", "violation": {"clip": {"a": {"b": 1}}}})", "a third level of groups"},
        {R"({"model": "m", "clip": {"enabled": true}})", "the clip group outside violation"},
        {R"({"model": "m", "archive": {"dir": "/x"}})", "the old archive dir key"},
        {R"({"model": "m", "record": {"enabled": true}})", "the old record group"},
        {R"({"model": "m", "camera": {"width": 1281}})", "an odd width"},
        {R"({"model": "m", "camera": {"width": true}})", "a boolean for a number"},
        {R"({"model": "m", "camera": {"width": "1280"}})", "text for a number"},
        {R"({"model": "m", "camera": {"width": 1280.0}})", "a real for a whole number"},
        {R"({"model": "m", "camera": {"rotation": 45}})", "a rotation that is not allowed"},
        {R"({"model": "m", "camera": {"rotation": "180"}})", "text for the rotation"},
        {R"({"model": "m", "camera": {"node": 11}})", "a number for the node"},
        {R"({"model": "m", "archive": {"enabled": 1}})", "a number for enabled"},
        {R"({"model": "m", "stream": {"enabled": "true"}})", "text for stream enabled"},
        {R"({"model": "m", "stream": {"url": ""}})", "an empty stream address"},
        {R"({"model": "m", "stream": {"port": 1}})", "an unknown stream key"},
        {R"({"model": "m", "archive": {"enabled": "true"}})", "text for enabled"},
        {R"({"model": true})", "a boolean for the model"},
        {R"({"model": 5})", "a number for the model"},
        {R"({"model": ""})", "an empty model"},
        {R"({"model": "m", "rule": {"min_confidence": 1.5}})", "a confidence above 1"},
        {R"({"model": "m", "violation": {"keep_days": 0}})", "keep_days 0"},
        {R"({"model": "m", "violation": {"min_free_gb": -1}})", "violation min_free_gb below 0"},
        {R"({"model": "m", "session": {"min_free_gb": -1}})", "session min_free_gb below 0"},
        {R"({"model": "m", "session": {"output_dir": ""}})", "an empty session folder"},
        {R"({"model": "m", "archive": {"min_free_gb": 100001}})", "archive min_free_gb above"},
        {R"({"model": "m", "violation": {"output_dir": ""}})", "an empty violation folder"},
        {R"({"model": "m", "archive": {"output_dir": ""}})", "an empty archive folder"},
        {R"({"model": "m", )", "a cut text"},
        {"", "an empty file"},
        {"[]", "an array"},
        {R"({"model": "m"} {})", "two objects"},
    };
    for (const Bad& item : bad) {
        writeDefaultConfig(root, item.text);
        check.expect(parseIn(base, {}).status == ParseStatus::Invalid, item.what);
    }
    writeDefaultConfig(root, R"({"model": "m", "bogus": 1})");
    check.expect(parseIn(base, {"--help"}).status == ParseStatus::Help,
                 "--help works with a broken config file");
    writeDefaultConfig(root, std::string(oversizedConfigBytes, ' '));
    check.expect(parseIn(base, {"--model", "m"}).status == ParseStatus::Invalid,
                 "a config file over the size limit");
}

void checkConfig(Check& check) {
    const std::optional<fs::path> root{makeRoot()};
    check.expect(root.has_value(), "a temporary folder");
    if (!root) {
        return;
    }
    checkConfigEveryKey(check, *root);
    checkConfigOrder(check, *root);
    checkConfigModel(check, *root);
    checkConfigFiles(check, *root);
    checkConfigRefusals(check, *root);
    std::error_code error;
    fs::remove_all(*root, error);
}
} // namespace

int main() {
    const Logger log{"TEST_OPTIONS", LogLevel::Debug};
    Check check{log};
    checkDefaults(check);
    checkEveryFlag(check);
    checkCases(check);
    checkConfig(check);
    if (check.ok()) {
        log.info("PASS");
        return EXIT_SUCCESS;
    }
    log.error("FAIL");
    return EXIT_FAILURE;
}
