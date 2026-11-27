#ifndef SESSION_WRITER_H
#define SESSION_WRITER_H

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "handles.h"
#include "logger.h"
#include "packet_queue.h"
#include "video_file.h"
#include "violation_store.h"

/** @brief Defaults of the session video: the folder and the free space that the volume keeps. */
constexpr std::string_view defaultSessionRoot{"/data/sessions"};
constexpr std::uint64_t defaultSessionMinFreeBytes{3'000'000'000};

/** @brief Free bytes of the volume that holds a path, or empty when that is not known. */
using FreeSpaceFunction = std::optional<std::uint64_t> (*)(const std::string& path);

/** @brief Where, and how much room to keep. */
struct SessionConfig {
    /** Root directory. It must exist and be writable: the writer does not create it. */
    std::string_view root{defaultSessionRoot};
    /** Free bytes that the volume must keep. The file is closed and the writing stops when the
     *  volume is below it. 0 turns this limit off. */
    std::uint64_t minFreeBytes{defaultSessionMinFreeBytes};
    /** Permissions of the file, applied with fchmod so that the umask does not change them. */
    unsigned fileMode{defaultFileMode};
    /** How the free space is read. Null means the real volume (volumeFreeBytes). A test gives its
     *  own. */
    FreeSpaceFunction freeSpace{nullptr};
    /** The format of the file: `.h265` as the encoder wrote it (the default), or another. */
    VideoOutput output{rawVideoOutput};
    /** Empty: the file is named by the clock, in a folder of the day (see SessionWriter). A name:
     *  the file is `<root>/<fileName><suffix>`, written as `<fileName><suffix>.part` until
     *  publish() renames it, and a root that is missing is created. */
    std::string_view fileName{};
};

/**
 * @brief Writes the packets of an H.265 stream of one run into one file:
 *        `<root>/<YYYY-MM-DD>/<HHMMSS>.h265` (or `.mp4`, see SessionConfig::output), named by the
 *        wall-clock time at which the first key frame arrives (a second run in the same second gets
 *        `_2`, `_3` and so on).
 *
 * With SessionConfig::fileName the file is `<root>/<fileName><suffix>` instead, with no folder of
 * the day. It is written under the name `<fileName><suffix>.part` and publish() renames it to the
 * final name once the video is complete, replacing the file of an earlier run: a run that stops
 * half way leaves only the `.part`.
 *
 * The file has its final name from the start and is flushed with fsync at every key frame (so a
 * power cut loses about a second or two) and when the run ends. A raw H.265 stream and a
 * fragmented MP4 play when they are cut short, which is why there is no temporary name. A gap in
 * the stream (the queue was full) does not start a new file: the key frame after it carries the
 * VPS, SPS and PPS, so the stream goes on. A failure never removes the file: the video that was
 * written is kept, the writing stops for the rest of the run and the reason is logged once. It
 * allocates strings and waits for the disk: it belongs to a thread of its own.
 */
class SessionWriter {
public:
    /**
     * @brief Checks the root and the free space.
     * @param config Root, floor and file mode.
     * @param log Logger that the object keeps for the rest of its life.
     * @return The writer, or empty when the root is missing, not a directory or not writable, or
     *         the volume is already below the floor, which is logged.
     */
    static std::optional<SessionWriter> create(const SessionConfig& config, const Logger& log);

    /**
     * @brief Writes one packet. Packets before the first key frame are skipped. At every key frame
     *        the file is flushed and the free space is checked.
     * @param packet The next packet of the stream.
     * @param wallNow Wall-clock time, used to name the file when it is opened.
     * @return false when the disk refused a write or the file could not be opened (logged, and
     *         the writer is stopped). true otherwise, also for a packet that is ignored because
     *         the writer is stopped.
     */
    [[nodiscard]] bool consume(const QueuedPacket& packet, std::time_t wallNow);

    /** @brief Flushes and closes the file at the end of the run. */
    void finish();

    /**
     * @brief Gives a finished file with a name (SessionConfig::fileName) its final name. Call it
     *        after finish(), when the video is complete.
     * @return true when the file now has its final name. false when nothing was written, the
     *         writer is not named or was stopped by a failure (the `.part` stays), or the rename
     *         failed, which is logged.
     */
    [[nodiscard]] bool publish();

    /** @brief Whether the writing stopped (no room, or a failure). It does not start again. */
    bool stopped() const noexcept {
        return isStopped;
    }

    /** @brief Why it stopped, empty while it has not. */
    const std::string& stopReason() const noexcept {
        return reason;
    }

    /** @brief Path of the file, empty until the first key frame opened it. */
    const std::string& path() const noexcept {
        return filePath;
    }

    /** @brief Bytes of the packets that were written to the file (an MP4 file is a little larger).
     */
    std::uint64_t bytesWritten() const noexcept {
        return written;
    }

    /** @brief Gaps in the stream that the file has (a key frame came after dropped packets). */
    std::size_t gapCount() const noexcept {
        return gaps;
    }

    /** @brief Packets skipped before the first key frame. */
    std::size_t skippedPackets() const noexcept {
        return skipped;
    }

private:
    SessionWriter(const SessionConfig& config, Logger logger);

    bool open(std::time_t wallNow);
    bool hasRoom() const;
    bool flush();
    void stop(const std::string& why);
    void close();

    bool openNamed();

    std::string root;
    std::string name;
    std::uint64_t minFreeBytes;
    unsigned fileMode;
    FreeSpaceFunction freeSpace;
    VideoOutput output;
    Logger log;
    FileDescriptor file;
    std::unique_ptr<VideoFileWriter> video;
    std::string filePath;
    std::uint64_t written{0};
    std::size_t gaps{0};
    std::size_t skipped{0};
    bool isStopped{false};
    bool published{false};
    std::string reason;
};

#endif // SESSION_WRITER_H
