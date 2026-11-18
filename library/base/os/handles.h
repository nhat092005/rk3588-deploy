#ifndef OS_HANDLES_H
#define OS_HANDLES_H

#include <cerrno>
#include <csignal>
#include <cstddef>
#include <optional>
#include <system_error>
#include <utility>

#include <sys/mman.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include "logger.h"

constexpr int invalidDescriptor{-1};

/**
 * @brief The error of the C call that has just failed.
 * @return errno as an error code. Call it right after the failing call, before anything else
 *         can change errno.
 */
inline std::error_code lastError() noexcept {
    return {errno, std::generic_category()};
}

/**
 * @brief Owns one file descriptor. Move-only. A failure to close is logged by the destructor and
 *        by move assignment. After an explicit close() the descriptor is invalid, so nothing is
 *        logged twice.
 */
class FileDescriptor {
public:
    FileDescriptor() noexcept = default;

    /**
     * @brief Takes ownership of a descriptor.
     * @param rawDescriptor Result of open() or similar. A negative value means an empty handle.
     * @param logger Logger that reports a failure to close.
     */
    FileDescriptor(int rawDescriptor, Logger logger) noexcept
        : descriptor{rawDescriptor}, log{logger} {}
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;
    FileDescriptor(FileDescriptor&& other) noexcept
        : descriptor{std::exchange(other.descriptor, invalidDescriptor)}, log{other.log} {}
    FileDescriptor& operator=(FileDescriptor&& other) noexcept {
        if (this != &other) {
            closeAndLog();
            descriptor = std::exchange(other.descriptor, invalidDescriptor);
            log = other.log;
        }
        return *this;
    }
    ~FileDescriptor() {
        closeAndLog();
    }

    /** @brief Whether this handle owns a descriptor. */
    bool valid() const noexcept {
        return descriptor >= 0;
    }

    /**
     * @brief The raw descriptor for system calls.
     * @return The descriptor, still owned by this handle, or a negative value when empty.
     */
    int get() const noexcept {
        return descriptor;
    }

    /**
     * @brief Closes the descriptor once. The handle is empty afterwards.
     * @return The error of close(), or no error when it succeeded or the handle was empty.
     */
    [[nodiscard]] std::error_code close() noexcept {
        if (!valid()) {
            return {};
        }
        const int result{::close(descriptor)};
        descriptor = invalidDescriptor;
        return result == 0 ? std::error_code{} : lastError();
    }

private:
    void closeAndLog() noexcept {
        const std::error_code error{close()};
        if (error) {
            log.error("close failed: ", error);
        }
    }

    int descriptor{invalidDescriptor};
    // Default logger of a handle that owns nothing: it never fires. Owners pass their own.
    Logger log{"FD", LogLevel::Error};
};

/** @brief Owns one mmap region. Move-only. A failure to unmap is logged like in FileDescriptor. */
class Mapping {
public:
    Mapping() noexcept = default;

    /**
     * @brief Takes ownership of a region.
     * @param mapped Result of mmap(). MAP_FAILED gives an empty mapping.
     * @param mappedLength Length that was passed to mmap(), in bytes.
     * @param logger Logger that reports a failure to unmap.
     */
    Mapping(void* mapped, std::size_t mappedLength, Logger logger) noexcept
        : address{mapped == MAP_FAILED ? nullptr : mapped}, length{mappedLength}, log{logger} {}
    Mapping(const Mapping&) = delete;
    Mapping& operator=(const Mapping&) = delete;
    Mapping(Mapping&& other) noexcept
        : address{std::exchange(other.address, nullptr)}, length{std::exchange(other.length, 0)},
          log{other.log} {}
    Mapping& operator=(Mapping&& other) noexcept {
        if (this != &other) {
            unmapAndLog();
            address = std::exchange(other.address, nullptr);
            length = std::exchange(other.length, 0);
            log = other.log;
        }
        return *this;
    }
    ~Mapping() {
        unmapAndLog();
    }

    /** @brief Whether this handle owns a region. */
    bool valid() const noexcept {
        return address != nullptr;
    }

    /** @brief Start of the region, read-only, or null when empty. */
    const void* data() const noexcept {
        return address;
    }

    /** @brief Start of the region, for writing, or null when empty. */
    void* data() noexcept {
        return address;
    }

    /** @brief Length of the region in bytes, 0 when empty. */
    std::size_t size() const noexcept {
        return length;
    }

    /**
     * @brief Unmaps the region once. The handle is empty afterwards.
     * @return The error of munmap(), or no error when it succeeded or the handle was empty.
     */
    [[nodiscard]] std::error_code unmap() noexcept {
        if (!valid()) {
            return {};
        }
        const int result{munmap(address, length)};
        address = nullptr;
        length = 0;
        return result == 0 ? std::error_code{} : lastError();
    }

private:
    void unmapAndLog() noexcept {
        const std::error_code error{unmap()};
        if (error) {
            log.error("munmap failed: ", error);
        }
    }

    void* address{nullptr};
    std::size_t length{0};
    Logger log{"MAP", LogLevel::Error};
};

/** @brief SIGINT and SIGTERM as a readable descriptor. They are blocked, so no handler runs. */
class StopSignal {
public:
    /**
     * @brief Blocks SIGINT and SIGTERM in the calling thread and opens a signalfd for them.
     * @param log Logger that the descriptor uses to report a failure to close.
     * @return The StopSignal, or empty on failure, which is logged.
     */
    static std::optional<StopSignal> create(const Logger& log) {
        sigset_t mask{};
        if (sigemptyset(&mask) != 0 || sigaddset(&mask, SIGINT) != 0 ||
            sigaddset(&mask, SIGTERM) != 0 || sigprocmask(SIG_BLOCK, &mask, nullptr) != 0) {
            log.error("blocking SIGINT and SIGTERM failed: ", lastError());
            return std::nullopt;
        }
        FileDescriptor signals{signalfd(invalidDescriptor, &mask, SFD_CLOEXEC | SFD_NONBLOCK), log};
        if (!signals.valid()) {
            log.error("signalfd failed: ", lastError());
            return std::nullopt;
        }
        return std::optional<StopSignal>{StopSignal{std::move(signals)}};
    }

    /**
     * @brief The signalfd to poll.
     * @return A descriptor that becomes readable when SIGINT or SIGTERM is pending. It stays owned
     *         by this StopSignal.
     */
    int descriptor() const noexcept {
        return signals.get();
    }

private:
    explicit StopSignal(FileDescriptor signalDescriptor) noexcept
        : signals{std::move(signalDescriptor)} {}

    FileDescriptor signals;
};

#endif
