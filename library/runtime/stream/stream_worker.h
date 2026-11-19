#ifndef STREAM_WORKER_H
#define STREAM_WORKER_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "logger.h"
#include "packet_queue.h"

/** @brief Where the stream is published when --stream-url is not given: the MediaMTX on the board.
 */
constexpr std::string_view defaultStreamUrl{"rtsp://127.0.0.1:8554/live"};

/** @brief How long the program may leave its input unread by default, in milliseconds. */
constexpr int defaultStreamStallMs{5'000};

/** @brief What the stream worker needs to know. */
struct StreamConfig {
    /** The program that publishes the stream and its arguments, started with the packets on its
     *  standard input (see ffmpegPushCommand()). Not empty. */
    std::vector<std::string> command;
    /** Bitrate of the video in kilobits per second, above 0. It sizes the queue. */
    int kbps{0};
    /** Largest packet that the encoder can produce, in bytes, above 0. */
    std::size_t maxPacketBytes{0};
    /** How long the program may leave its input unread before the stream is given up, in
     *  milliseconds. */
    int stallMs{defaultStreamStallMs};
};

/** @brief What the stream thread has done, readable from any thread. */
struct StreamCounts {
    std::atomic<std::int64_t> packetsSent{0};
    std::atomic<std::int64_t> bytesSent{0};
    /** True once the stream was given up: the program ended, stalled, or did not start. */
    std::atomic<bool> failed{false};
};

/**
 * @brief The command that publishes an H.265 Annex B stream on standard input to an RTSP address
 *        over TCP, without re-encoding.
 * @param url RTSP address to publish to, not empty.
 * @return The program and its arguments, for StreamConfig::command.
 *
 * Raw H.265 carries no timestamps, and ffmpeg would assume 25 frames per second for a 30 fps
 * stream, so that the delay grows. The packets are stamped with the time they arrive instead.
 */
std::vector<std::string> ffmpegPushCommand(std::string_view url);

/**
 * @brief The work of the stream thread (only created for --stream true): starts the publishing
 *        program, takes the encoded packets that the writer thread pushes into the queue and
 *        writes them to its input.
 *
 * The queue holds about one second: a viewer wants the latest picture, not a deep backlog. When
 * it is full the writer's push drops packets until the next key frame, as for the recording. When
 * the program ends, stalls or cannot start, the stream is given up with one warning, the queue is
 * closed (so later pushes cost nothing) and the rest of the application is not affected.
 * Move-only through its members.
 */
class StreamWorker {
public:
    /**
     * @brief Creates the queue.
     * @param config The command, bitrate and packet size.
     * @param log Logger that the object keeps for the rest of its life.
     * @return The worker, or empty when the command is empty or a size is 0, which is logged.
     */
    static std::optional<StreamWorker> create(const StreamConfig& config, const Logger& log);

    StreamWorker(const StreamWorker&) = delete;
    StreamWorker& operator=(const StreamWorker&) = delete;
    StreamWorker(StreamWorker&& other) noexcept;
    StreamWorker& operator=(StreamWorker&& other) noexcept;
    ~StreamWorker();

    /** @brief The queue that the writer thread pushes the packets into. Owned by the worker. */
    PacketQueue& queue() noexcept;

    /**
     * @brief Starts the program and sends packets until stop() is called, the queue is closed and
     *        empty, or the stream is given up. Ends the program before it returns. Run it on one
     *        thread, which should be pinned to a small core first: the program inherits that.
     */
    void run() noexcept;

    /** @brief Ends run() early, also while it waits on a program that does not read. Safe from any
     *         thread. */
    void stop() noexcept;

    /** @brief The counts, valid while the worker lives. */
    const StreamCounts& counts() const noexcept;

private:
    struct State;
    explicit StreamWorker(std::unique_ptr<State> initial) noexcept;

    std::unique_ptr<State> state;
};

#endif // STREAM_WORKER_H
