#ifndef SESSION_WORKER_H
#define SESSION_WORKER_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "app_options.h"
#include "logger.h"
#include "packet_queue.h"

/** @brief What the session thread has done, readable from any thread. */
struct SessionCounts {
    std::atomic<std::int64_t> bytesWritten{0};
    /** Gaps in the stream (packets dropped because the queue was full). */
    std::atomic<std::int64_t> gaps{0};
    std::atomic<std::int64_t> writeErrors{0};
    /** True once the writing stopped for the rest of the run (no room, or a failure). */
    std::atomic<bool> stopped{false};
};

/**
 * @brief The work of the session thread (only created for --session true): takes the encoded
 *        packets that the writer thread pushes into the queue and writes them to one file for the
 *        whole run (SessionWriter). The disk can stall for seconds, and the file is flushed every
 *        second, which is why this is a thread of its own and the queue holds about 6 seconds.
 *        Move-only through its members.
 */
class SessionWorker {
public:
    /**
     * @brief Creates the queue and the writer, which checks the folder and the free space.
     * @param options The command line: folder, floor, video bitrate.
     * @param log Logger that the object keeps for the rest of its life.
     * @return The worker, or empty when the folder is missing or not writable or the volume is
     *         below the floor, which is logged.
     */
    static std::optional<SessionWorker> create(const AppOptions& options, const Logger& log);

    SessionWorker(const SessionWorker&) = delete;
    SessionWorker& operator=(const SessionWorker&) = delete;
    SessionWorker(SessionWorker&& other) noexcept;
    SessionWorker& operator=(SessionWorker&& other) noexcept;
    ~SessionWorker();

    /** @brief The queue that the writer thread pushes the packets into. Owned by the worker. */
    PacketQueue& queue() noexcept;

    /** @brief Writes until the queue is closed and empty, then flushes and closes the file. */
    void run() noexcept;

    /** @brief The counts, valid while the worker lives. */
    const SessionCounts& counts() const noexcept;

    /** @brief Path of the file, empty when no key frame has opened it. Read it after run(). */
    std::string path() const;

private:
    struct State;
    explicit SessionWorker(std::unique_ptr<State> initial) noexcept;

    std::unique_ptr<State> state;
};

#endif // SESSION_WORKER_H
