#ifndef SEGMENT_WRITER_H
#define SEGMENT_WRITER_H

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "handles.h"
#include "logger.h"
#include "packet_queue.h"
#include "violation_store.h"

/** @brief Defaults of the continuous recording. */
constexpr std::string_view defaultRecordRoot{"/data/records"};
constexpr double defaultSegmentSeconds{60.0};

/** @brief Where and how long. */
struct SegmentConfig {
    /** Root directory. It must exist and be writable: the writer does not create it. */
    std::string_view root{defaultRecordRoot};
    /** Seconds of video in a segment, above 0. A segment ends at the first key frame at or after
     *  this long. */
    double segmentSeconds{defaultSegmentSeconds};
    /** Permissions of the files, applied with fchmod so that the umask does not change them. */
    unsigned fileMode{defaultFileMode};
};

/** @brief A segment that was completed: its file is final and flushed. */
struct ClosedSegment {
    std::string path;
    std::uint64_t bytes{0};
};

/**
 * @brief Writes the packets of an H.265 stream into segment files:
 *        `<root>/<YYYY-MM-DD>/<HHMMSS>.h265`, named by the wall-clock time at which the segment
 *        starts (a second one in the same second gets `_2`, `_3` and so on). A segment starts on a
 *        key frame, which carries the VPS, SPS and PPS, so every file plays on its own.
 *
 * A file is written under a hidden name (`.<HHMMSS>.h265.tmp`), flushed with fsync when it is
 * complete, and renamed, so a file with its final name is always whole. A power cut leaves at most
 * a hidden temporary file. It allocates strings and waits for the disk: it belongs to a thread of
 * its own. Move-only through its members: a file that is open when the object goes is closed and
 * its temporary file stays (call finish() first).
 */
class SegmentWriter {
public:
    /**
     * @brief Checks the root.
     * @param config Root, segment length and file mode.
     * @param log Logger that the object keeps for the rest of its life.
     * @return The writer, or empty when the root is missing, not a directory or not writable, or
     *         the length is not above 0, which is logged.
     */
    static std::optional<SegmentWriter> create(const SegmentConfig& config, const Logger& log);

    /**
     * @brief Whether this packet ends the segment that is open and starts the next, or starts the
     *        first one: a key frame and (no file is open, or the packet follows a gap, or the
     *        segment has lasted long enough). The caller can then check that there is room before
     *        it lets consume() open a file.
     * @param packet The next packet of the stream.
     */
    bool startsSegment(const QueuedPacket& packet) const noexcept;

    /**
     * @brief Writes one packet.
     * @param packet The next packet of the stream.
     * @param wallNow Wall-clock time, used to name a segment that starts now.
     * @param allowNew false to refuse to open a file (no room on the volume): the segment that is
     *        open is still completed, and the packets are skipped until a key frame that may
     *        start one.
     * @return false when the disk refused a write or a file could not be opened (logged). The
     *         file is then removed and the packets are skipped until the next key frame.
     */
    [[nodiscard]] bool consume(const QueuedPacket& packet, std::time_t wallNow, bool allowNew);

    /** @brief Completes the segment that is open, at the end of the stream. */
    void finish();

    /** @brief The segments completed since the last call, oldest first. */
    std::vector<ClosedSegment> takeClosed();

    /** @brief Segments completed since create(). */
    std::size_t segmentCount() const noexcept {
        return segments;
    }

    /** @brief Packets skipped because no segment was open and they were not a key frame that
     *         could start one. */
    std::size_t skippedPackets() const noexcept {
        return skipped;
    }

private:
    SegmentWriter(const SegmentConfig& config, Logger logger)
        : root{config.root}, segmentSeconds{config.segmentSeconds}, fileMode{config.fileMode},
          log{logger} {}

    bool open(std::time_t wallNow, const QueuedPacket& first);
    bool close();
    void discard();

    std::string root;
    double segmentSeconds;
    unsigned fileMode;
    Logger log;
    FileDescriptor file;
    std::string tempPath;
    std::string finalPath;
    double segmentStart{0.0};
    std::uint64_t written{0};
    std::vector<ClosedSegment> closed;
    std::size_t segments{0};
    std::size_t skipped{0};
};

#endif // SEGMENT_WRITER_H
