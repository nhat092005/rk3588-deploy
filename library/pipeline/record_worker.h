#ifndef RECORD_WORKER_H
#define RECORD_WORKER_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

#include "app_options.h"
#include "logger.h"
#include "packet_queue.h"

/** @brief What the recording thread has done, readable from any thread. */
struct RecordCounts {
    std::atomic<std::int64_t> segmentsSaved{0};
    std::atomic<std::int64_t> bytesSaved{0};
    std::atomic<std::int64_t> writeErrors{0};
    /** Old segments removed to keep the limits. */
    std::atomic<std::int64_t> segmentsRemoved{0};
    /** Times a new segment was not opened because the volume had no room. */
    std::atomic<std::int64_t> segmentsRefused{0};
};

/**
 * @brief The work of the recording thread (only created for --record true): takes the encoded
 * packets that the writer thread pushes into the queue and writes them to segment files
 *        (SegmentWriter), keeping the folder within its limits (RecordingRetention) at the start of
 *        every segment, after every completed one, and once an hour. The disk can stall for
 *        seconds (measured on the board: 2.2 s for one write), which is why this is a thread of its
 *        own and the queue holds about 6 seconds. Move-only through its members.
 */
class RecordWorker {
public:
    /**
     * @brief Creates the queue, the segment writer and the retention (which removes the leftover
     *        temporary files of an interrupted run and reads the folder).
     * @param options The command line: folder, segment length, limits, video bitrate.
     * @param log Logger that the object keeps for the rest of its life.
     * @return The worker, or empty when the folder is missing or not writable or a limit is out of
     *         range, which is logged.
     */
    static std::optional<RecordWorker> create(const AppOptions& options, const Logger& log);

    RecordWorker(const RecordWorker&) = delete;
    RecordWorker& operator=(const RecordWorker&) = delete;
    RecordWorker(RecordWorker&& other) noexcept;
    RecordWorker& operator=(RecordWorker&& other) noexcept;
    ~RecordWorker();

    /** @brief The queue that the writer thread pushes the packets into. Owned by the worker. */
    PacketQueue& queue() noexcept;

    /**
     * @brief Writes until the queue is closed and empty, then completes the open segment.
     */
    void run() noexcept;

    /** @brief The counts, valid while the worker lives. */
    const RecordCounts& counts() const noexcept;

    /** @brief Bytes of the segments in the folder now (the retention's index). */
    std::uint64_t storedBytes() const noexcept;

private:
    struct State;
    explicit RecordWorker(std::unique_ptr<State> initial) noexcept;

    std::unique_ptr<State> state;
};

#endif // RECORD_WORKER_H
