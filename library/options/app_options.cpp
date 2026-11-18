#include "app_options.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdlib>

namespace {
constexpr int decimalBase{10};
constexpr std::size_t programIndex{0};
constexpr std::string_view helpFlag{"--help"};

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

bool setOutputDir(AppOptions& options, const char* text) {
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

// --record takes no value: its presence turns the recording on.
bool setRecord(AppOptions& options, const char* /*unused*/) {
    options.record = true;
    return true;
}

bool setRecordDir(AppOptions& options, const char* text) {
    options.recordDir = text;
    return *text != '\0';
}

bool setRecordSegmentSeconds(AppOptions& options, const char* text) {
    double value{0.0};
    if (!parseReal(text, std::nextafter(0.0, 1.0), maxSegmentSeconds, value)) {
        return false;
    }
    options.recordSegmentSeconds = value;
    return true;
}

bool setRecordMaxStorageGb(AppOptions& options, const char* text) {
    double value{0.0};
    if (!parseReal(text, std::nextafter(0.0, 1.0), maxStorageGbLimit, value)) {
        return false;
    }
    options.recordMaxStorageGb = value;
    return true;
}

bool setRecordKeepDays(AppOptions& options, const char* text) {
    std::int64_t value{0};
    if (!parseWhole(text, 1, maxKeepDays, value)) {
        return false;
    }
    options.recordKeepDays = static_cast<int>(value);
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
    // false for a flag that stands alone (--record): the next argument is not its value.
    bool takesValue{true};
};

constexpr std::array<Flag, 22> flags{{
    {"--model", setModel, "path of the .rknn model (required)"},
    {"--node", setNode, "camera capture node"},
    {"--width", setWidth, "camera frame width in pixels, even"},
    {"--height", setHeight, "camera frame height in pixels, even"},
    {"--rotation", setRotation, "camera mounting, 0, 90, 180 or 270 degrees clockwise"},
    {"--output-dir", setOutputDir, "folder of the violation files"},
    {"--video-kbps", setVideoKbps, "video bitrate in kilobits per second"},
    {"--jpeg-quality", setJpegQuality, "JPEG quality factor, 1 to 99"},
    {"--hold-seconds", setHoldSeconds, "seconds before a violation is reported"},
    {"--gap-seconds", setGapSeconds, "seconds of break that do not end a violation"},
    {"--min-confidence", setMinConfidence, "lowest confidence of a NoHelmet or NoVest box, 0 to 1"},
    {"--clip-pre-seconds", setClipPreSeconds, "seconds of video kept before a violation, 0 to 60"},
    {"--clip-post-seconds", setClipPostSeconds, "seconds of video after a violation, 0 to 60"},
    {"--max-storage-gb", setMaxStorageGb, "most gigabytes the violation folder may use"},
    {"--keep-days", setKeepDays, "days a violation is kept, 1 to 3650"},
    {"--record", setRecord, "record the video continuously (no value)", false},
    {"--record-dir", setRecordDir, "folder of the recordings"},
    {"--record-segment-seconds", setRecordSegmentSeconds, "seconds of video in a recording file"},
    {"--record-max-storage-gb", setRecordMaxStorageGb, "most gigabytes the recordings may use"},
    {"--record-keep-days", setRecordKeepDays, "days a recording is kept, 1 to 3650"},
    {"--frames", setFrames, "frames to process, 0 for no limit"},
    {"--stats-seconds", setStatsSeconds, "seconds between two lines of statistics"},
}};

void logUsage(const char* program, const Logger& log) {
    log.info("usage: ", program, " --model <path> [flags]");
    for (const Flag& flag : flags) {
        log.info("  ", flag.name, flag.takesValue ? " <value>  " : "  ", flag.help);
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
} // namespace

ParseResult parseOptions(const std::vector<const char*>& args, const Logger& log) {
    ParseResult result;
    const char* program{args.empty() ? "program" : args.at(programIndex)};
    for (std::size_t at{1}; at < args.size(); ++at) {
        const std::string_view name{args.at(at)};
        if (name == helpFlag) {
            logUsage(program, log);
            result.status = ParseStatus::Help;
            return result;
        }
        const Flag* flag{findFlag(name)};
        if (flag == nullptr) {
            log.error("unknown flag ", name);
            logUsage(program, log);
            return result;
        }
        if (!flag->takesValue) {
            (void)flag->set(result.options, "");
            continue;
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
        log.error("--model is required");
        logUsage(program, log);
        return result;
    }
    result.status = ParseStatus::Run;
    return result;
}
