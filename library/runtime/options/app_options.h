#ifndef APP_OPTIONS_H
#define APP_OPTIONS_H

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "jpeg_encoder.h"
#include "logger.h"
#include "recording_retention.h"
#include "rotation.h"
#include "segment_writer.h"
#include "session_writer.h"
#include "stream_worker.h"
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
/** @brief Free space the volume keeps, in gigabytes, as the retention libraries define it. */
constexpr double defaultMinFreeGb{static_cast<double>(defaultMinFreeBytes) / bytesPerGigabyte};
constexpr double defaultArchiveMinFreeGb{static_cast<double>(defaultArchiveMinFreeBytes) /
                                         bytesPerGigabyte};
constexpr double defaultSessionMinFreeGb{static_cast<double>(defaultSessionMinFreeBytes) /
                                         bytesPerGigabyte};
/** @brief Largest camera side accepted, in pixels. */
constexpr int maxCameraSide{16'384};
/** @brief Smallest height of a downscaled output, and the heights that are set when it is turned
 *         on without one: images 720, video 1080. */
constexpr int minDownscaleHeight{240};
constexpr int defaultImageDownscaleHeight{720};
constexpr int defaultVideoDownscaleHeight{1080};

/** @brief Where the default config file is, below the folder of the application (see parseOptions).
 */
constexpr std::string_view defaultConfigFile{"resource/config/app_config.json"};

/** @brief Everything the user can set in the config file or on the command line. The rest is fixed
 *         in the code. */
struct AppOptions {
    /** Path of the .rknn model. Required, no default. */
    std::string_view modelPath;
    /** The config file to read instead of the default one. Empty: the default file, if it exists.
     */
    std::string_view configPath;
    /** Camera capture node and the frame size to ask for. */
    std::string_view cameraNode{defaultCameraNode};
    int width{defaultCameraWidth};
    int height{defaultCameraHeight};
    /** How the camera is mounted. */
    Rotation rotation{Rotation::None};
    /** Violations as files, on unless --violation false is given. Off: the rule and the boxes in
     *  the video go on, nothing is stored and the folder need not exist. */
    bool violation{true};
    /** What a stored violation holds: the two JPEG images and the video clip. Both off leaves
     *  only event.json. They do nothing while `violation` is off. */
    bool violationImage{true};
    bool violationClip{true};
    /** Smaller images and clips: with the switch on, the file is scaled to this height (the width
     *  keeps the proportions), and a clip is encoded again at that size. */
    bool violationImageDownscale{false};
    int violationImageHeight{defaultImageDownscaleHeight};
    bool violationClipDownscale{false};
    int violationClipHeight{defaultVideoDownscaleHeight};
    /** Folder of the violation files. */
    std::string_view outputDir{defaultViolationRoot};
    /** Video bitrate in kilobits per second and JPEG quality factor. */
    int videoKbps{defaultVideoKbps};
    int jpegQuality{defaultJpegQuality};
    /** How the video files (the clip of a violation, the archive and the session) are stored: raw
     *  H.265 or fragmented MP4. */
    VideoFormat fileFormat{VideoFormat::H265};
    /** Violation rule: seconds before a violation is reported, seconds of break that do not end
     *  it, and the lowest confidence of a NoHelmet or NoVest box. */
    double holdSeconds{defaultHoldSeconds};
    double gapSeconds{defaultGapSeconds};
    float minConfidence{defaultViolationConfidence};
    /** Seconds of video before and after a violation in its clip, at least one above 0 while the
     *  clip is on. */
    double clipPreSeconds{defaultClipPreSeconds};
    double clipPostSeconds{defaultClipPostSeconds};
    /** The folder of the violations may use this many gigabytes (10^9 bytes) and keeps a
     *  violation this many days. The oldest go first. */
    double maxStorageGb{defaultMaxStorageGb};
    int keepDays{defaultKeepDays};
    /** Free gigabytes that the volume of the violations must keep: below it the oldest violations
     *  go. 0 turns the limit off. */
    double minFreeGb{defaultMinFreeGb};
    /** Archive of the video in segment files, off unless --archive true is given: the segments go
     * to archiveDir, archiveSegmentSeconds long, within their own size and age limits. */
    bool archive{false};
    std::string_view archiveDir{defaultArchiveRoot};
    double archiveSegmentSeconds{defaultSegmentSeconds};
    double archiveMaxStorageGb{defaultArchiveMaxStorageGb};
    int archiveKeepDays{defaultArchiveKeepDays};
    /** The same for the volume of the archive. Keep it above the one of the violations, so that
     *  the video is trimmed before a violation is. */
    double archiveMinFreeGb{defaultArchiveMinFreeGb};
    /** The archive video scaled to this height when the switch is on. */
    bool archiveDownscale{false};
    int archiveHeight{defaultVideoDownscaleHeight};
    /** Video of the whole run in one file, off unless --session true is given: the file goes to
     *  sessionDir, is never removed, and stops growing when the volume has less than
     *  sessionMinFreeGb free. */
    bool session{false};
    std::string_view sessionDir{defaultSessionRoot};
    double sessionMinFreeGb{defaultSessionMinFreeGb};
    /** The session video scaled to this height when the switch is on. */
    bool sessionDownscale{false};
    int sessionHeight{defaultVideoDownscaleHeight};
    /** Live stream of the video, off unless --stream true is given: ffmpeg publishes it to
     *  streamUrl (an RTSP server such as MediaMTX must listen there). */
    bool stream{false};
    std::string_view streamUrl{defaultStreamUrl};
    /** Frames to process, 0 for no limit (until SIGINT or SIGTERM). */
    std::int64_t frames{0};
    /** Seconds between two lines of statistics. */
    double statsSeconds{defaultStatsSeconds};
};

/** @brief What parseOptions found. */
enum class ParseStatus : std::uint8_t { Run, Help, Invalid };

/**
 * @brief The result of parseOptions: `options` is meaningful only when `status` is Run.
 *
 * The text fields of `options` refer to the command line and to `storage`, so the result is
 * move-only and must outlive every use of `options`.
 */
struct ParseResult {
    ParseResult() = default;
    ParseResult(const ParseResult&) = delete;
    ParseResult& operator=(const ParseResult&) = delete;
    ParseResult(ParseResult&&) noexcept = default;
    ParseResult& operator=(ParseResult&&) noexcept = default;
    ~ParseResult() = default;

    ParseStatus status{ParseStatus::Invalid};
    AppOptions options;
    /** The strings that the config file gave. Never resized after the file was read. */
    std::vector<std::string> storage;
};

/**
 * @brief Builds the options from the defaults, then the config file, then the command line: a
 *        later source wins.
 * @param args The command line from commandLine(), the first entry is the program name.
 * @param baseDir The folder of the application (`.../deploy`, see executableBaseDirectory()), or
 *        empty for none. The default config file is `<baseDir>/resource/config/app_config.json`,
 *        read when it exists, and a relative `model` in a config file is relative to `baseDir`.
 * @param log Logger that receives the usage text and the reason of a failure.
 * @return Run with the options, Help when `--help` is among the arguments (the usage was logged),
 *         or Invalid on an unknown flag or key, a missing or bad value, a config file that cannot
 *         be read or is not valid, or a missing model (the reason was logged).
 *
 * The command line is `--name value` pairs in any order (a flag given twice keeps its last
 * value), `--config <file>` to read another config file (it must exist), and `--help`. The flags:
 * --model, --node, --width, --height, --rotation, --video-kbps, --jpeg-quality, --file-format (h265
 * or mp4), --hold-seconds,
 * --gap-seconds, --min-confidence, --violation (true or false), --violation-output-dir,
 * --violation-image (true or false), --violation-clip (true or false),
 * --violation-clip-pre-seconds, --violation-clip-post-seconds, --violation-image-downscale (true or
 * false), --violation-image-downscale-height, --violation-clip-downscale,
 * --violation-clip-downscale-height, --max-storage-gb,
 * --keep-days, --violation-min-free-gb,
 * --archive (true or false),
 * --archive-output-dir, --archive-segment-seconds, --archive-max-storage-gb, --archive-keep-days,
 * --archive-min-free-gb, --archive-downscale, --archive-downscale-height,
 * --session (true or false), --session-output-dir, --session-min-free-gb, --session-downscale,
 * --session-downscale-height,
 * --stream (true or false), --stream-url, --frames, --stats-seconds.
 *
 * The config file is one JSON object: `model` at the top and the other options in groups
 * (`camera`, `encode`, `rule`, `violation`, `archive`, `session`, `stream`, `run`; `violation`
 * holds the groups `image` and `clip`, and these, `archive` and `session` a group `downscale`), for
 * example
 * `{"model": "m.rknn", "camera": {"width": 3840}, "archive": {"enabled": true}}`. Every key is
 * optional. A key has the kind of value of its flag (`width` is a number, `node` a string,
 * `archive.enabled` true or false) and the same range. The place of each key is in the table of
 * flags in app_options.cpp.
 */
ParseResult parseOptions(const std::vector<const char*>& args, std::string_view baseDir,
                         const Logger& log);

#endif // APP_OPTIONS_H
