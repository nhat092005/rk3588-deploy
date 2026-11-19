#include "child_process.h"

#include <array>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <system_error>
#include <thread>
#include <utility>

#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
constexpr int pipeEnds{2};
constexpr int standardInput{0};
// How long a full pipe is waited for before the stop flag and the stall time are looked at again.
constexpr int pollStepMs{50};
constexpr std::chrono::milliseconds reapStep{20};
constexpr std::chrono::milliseconds termGrace{1'000};
constexpr int signalExitBase{128};

int exitCodeOf(int waitStatus) {
    return WIFEXITED(waitStatus) ? WEXITSTATUS(waitStatus) : signalExitBase + WTERMSIG(waitStatus);
}

// Owns the spawn attributes and the file actions, which have to be destroyed.
struct SpawnSetup {
    SpawnSetup() = default;
    SpawnSetup(const SpawnSetup&) = delete;
    SpawnSetup& operator=(const SpawnSetup&) = delete;
    SpawnSetup(SpawnSetup&&) = delete;
    SpawnSetup& operator=(SpawnSetup&&) = delete;
    ~SpawnSetup() {
        posix_spawn_file_actions_destroy(&actions);
        posix_spawnattr_destroy(&attributes);
    }

    // The read end of the pipe becomes the standard input; the child gets no blocked signals and
    // the default SIGPIPE.
    bool prepare(int pipeRead) {
        sigset_t none;
        sigset_t pipeSignal;
        if (posix_spawn_file_actions_init(&actions) != 0) {
            return false;
        }
        if (posix_spawnattr_init(&attributes) != 0) {
            posix_spawn_file_actions_destroy(&actions);
            return false;
        }
        return sigemptyset(&none) == 0 && sigemptyset(&pipeSignal) == 0 &&
               sigaddset(&pipeSignal, SIGPIPE) == 0 &&
               posix_spawn_file_actions_adddup2(&actions, pipeRead, standardInput) == 0 &&
               posix_spawnattr_setsigmask(&attributes, &none) == 0 &&
               posix_spawnattr_setsigdefault(&attributes, &pipeSignal) == 0 &&
               posix_spawnattr_setflags(&attributes,
                                        POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF) == 0;
    }

    posix_spawn_file_actions_t actions{};
    posix_spawnattr_t attributes{};
};
} // namespace

std::optional<ChildProcess> ChildProcess::spawn(const std::vector<std::string>& command,
                                                const Logger& log) {
    if (command.empty()) {
        log.error("no command to start");
        return std::nullopt;
    }
    // A write to a pipe whose reader ended must give EPIPE, not kill the application.
    (void)std::signal(SIGPIPE, SIG_IGN);
    std::array<int, pipeEnds> ends{};
    if (pipe2(ends.data(), O_CLOEXEC) != 0) {
        log.error("pipe for ", command.front(), " failed: ", lastError());
        return std::nullopt;
    }
    const FileDescriptor readEnd{ends.at(0), log};
    FileDescriptor writeEnd{ends.at(1), log};
    // The write end only: the child reads from the other one and must block there.
    if (fcntl(writeEnd.get(), F_SETFL, O_NONBLOCK) != 0) {
        log.error("non-blocking pipe for ", command.front(), " failed: ", lastError());
        return std::nullopt;
    }
    // The C API wants char* and a nullptr at the end.
    std::vector<std::string> arguments{command};
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);
    for (std::string& argument : arguments) {
        argv.push_back(argument.data());
    }
    argv.push_back(nullptr);
    SpawnSetup setup;
    if (!setup.prepare(readEnd.get())) {
        log.error("spawn setup for ", command.front(), " failed");
        return std::nullopt;
    }
    pid_t child{-1};
    const int result{posix_spawnp(&child, command.front().c_str(), &setup.actions,
                                  &setup.attributes, argv.data(), environ)};
    if (result != 0) {
        log.error("start of ", command.front(),
                  " failed: ", std::error_code{result, std::generic_category()});
        return std::nullopt;
    }
    return std::optional<ChildProcess>{ChildProcess{child, std::move(writeEnd), log}};
}

ChildProcess::ChildProcess(pid_t child, FileDescriptor input, const Logger& logger) noexcept
    : pid{child}, pipeInput{std::move(input)}, log{logger} {}

ChildProcess::ChildProcess(ChildProcess&& other) noexcept
    : pid{std::exchange(other.pid, -1)}, pipeInput{std::move(other.pipeInput)}, log{other.log},
      status{std::exchange(other.status, std::nullopt)} {}

ChildProcess& ChildProcess::operator=(ChildProcess&& other) noexcept {
    if (this != &other) {
        (void)finish(termGrace);
        pid = std::exchange(other.pid, -1);
        pipeInput = std::move(other.pipeInput);
        log = other.log;
        status = std::exchange(other.status, std::nullopt);
    }
    return *this;
}

ChildProcess::~ChildProcess() {
    (void)finish(termGrace);
}

WriteResult ChildProcess::write(const std::vector<std::uint8_t>& bytes,
                                const WriteLimits& limits) noexcept {
    std::size_t done{0};
    auto lastProgress{std::chrono::steady_clock::now()};
    while (done < bytes.size()) {
        if (limits.stop->load()) {
            return WriteResult::Stopped;
        }
        const ssize_t count{::write(pipeInput.get(), bytes.data() + done, bytes.size() - done)};
        if (count > 0) {
            done += static_cast<std::size_t>(count);
            lastProgress = std::chrono::steady_clock::now();
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else if (count < 0 && errno == EAGAIN) {
            if (std::chrono::steady_clock::now() - lastProgress > limits.stall) {
                return WriteResult::Stalled;
            }
            pollfd waiting{pipeInput.get(), POLLOUT, 0};
            (void)::poll(&waiting, 1, pollStepMs);
        } else {
            return WriteResult::Broken;
        }
    }
    return WriteResult::Done;
}

std::optional<int> ChildProcess::pollExit() noexcept {
    if (status || pid < 0) {
        return status;
    }
    int waitStatus{0};
    const pid_t ended{waitpid(pid, &waitStatus, WNOHANG)};
    if (ended == pid) {
        status = exitCodeOf(waitStatus);
    }
    return status;
}

int ChildProcess::finish(std::chrono::milliseconds grace) noexcept {
    if (pid < 0) {
        return status.value_or(-1);
    }
    if (const std::error_code error{pipeInput.close()}; error) {
        log.warn("closing the pipe to the child failed: ", error);
    }
    const auto waitUntil = [this](std::chrono::milliseconds limit) {
        const auto deadline{std::chrono::steady_clock::now() + limit};
        while (!pollExit() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(reapStep);
        }
        return status.has_value();
    };
    if (!waitUntil(grace)) {
        log.warn("the child did not end after its input closed, sending SIGTERM");
        (void)kill(pid, SIGTERM);
        if (!waitUntil(termGrace)) {
            log.warn("the child did not end after SIGTERM, sending SIGKILL");
            (void)kill(pid, SIGKILL);
            int waitStatus{0};
            if (waitpid(pid, &waitStatus, 0) == pid) {
                status = exitCodeOf(waitStatus);
            }
        }
    }
    pid = -1;
    return status.value_or(-1);
}
