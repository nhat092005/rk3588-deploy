#ifndef APP_OPTIONS_H
#define APP_OPTIONS_H

#include <cstdint>
#include <string_view>
#include <vector>

#include "jpeg_encoder.h"
#include "logger.h"
#include "recording_retention.h"
#include "rotation.h"
#include "segment_writer.h"
#include "video_encoder.h"
#include "violation_retention.h"
#include "violation_rule.h"
#include "violation_store.h"

/** @brief Defaults of the options that no library owns. */
constexpr std::string_view defaultCameraNode{"/dev/video11"};
constexpr int defaultCameraWidth{3840};
constexpr int defaultCameraHeight{2160};
constexpr double defaultStatsSeconds{1.0};
/** @brief Seconds of video kept before and recorded after a violation. */
constexpr double defaultClipPreSeconds{5.0};
constexpr double defaultClipPostSeconds{5.0};
/** @brief Longest clip time accepted on either side, in seconds. */
constexpr double maxClipSeconds{60.0};
/** @brief Longest segment accepted, in seconds. */
constexpr double maxSegmentSeconds{3600.0};
/** @brief Largest storage limit and age accepted. */
constexpr double maxStorageGbLimit{100'000.0};
constexpr int maxKeepDays{3650};
/** @brief Largest camera side accepted, in pixels. */
constexpr int maxCameraSide{16'384};

/** @brief Everything the user can set on the command line. The rest is fixed in the code. */
struct AppOptions {
    /** Path of the .rknn model. Required, no default. */
    std::string_view modelPath;
    /** Camera capture node and the frame size to ask for. */
    std::string_view cameraNode{defaultCameraNode};
    int width{defaultCameraWidth};
    int height{defaultCameraHeight};
    /** How the camera is mounted. */
    Rotation rotation{Rotation::None};
    /** Folder of the violation files and the video. */
    std::string_view outputDir{defaultViolationRoot};
    /** Video bitrate in kilobits per second and JPEG quality factor. */
    int videoKbps{defaultVideoKbps};
    int jpegQuality{defaultJpegQuality};
    /** Violation rule: seconds before a violation is reported, seconds of break that do not end
     *  it, and the lowest confidence of a NoHelmet or NoVest box. */
    double holdSeconds{defaultHoldSeconds};
    double gapSeconds{defaultGapSeconds};
    float minConfidence{defaultViolationConfidence};
    /** Seconds of video before and after a violation in its clip. Both 0 means no clip. */
    double clipPreSeconds{defaultClipPreSeconds};
    double clipPostSeconds{defaultClipPostSeconds};
    /** The folder of the violations may use this many gigabytes (10^9 bytes) and keeps a
     *  violation this many days. The oldest go first. */
    double maxStorageGb{defaultMaxStorageGb};
    int keepDays{defaultKeepDays};
    /** Continuous recording of the video, off unless --record is given: the segments go to
     *  recordDir, recordSegmentSeconds long, within their own size and age limits. */
    bool record{false};
    std::string_view recordDir{defaultRecordRoot};
    double recordSegmentSeconds{defaultSegmentSeconds};
    double recordMaxStorageGb{defaultRecordMaxStorageGb};
    int recordKeepDays{defaultRecordKeepDays};
    /** Frames to process, 0 for no limit (until SIGINT or SIGTERM). */
    std::int64_t frames{0};
    /** Seconds between two lines of statistics. */
    double statsSeconds{defaultStatsSeconds};
};

/** @brief What parseOptions found. */
enum class ParseStatus : std::uint8_t { Run, Help, Invalid };

/** @brief The result of parseOptions: `options` is meaningful only when `status` is Run. */
struct ParseResult {
    ParseStatus status{ParseStatus::Invalid};
    AppOptions options;
};

/**
 * @brief Reads the command line: `--name value` pairs in any order (a flag given twice keeps its
 *        last value), and `--help`.
 * @param args The command line from commandLine(), the first entry is the program name.
 * @param log Logger that receives the usage text and the reason of a failure.
 * @return Run with the options, Help after `--help` (the usage was logged), or Invalid on an
 *         unknown flag, a missing or bad value, or a missing --model (the reason was logged).
 *
 * The flags: --model, --node, --width, --height, --rotation, --output-dir, --video-kbps,
 * --jpeg-quality, --hold-seconds, --gap-seconds, --min-confidence, --clip-pre-seconds,
 * --clip-post-seconds, --max-storage-gb, --keep-days, --record (no value), --record-dir,
 * --record-segment-seconds, --record-max-storage-gb, --record-keep-days, --frames, --stats-seconds.
 * The values are checked against the ranges of the libraries that use them.
 */
ParseResult parseOptions(const std::vector<const char*>& args, const Logger& log);

#endif // APP_OPTIONS_H
