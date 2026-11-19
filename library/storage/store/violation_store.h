#ifndef VIOLATION_STORE_H
#define VIOLATION_STORE_H

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <optional>
#include <string>
#include <string_view>

#include "logger.h"
#include "video_file.h"
#include "violation_rule.h"

/** @brief Where the violations go on the board. The root is a parameter of the application. */
constexpr std::string_view defaultViolationRoot{"/data/violations"};
/** @brief Permissions of the files that are written (rw-r--r--). Directories get the same with
 *         the execute bit wherever there is a read bit. */
constexpr unsigned defaultFileMode{0644};

/** @brief Where and with which permissions to store. */
struct StoreConfig {
    /** Root directory. It must exist and be writable: the store does not create it. */
    std::string_view root{defaultViolationRoot};
    /** Permissions of the files, applied with fchmod so that the umask does not change them. */
    unsigned fileMode{defaultFileMode};
    /** The format of the video clip: `.h265` as the encoder wrote it (the default), or another. */
    VideoOutput clipOutput{rawVideoOutput};
};

/** @brief A block of bytes that someone else owns, for example a JPEG of JpegEncoder. */
struct StoredBytes {
    const std::uint8_t* data{nullptr};
    std::size_t size{0};
};

/** @brief A video clip that someone else owns: the packets of an H.265 stream, one after the other
 *         in a block of bytes, and the list that says where each one ends. */
struct StoredClip {
    const std::uint8_t* data{nullptr};
    std::size_t size{0};
    /** The packets in order. Their sizes add up to `size`. The first is a key frame. */
    const PacketInfo* packets{nullptr};
    std::size_t packetCount{0};
};

/** @brief One violation to store. */
struct ViolationRecord {
    /** The event of ViolationRule. Its box must be in the pixels of the images below. */
    ViolationEvent event;
    /** Wall-clock time of the event. It names the day directory and the event directory. */
    std::time_t wallTime{0};
    /** Width and height in pixels of the frame that the box refers to, both above 0, also when
     *  there is no image. */
    int imageWidth{0};
    int imageHeight{0};
    /** The frame as the camera saw it, a JPEG. Empty (size 0) when there is none. */
    StoredBytes original;
    /** The frame with the boxes drawn, a JPEG. Empty (size 0) when there is none. */
    StoredBytes marked;
};

/**
 * @brief Writes a violation to disk, one directory per violation:
 *        `<root>/<YYYY-MM-DD>/<HHMMSS>_id<track>_<kind>/` with `original.jpg` and `marked.jpg`
 *        (when given) and `event.json`, which names the files that exist. The kind is NoHelmet,
 * NoVest or NoHelmetNoVest. A second violation with the same name gets the suffix `_2`, `_3` and so
 * on.
 *
 * The directory appears whole: the files are written into a hidden temporary directory, flushed
 * to disk, and the directory is renamed at the end, so a power cut leaves either nothing or a
 * complete violation (plus at most a hidden `.tmp` directory). Meant for a worker thread: it
 * allocates strings and waits for the disk, so it does not belong in the frame loop.
 */
class ViolationStore {
public:
    /**
     * @brief Checks that the root is a directory that the process can write.
     * @param config Root directory and file permissions.
     * @param log Logger that the object keeps for the rest of its life.
     * @return The store, or empty when the root is missing, is not a directory or is not
     *         writable, which is logged.
     */
    static std::optional<ViolationStore> create(const StoreConfig& config, const Logger& log);

    /**
     * @brief Writes one violation.
     * @param record The event, the time and the images. The original must not be empty, the image
     *        size must be above 0 and the event must have NoHelmet or NoVest set.
     * @return The path of the violation directory, or empty on failure (bad record, full disk,
     *         lost permission), which is logged. Nothing is left behind on failure.
     */
    [[nodiscard]] std::optional<std::string> save(const ViolationRecord& record) const noexcept;

    /**
     * @brief Adds the video clip of a stored violation as `clip.h265` (or `clip.mp4`, see
     *        StoreConfig::clipOutput) in its directory. Called later than save(), when the clip is
     *        complete.
     * @param directory The path that save() returned.
     * @param clip The H.265 bytes (Annex B) and their packets. Not empty.
     * @return false when the directory is gone, the clip is empty or its packets do not add up,
     *         a clip file exists, or the write fails (full disk), which is logged. Nothing is left
     *         behind on failure.
     *
     * The file appears whole: it is written under a hidden name, flushed, and renamed.
     */
    [[nodiscard]] bool saveClip(const std::string& directory,
                                const StoredClip& clip) const noexcept;

private:
    ViolationStore(std::string rootPath, unsigned mode, VideoOutput output, Logger logger)
        : root{std::move(rootPath)}, fileMode{mode}, clipOutput{output}, log{logger} {}

    std::string root;
    unsigned fileMode;
    VideoOutput clipOutput;
    Logger log;
};

#endif // VIOLATION_STORE_H
