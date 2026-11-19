#ifndef CHILD_PROCESS_H
#define CHILD_PROCESS_H

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <sys/types.h>

#include "handles.h"
#include "logger.h"

/** @brief How ChildProcess::write() ended. */
enum class WriteResult : std::uint8_t {
    /** Every byte went into the pipe. */
    Done,
    /** The child did not read for the whole stall time. */
    Stalled,
    /** The child closed its input or ended. */
    Broken,
    /** The stop flag was set. */
    Stopped
};

/** @brief The limits of one ChildProcess::write(). */
struct WriteLimits {
    /** How long the pipe may stay full before the write gives up. */
    std::chrono::milliseconds stall;
    /** Set from another thread to end the write early. */
    const std::atomic<bool>* stop;
};

/**
 * @brief A program started with a pipe to its standard input. Move-only. The destructor closes
 *        the pipe and ends the program, so no child outlives the object.
 *
 * The child starts with the default signal handling and no blocked signals (the application
 * blocks SIGINT and SIGTERM for its signalfd, and a child would inherit that), and without the
 * descriptors of the application (they are all close-on-exec). SIGPIPE is ignored in the
 * application, so a child that ended shows up as WriteResult::Broken and not as a signal.
 */
class ChildProcess {
public:
    /**
     * @brief Starts a program, searched in PATH, with its standard input connected to a pipe.
     * @param command The program name, then its arguments. Not empty.
     * @param log Logger that the object keeps for the rest of its life.
     * @return The child, or empty when the pipe or the start failed, which is logged.
     */
    static std::optional<ChildProcess> spawn(const std::vector<std::string>& command,
                                             const Logger& log);

    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;
    ChildProcess(ChildProcess&& other) noexcept;
    ChildProcess& operator=(ChildProcess&& other) noexcept;
    ~ChildProcess();

    /**
     * @brief Writes all the bytes to the standard input of the child, without blocking for
     *        longer than the limits.
     * @param bytes The bytes to write.
     * @param limits The stall time and the stop flag.
     * @return How it ended. Anything but Done leaves the bytes partly written.
     */
    [[nodiscard]] WriteResult write(const std::vector<std::uint8_t>& bytes,
                                    const WriteLimits& limits) noexcept;

    /**
     * @brief Whether the child has ended, without waiting.
     * @return Its exit status (128 plus the signal number when a signal ended it), or empty while
     *         it runs. The child is reaped at the first non-empty answer.
     */
    [[nodiscard]] std::optional<int> pollExit() noexcept;

    /**
     * @brief Closes the input and waits for the child to end: up to `grace`, then SIGTERM, then
     *        SIGKILL. Does nothing more once the child is reaped.
     * @param grace How long the child may take to end after its input closed.
     * @return The exit status, as in pollExit().
     */
    int finish(std::chrono::milliseconds grace) noexcept;

private:
    ChildProcess(pid_t child, FileDescriptor input, const Logger& logger) noexcept;

    pid_t pid{-1};
    FileDescriptor pipeInput;
    Logger log;
    std::optional<int> status;
};

#endif // CHILD_PROCESS_H
