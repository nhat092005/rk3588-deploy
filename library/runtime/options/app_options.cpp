#include "app_options.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <string>

#include "file_text.h"
#include "config_json.h"

namespace {
constexpr int decimalBase{10};
constexpr std::size_t programIndex{0};
constexpr std::string_view helpFlag{"--help"};
constexpr std::string_view configFlag{"--config"};
constexpr std::string_view modelKey{"model"};

// A whole number in [low, high], or false for junk, trailing text or a value out of range.
bool parseWhole(const char* text, std::int64_t low, std::int64_t high, std::int64_t& value) {
    char* end{nullptr};
    const long long parsed{std::strtoll(text, &end, decimalBase)};
    if (end == text || *end != '\0' || parsed < low || parsed > high) {
        return false;
    }
    value = parsed;
    return true;
}

// A finite number in [low, high], or false for junk, trailing text, nan, inf or out of range.
bool parseReal(const char* text, double low, double high, double& value) {
    char* end{nullptr};
    const double parsed{std::strtod(text, &end)};
    if (end == text || *end != '\0' || !std::isfinite(parsed) || parsed < low || parsed > high) {
        return false;
    }
    value = parsed;
    return true;
}

bool setModel(AppOptions& options, const char* text) {
    options.modelPath = text;
    return *text != '\0';
}

bool setConfig(AppOptions& options, const char* text) {
    options.configPath = text;
    return *text != '\0';
}

bool setNode(AppOptions& options, const char* text) {
    options.cameraNode = text;
    return *text != '\0';
}

bool setWidth(AppOptions& options, const char* text) {
    std::int64_t value{0};
    if (!parseWhole(text, 1, maxCameraSide, value) || value % 2 != 0) {
        return false;
    }
    options.width = static_cast<int>(value);
    return true;
}

bool setHeight(AppOptions& options, const char* text) {
    std::int64_t value{0};
    if (!parseWhole(text, 1, maxCameraSide, value) || value % 2 != 0) {
        return false;
    }
    options.height = static_cast<int>(value);
    return true;
}

bool setRotation(AppOptions& options, const char* text) {
    const std::optional<Rotation> rotation{parseRotation(text)};
    if (rotation) {
        options.rotation = *rotation;
    }
    return rotation.has_value();
}

bool setViolationOutputDir(AppOptions& options, const char* text) {
    options.outputDir = text;
    return *text != '\0';
}

bool setVideoKbps(AppOptions& options, const char* text) {
    std::int64_t value{0};
    if (!parseWhole(text, 1, maxVideoKbps, value)) {
        return false;
    }
    options.videoKbps = static_cast<int>(value);
    return true;
}

bool setJpegQuality(AppOptions& options, const char* text) {
    std::int64_t value{0};
    if (!parseWhole(text, minJpegQuality, maxJpegQuality, value)) {
        return false;
    }
    options.jpegQuality = static_cast<int>(value);
    return true;
}

// The hold time must be above 0: the lower bound is the smallest positive double.
bool setHoldSeconds(AppOptions& options, const char* text) {
    double value{0.0};
    constexpr double day{86'400.0};
    if (!parseReal(text, std::nextafter(0.0, 1.0), day, value)) {
        return false;
    }
    options.holdSeconds = value;
    return true;
}

bool setGapSeconds(AppOptions& options, const char* text) {
    double value{0.0};
    constexpr double day{86'400.0};
    if (!parseReal(text, 0.0, day, value)) {
        return false;
    }
    options.gapSeconds = value;
    return true;
}

bool setMinConfidence(AppOptions& options, const char* text) {
    double value{0.0};
    if (!parseReal(text, 0.0, 1.0, value)) {
        return false;
    }
    options.minConfidence = static_cast<float>(value);
    return true;
}

// true or false, nothing else.
bool parseSwitch(const char* text, bool& value) {
    const std::string_view word{text};
    if (word != "true" && word != "false") {
        return false;
    }
    value = word == "true";
    return true;
}

bool setViolation(AppOptions& options, const char* text) {
    return parseSwitch(text, options.violation);
}

bool setViolationImage(AppOptions& options, const char* text) {
    return parseSwitch(text, options.violationImage);
}

bool setViolationClip(AppOptions& options, const char* text) {
    return parseSwitch(text, options.violationClip);
}

// A height for a downscaled output: even, from minDownscaleHeight to the largest camera side.
// Whether it is below the camera is checked when the camera is known.
bool parseDownscaleHeight(const char* text, int& height) {
    std::int64_t value{0};
    if (!parseWhole(text, minDownscaleHeight, maxCameraSide, value) || value % 2 != 0) {
        return false;
    }
    height = static_cast<int>(value);
    return true;
}

bool setViolationImageDownscale(AppOptions& options, const char* text) {
    return parseSwitch(text, options.violationImageDownscale);
}

bool setViolationImageHeight(AppOptions& options, const char* text) {
    return parseDownscaleHeight(text, options.violationImageHeight);
}

bool setViolationClipDownscale(AppOptions& options, const char* text) {
    return parseSwitch(text, options.violationClipDownscale);
}

// h265 or mp4, nothing else.
bool parseFormat(const char* text, VideoFormat& format) {
    const std::optional<VideoFormat> parsed{parseVideoFormat(text)};
    if (!parsed) {
        return false;
    }
    format = *parsed;
    return true;
}

bool setViolationClipFormat(AppOptions& options, const char* text) {
    return parseFormat(text, options.violationClipFormat);
}

bool setArchiveFormat(AppOptions& options, const char* text) {
    return parseFormat(text, options.archiveFormat);
}

bool setSessionFormat(AppOptions& options, const char* text) {
    return parseFormat(text, options.sessionFormat);
}

bool setViolationClipHeight(AppOptions& options, const char* text) {
    return parseDownscaleHeight(text, options.violationClipHeight);
}

bool setArchiveDownscale(AppOptions& options, const char* text) {
    return parseSwitch(text, options.archiveDownscale);
}

bool setArchiveHeight(AppOptions& options, const char* text) {
    return parseDownscaleHeight(text, options.archiveHeight);
}

bool setSessionDownscale(AppOptions& options, const char* text) {
    return parseSwitch(text, options.sessionDownscale);
}

bool setSessionHeight(AppOptions& options, const char* text) {
    return parseDownscaleHeight(text, options.sessionHeight);
}

bool setClipPreSeconds(AppOptions& options, const char* text) {
    double value{0.0};
    if (!parseReal(text, 0.0, maxClipSeconds, value)) {
        return false;
    }
    options.clipPreSeconds = value;
    return true;
}

bool setClipPostSeconds(AppOptions& options, const char* text) {
    double value{0.0};
    if (!parseReal(text, 0.0, maxClipSeconds, value)) {
        return false;
    }
    options.clipPostSeconds = value;
    return true;
}

bool setMaxStorageGb(AppOptions& options, const char* text) {
    double value{0.0};
    if (!parseReal(text, std::nextafter(0.0, 1.0), maxStorageGbLimit, value)) {
        return false;
    }
    options.maxStorageGb = value;
    return true;
}

bool setKeepDays(AppOptions& options, const char* text) {
    std::int64_t value{0};
    if (!parseWhole(text, 1, maxKeepDays, value)) {
        return false;
    }
    options.keepDays = static_cast<int>(value);
    return true;
}

bool setMinFreeGb(AppOptions& options, const char* text) {
    double value{0.0};
    if (!parseReal(text, 0.0, maxStorageGbLimit, value)) {
        return false;
    }
    options.minFreeGb = value;
    return true;
}

bool setSession(AppOptions& options, const char* text) {
    const std::string_view value{text};
    if (value != "true" && value != "false") {
        return false;
    }
    options.session = value == "true";
    return true;
}

bool setSessionOutputDir(AppOptions& options, const char* text) {
    options.sessionDir = text;
    return *text != '\0';
}

bool setSessionMinFreeGb(AppOptions& options, const char* text) {
    double value{0.0};
    if (!parseReal(text, 0.0, maxStorageGbLimit, value)) {
        return false;
    }
    options.sessionMinFreeGb = value;
    return true;
}

// --archive takes true or false.
bool setArchive(AppOptions& options, const char* text) {
    const std::string_view value{text};
    if (value != "true" && value != "false") {
        return false;
    }
    options.archive = value == "true";
    return true;
}

bool setStream(AppOptions& options, const char* text) {
    const std::string_view value{text};
    if (value != "true" && value != "false") {
        return false;
    }
    options.stream = value == "true";
    return true;
}

bool setStreamUrl(AppOptions& options, const char* text) {
    options.streamUrl = text;
    return *text != '\0';
}

bool setArchiveOutputDir(AppOptions& options, const char* text) {
    options.archiveDir = text;
    return *text != '\0';
}

bool setArchiveSegmentSeconds(AppOptions& options, const char* text) {
    double value{0.0};
    if (!parseReal(text, std::nextafter(0.0, 1.0), maxSegmentSeconds, value)) {
        return false;
    }
    options.archiveSegmentSeconds = value;
    return true;
}

bool setArchiveMaxStorageGb(AppOptions& options, const char* text) {
    double value{0.0};
    if (!parseReal(text, std::nextafter(0.0, 1.0), maxStorageGbLimit, value)) {
        return false;
    }
    options.archiveMaxStorageGb = value;
    return true;
}

bool setArchiveMinFreeGb(AppOptions& options, const char* text) {
    double value{0.0};
    if (!parseReal(text, 0.0, maxStorageGbLimit, value)) {
        return false;
    }
    options.archiveMinFreeGb = value;
    return true;
}

bool setArchiveKeepDays(AppOptions& options, const char* text) {
    std::int64_t value{0};
    if (!parseWhole(text, 1, maxKeepDays, value)) {
        return false;
    }
    options.archiveKeepDays = static_cast<int>(value);
    return true;
}

bool setFrames(AppOptions& options, const char* text) {
    std::int64_t value{0};
    constexpr std::int64_t most{1'000'000'000'000};
    if (!parseWhole(text, 0, most, value)) {
        return false;
    }
    options.frames = value;
    return true;
}

bool setStatsSeconds(AppOptions& options, const char* text) {
    double value{0.0};
    constexpr double hour{3'600.0};
    if (!parseReal(text, std::nextafter(0.0, 1.0), hour, value)) {
        return false;
    }
    options.statsSeconds = value;
    return true;
}

using Setter = bool (*)(AppOptions&, const char*);

struct Flag {
    std::string_view name;
    Setter set;
    const char* help;
    // The kind of value the config file must give, and its place there: "group.key", or "key" at
    // the top. Empty path: not in the config file.
    JsonKind kind;
    std::string_view path;
};

constexpr std::array<Flag, 44> flags{{
    {"--model", setModel, "path of the .rknn model (required unless the config file gives it)",
     JsonKind::String, "model"},
    {"--config", setConfig, "config file, default <app folder>/resource/config/app_config.json",
     JsonKind::String, ""},
    {"--node", setNode, "camera capture node", JsonKind::String, "camera.node"},
    {"--width", setWidth, "camera frame width in pixels, even", JsonKind::Number, "camera.width"},
    {"--height", setHeight, "camera frame height in pixels, even", JsonKind::Number,
     "camera.height"},
    {"--rotation", setRotation, "camera mounting, 0, 90, 180 or 270 degrees clockwise",
     JsonKind::Number, "camera.rotation"},
    {"--video-kbps", setVideoKbps, "video bitrate in kilobits per second", JsonKind::Number,
     "encode.video_kbps"},
    {"--jpeg-quality", setJpegQuality, "JPEG quality factor, 1 to 99", JsonKind::Number,
     "encode.jpeg_quality"},
    {"--hold-seconds", setHoldSeconds, "seconds before a violation is reported", JsonKind::Number,
     "rule.hold_seconds"},
    {"--gap-seconds", setGapSeconds, "seconds of break that do not end a violation",
     JsonKind::Number, "rule.gap_seconds"},
    {"--min-confidence", setMinConfidence, "lowest confidence of a NoHelmet or NoVest box, 0 to 1",
     JsonKind::Number, "rule.min_confidence"},
    {"--violation", setViolation, "save the violations as files, true or false", JsonKind::Boolean,
     "violation.enabled"},
    {"--violation-output-dir", setViolationOutputDir, "folder of the violation files",
     JsonKind::String, "violation.output_dir"},
    {"--violation-image", setViolationImage, "save the two images of a violation, true or false",
     JsonKind::Boolean, "violation.image.enabled"},
    {"--violation-clip", setViolationClip, "save the video clip of a violation, true or false",
     JsonKind::Boolean, "violation.clip.enabled"},
    {"--violation-image-downscale", setViolationImageDownscale,
     "scale the violation images down, true or false", JsonKind::Boolean,
     "violation.image.downscale.enabled"},
    {"--violation-image-downscale-height", setViolationImageHeight,
     "height of the scaled violation images, even, 240 to 16384", JsonKind::Number,
     "violation.image.downscale.height"},
    {"--violation-clip-downscale", setViolationClipDownscale,
     "scale the violation clips down, true or false", JsonKind::Boolean,
     "violation.clip.downscale.enabled"},
    {"--violation-clip-downscale-height", setViolationClipHeight,
     "height of the scaled violation clips, even, 240 to 16384", JsonKind::Number,
     "violation.clip.downscale.height"},
    {"--violation-clip-format", setViolationClipFormat,
     "how the clip is stored: h265 (the encoder's bytes) or mp4 (fragmented MP4)", JsonKind::String,
     "violation.clip.format"},
    {"--violation-clip-pre-seconds", setClipPreSeconds,
     "seconds of video kept before a violation, 0 to 60", JsonKind::Number,
     "violation.clip.pre_seconds"},
    {"--violation-clip-post-seconds", setClipPostSeconds,
     "seconds of video after a violation, 0 to 60", JsonKind::Number,
     "violation.clip.post_seconds"},
    {"--max-storage-gb", setMaxStorageGb, "most gigabytes the violation folder may use",
     JsonKind::Number, "violation.max_storage_gb"},
    {"--keep-days", setKeepDays, "days a violation is kept, 1 to 3650", JsonKind::Number,
     "violation.keep_days"},
    {"--violation-min-free-gb", setMinFreeGb,
     "free gigabytes the violation volume keeps, 0 to 100000", JsonKind::Number,
     "violation.min_free_gb"},
    {"--archive", setArchive, "archive the video in segment files, true or false",
     JsonKind::Boolean, "archive.enabled"},
    {"--archive-downscale", setArchiveDownscale, "scale the archive video down, true or false",
     JsonKind::Boolean, "archive.downscale.enabled"},
    {"--archive-downscale-height", setArchiveHeight,
     "height of the scaled archive video, even, 240 to 16384", JsonKind::Number,
     "archive.downscale.height"},
    {"--archive-format", setArchiveFormat,
     "how the archive files are stored: h265 (the encoder's bytes) or mp4 (fragmented MP4)",
     JsonKind::String, "archive.format"},
    {"--archive-output-dir", setArchiveOutputDir, "folder of the archive", JsonKind::String,
     "archive.output_dir"},
    {"--archive-segment-seconds", setArchiveSegmentSeconds, "seconds of video in an archive file",
     JsonKind::Number, "archive.segment_seconds"},
    {"--archive-max-storage-gb", setArchiveMaxStorageGb, "most gigabytes the archive may use",
     JsonKind::Number, "archive.max_storage_gb"},
    {"--archive-keep-days", setArchiveKeepDays, "days an archive file is kept, 1 to 3650",
     JsonKind::Number, "archive.keep_days"},
    {"--archive-min-free-gb", setArchiveMinFreeGb,
     "free gigabytes the archive volume keeps, 0 to 100000", JsonKind::Number,
     "archive.min_free_gb"},
    {"--session", setSession, "write the video of the whole run to one file, true or false",
     JsonKind::Boolean, "session.enabled"},
    {"--session-downscale", setSessionDownscale, "scale the session video down, true or false",
     JsonKind::Boolean, "session.downscale.enabled"},
    {"--session-downscale-height", setSessionHeight,
     "height of the scaled session video, even, 240 to 16384", JsonKind::Number,
     "session.downscale.height"},
    {"--session-format", setSessionFormat,
     "how the session video is stored: h265 (the encoder's bytes) or mp4 (fragmented MP4)",
     JsonKind::String, "session.format"},
    {"--session-output-dir", setSessionOutputDir, "folder of the session video", JsonKind::String,
     "session.output_dir"},
    {"--session-min-free-gb", setSessionMinFreeGb,
     "free gigabytes the session volume keeps, 0 to 100000", JsonKind::Number,
     "session.min_free_gb"},
    {"--stream", setStream, "stream the video live over RTSP, true or false", JsonKind::Boolean,
     "stream.enabled"},
    {"--stream-url", setStreamUrl, "RTSP address the stream is published to", JsonKind::String,
     "stream.url"},
    {"--frames", setFrames, "frames to process, 0 for no limit", JsonKind::Number, "run.frames"},
    {"--stats-seconds", setStatsSeconds, "seconds between two lines of statistics",
     JsonKind::Number, "run.stats_seconds"},
}};

void logUsage(const char* program, const Logger& log) {
    log.info("usage: ", program, " [--model <path>] [flags]");
    for (const Flag& flag : flags) {
        log.info("  ", flag.name, " <value>  ", flag.help);
    }
    log.info("  ", helpFlag, "  this text");
}

const Flag* findFlag(std::string_view name) {
    for (const Flag& flag : flags) {
        if (flag.name == name) {
            return &flag;
        }
    }
    return nullptr;
}

// The flag of a place in the config file. --config has no place there: a file does not name
// another file.
const Flag* findPath(std::string_view path) {
    for (const Flag& flag : flags) {
        if (!flag.path.empty() && flag.path == path) {
            return &flag;
        }
    }
    return nullptr;
}

// Whether some flag lives in the group of that path ("violation" or "violation.clip").
bool isGroup(std::string_view path) {
    return std::any_of(flags.begin(), flags.end(), [path](const Flag& flag) {
        return flag.path.size() > path.size() && flag.path.substr(0, path.size()) == path &&
               flag.path[path.size()] == '.';
    });
}

// The command line looked at before anything is applied: --help anywhere, and the --config file.
// The program name is skipped and the value of a flag is not taken for a flag.
struct Scan {
    bool help{false};
    const char* config{nullptr};
};

Scan scanArguments(const std::vector<const char*>& args) {
    Scan scan;
    for (std::size_t at{1}; at < args.size(); ++at) {
        const std::string_view name{args.at(at)};
        if (name == helpFlag) {
            scan.help = true;
            continue;
        }
        if (findFlag(name) == nullptr) {
            continue;
        }
        if (name == configFlag && at + 1 < args.size()) {
            scan.config = args.at(at + 1);
        }
        ++at;
    }
    return scan;
}

const char* kindName(JsonKind kind) {
    switch (kind) {
    case JsonKind::String:
        return "a string";
    case JsonKind::Number:
        return "a number";
    case JsonKind::Boolean:
        return "true or false";
    case JsonKind::Object:
        break;
    }
    return "a group";
}

bool isAbsolute(std::string_view path) {
    return !path.empty() && path.front() == '/';
}

// Puts the entries of one config file into the options. The strings are kept in the result.
bool applyConfig(const FileText& file, const std::string& path, std::string_view baseDir,
                 ParseResult& result, const Logger& log) {
    JsonParse json{parseConfigJson(file.text)};
    if (!json.ok) {
        log.error(path, ": ", json.reason, " (byte ", json.offset, ")");
        return false;
    }
    // Two strings at most per entry (the value, then the model path joined to baseDir). The
    // vector must not grow later: the options point into its elements.
    result.storage.reserve(json.entries.size() * 2);
    for (JsonEntry& entry : json.entries) {
        const std::string where{entry.group.empty() ? entry.key : entry.group + "." + entry.key};
        if (entry.kind == JsonKind::Object) {
            if (!isGroup(where)) {
                log.error(path, ": unknown group ", where);
                return false;
            }
            continue;
        }
        const Flag* flag{findPath(where)};
        if (flag == nullptr) {
            log.error(path, ": unknown key ", where);
            return false;
        }
        if (entry.kind != flag->kind) {
            log.error(path, ": ", where, " must be ", kindName(flag->kind));
            return false;
        }
        result.storage.push_back(std::move(entry.value));
        // An empty model must stay empty so that setModel refuses it: joined, it would be the
        // folder.
        if (flag->path == modelKey && !baseDir.empty() && !result.storage.back().empty() &&
            !isAbsolute(result.storage.back())) {
            std::string joined{baseDir};
            joined += '/';
            joined += result.storage.back();
            result.storage.push_back(std::move(joined));
        }
        if (!flag->set(result.options, result.storage.back().c_str())) {
            log.error(path, ": bad value for ", where, ": ", result.storage.back());
            return false;
        }
    }
    return true;
}

// Reads the config file, the given one or the default one. false only on a failure to report.
bool loadConfig(const Scan& scan, std::string_view baseDir, ParseResult& result,
                const Logger& log) {
    std::string path;
    if (scan.config != nullptr) {
        path = scan.config;
    } else if (!baseDir.empty()) {
        path = std::string{baseDir} + '/' + std::string{defaultConfigFile};
    } else {
        return true;
    }
    const FileText file{readTextFile(path, log)};
    if (file.status == ReadStatus::Failed) {
        return false;
    }
    if (file.status == ReadStatus::Missing) {
        if (scan.config != nullptr) {
            log.error("config file not found: ", path);
            return false;
        }
        log.info("no config file ", path, ", using the defaults and the command line");
        return true;
    }
    log.info("config file ", path);
    return applyConfig(file, path, baseDir, result, log);
}
} // namespace

ParseResult parseOptions(const std::vector<const char*>& args, std::string_view baseDir,
                         const Logger& log) {
    ParseResult result;
    const char* program{args.empty() ? "program" : args.at(programIndex)};
    const Scan scan{scanArguments(args)};
    if (scan.help) {
        logUsage(program, log);
        result.status = ParseStatus::Help;
        return result;
    }
    if (!loadConfig(scan, baseDir, result, log)) {
        return result;
    }
    for (std::size_t at{1}; at < args.size(); ++at) {
        const std::string_view name{args.at(at)};
        const Flag* flag{findFlag(name)};
        if (flag == nullptr) {
            log.error("unknown flag ", name);
            logUsage(program, log);
            return result;
        }
        if (at + 1 >= args.size()) {
            log.error(name, " needs a value");
            return result;
        }
        ++at;
        if (!flag->set(result.options, args.at(at))) {
            log.error("bad value for ", name, ": ", args.at(at));
            return result;
        }
    }
    if (result.options.modelPath.empty()) {
        log.error("a model is required: --model, or \"model\" in the config file");
        logUsage(program, log);
        return result;
    }
    const AppOptions& chosen{result.options};
    if (chosen.violation && chosen.violationClip && chosen.clipPreSeconds <= 0.0 &&
        chosen.clipPostSeconds <= 0.0) {
        log.error("the violation clip is on but pre_seconds and post_seconds are both 0: give one "
                  "of them a time, or turn the clip off (--violation-clip false)");
        return result;
    }
    result.status = ParseStatus::Run;
    return result;
}
