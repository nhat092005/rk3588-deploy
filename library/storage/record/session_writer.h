#ifndef SESSION_WRITER_H
#define SESSION_WRITER_H

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <optional>
#include <string>
#include <string_view>

#include "handles.h"
#include "logger.h"
#include "packet_queue.h"
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
};

/**
 * @brief Writes the packets of an H.265 stream of one run into one file:
 *        `<root>/<YYYY-MM-DD>/<HHMMSS>.h265`, named by the wall-clock time at which the first key
 *        frame arrives (a second run in the same second gets `_2`, `_3` and so on).
 *
 * The file has its final name from the start and is flushed with fsync at every key frame (so a
 * power cut loses about a second) and when the run ends. A raw H.265 stream plays when it is cut
 * short, which is why there is no temporary name. A gap in the stream (the queue was full) does
 * not start a new file: the key frame after it carries the VPS, SPS and PPS, so the stream goes on.
 * A failure never removes the file: the video that was written is kept, the writing stops for the
 * rest of the run and the reason is logged once. It allocates strings and waits for the disk: it
 * belongs to a thread of its own.
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

    /** @brief Bytes written to the file. */
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

    std::string root;
    std::uint64_t minFreeBytes;
    unsigned fileMode;
    FreeSpaceFunction freeSpace;
    Logger log;
    FileDescriptor file;
    std::string filePath;
    std::uint64_t written{0};
    std::size_t gaps{0};
    std::size_t skipped{0};
    bool isStopped{false};
    std::string reason;
};

#endif // SESSION_WRITER_H
