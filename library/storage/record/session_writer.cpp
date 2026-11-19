#include "session_writer.h"

#include <array>
#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "volume.h"

namespace {
namespace fs = std::filesystem;

constexpr std::size_t timeBufferBytes{64};
constexpr int maxNameAttempts{100};
constexpr unsigned readBits{0444U};
constexpr unsigned executeShift{2U};
constexpr double bytesPerGb{1e9};
constexpr const char* sessionSuffix{".h265"};

unsigned directoryMode(unsigned fileMode) noexcept {
    return fileMode | ((fileMode & readBits) >> executeShift);
}

std::string formatTime(std::time_t wallTime, const char* format) {
    std::tm parts{};
    localtime_r(&wallTime, &parts);
    std::array<char, timeBufferBytes> text{};
    const std::size_t length{std::strftime(text.data(), text.size(), format, &parts)};
    return std::string{text.data(), length};
}

bool exists(const std::string& path) noexcept {
    struct stat info {};
    return ::stat(path.c_str(), &info) == 0;
}

// Flushes a directory entry (a created name) to disk.
void syncDirectory(const std::string& path, const Logger& log) noexcept {
    const FileDescriptor directory{::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC), log};
    if (directory.valid() && ::fsync(directory.get()) != 0) {
        log.warn("fsync ", path, ": ", lastError());
    }
}

// Writes every byte. false on an error (the cause is logged).
bool writeAll(const FileDescriptor& file, const std::vector<std::uint8_t>& bytes,
              const Logger& log) noexcept {
    std::size_t done{0};
    while (done < bytes.size()) {
        const ssize_t count{::write(file.get(), bytes.data() + done, bytes.size() - done)};
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            log.error("write: ", lastError());
            return false;
        }
        done += static_cast<std::size_t>(count);
    }
    return true;
}
} // namespace

SessionWriter::SessionWriter(const SessionConfig& config, Logger logger)
    : root{config.root}, minFreeBytes{config.minFreeBytes}, fileMode{config.fileMode},
      freeSpace{config.freeSpace != nullptr ? config.freeSpace : &volumeFreeBytes}, log{logger} {}

std::optional<SessionWriter> SessionWriter::create(const SessionConfig& config, const Logger& log) {
    const std::string root{config.root};
    struct stat info {};
    if (::stat(root.c_str(), &info) != 0) {
        log.error("session root ", root, ": ", lastError());
        return std::nullopt;
    }
    if (!S_ISDIR(info.st_mode)) {
        log.error("session root ", root, " is not a directory");
        return std::nullopt;
    }
    if (::access(root.c_str(), W_OK | X_OK) != 0) {
        log.error("session root ", root, " is not writable: ", lastError());
        return std::nullopt;
    }
    SessionWriter writer{config, log};
    if (!writer.hasRoom()) {
        const std::optional<std::uint64_t> free{writer.freeSpace(root)};
        log.error(
            "the volume of ", root, " has ", static_cast<double>(free.value_or(0)) / bytesPerGb,
            " GB free, below the floor of ", static_cast<double>(config.minFreeBytes) / bytesPerGb,
            " GB (--session-min-free-gb): no session video can be written");
        return std::nullopt;
    }
    log.info("session root ", root);
    return std::optional<SessionWriter>{std::move(writer)};
}

// Whether the volume keeps at least the floor. Unknown free space counts as room.
bool SessionWriter::hasRoom() const {
    if (minFreeBytes == 0) {
        return true;
    }
    const std::optional<std::uint64_t> free{freeSpace(root)};
    return !free || *free >= minFreeBytes;
}

// Opens the file under its final name: the first name that is free.
bool SessionWriter::open(std::time_t wallNow) {
    const unsigned dirMode{directoryMode(fileMode)};
    std::string dayPath{root};
    dayPath += '/';
    dayPath += formatTime(wallNow, "%Y-%m-%d");
    if (::mkdir(dayPath.c_str(), dirMode) == 0) {
        syncDirectory(root, log);
    } else if (errno != EEXIST) {
        log.error("mkdir ", dayPath, ": ", lastError());
        return false;
    }
    const std::string base{formatTime(wallNow, "%H%M%S")};
    for (int attempt{0}; attempt < maxNameAttempts; ++attempt) {
        std::string name{base};
        if (attempt > 0) {
            name += '_';
            name += std::to_string(attempt + 1);
        }
        std::string candidate{dayPath};
        candidate += '/';
        candidate += name;
        candidate += sessionSuffix;
        if (exists(candidate)) {
            continue;
        }
        file = FileDescriptor{
            ::open(candidate.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, fileMode), log};
        if (!file.valid()) {
            log.error("open ", candidate, ": ", lastError());
            return false;
        }
        if (::fchmod(file.get(), fileMode) != 0) {
            log.warn("fchmod ", candidate, ": ", lastError());
        }
        filePath = candidate;
        syncDirectory(dayPath, log);
        log.info("session video ", filePath);
        return true;
    }
    log.error("no free name for the session video of ", base);
    return false;
}

// Pushes what was written to the disk. false when the disk refuses.
bool SessionWriter::flush() {
    if (::fsync(file.get()) != 0) {
        log.error("fsync ", filePath, ": ", lastError());
        return false;
    }
    return true;
}

// Flushes and closes the file, which is kept in any case.
void SessionWriter::close() {
    if (!file.valid()) {
        return;
    }
    (void)flush();
    const std::error_code closing{file.close()};
    if (closing) {
        log.error("closing ", filePath, ": ", closing.message());
    }
    syncDirectory(fs::path{filePath}.parent_path().string(), log);
}

// Closes the file with what it has and stops for the rest of the run. Logs the reason once.
void SessionWriter::stop(const std::string& why) {
    if (isStopped) {
        return;
    }
    close();
    isStopped = true;
    reason = why;
    log.error("session video stopped: ", why, " (", written, " bytes kept in ",
              filePath.empty() ? "no file" : filePath, "); the rest goes on");
}

bool SessionWriter::consume(const QueuedPacket& packet, std::time_t wallNow) {
    if (isStopped) {
        return true;
    }
    if (!file.valid()) {
        if (!packet.keyFrame) {
            ++skipped;
            return true;
        }
        if (!hasRoom()) {
            stop("the free space of the volume is below the floor");
            return true;
        }
        if (!open(wallNow)) {
            stop("the file could not be opened");
            return false;
        }
    } else if (packet.keyFrame) {
        if (packet.afterGap) {
            ++gaps;
        }
        // The frames since the last key frame go to the disk before this one is written.
        if (!flush()) {
            stop("the disk refused to flush the file");
            return false;
        }
        if (!hasRoom()) {
            stop("the free space of the volume fell below the floor");
            return true;
        }
    }
    if (!writeAll(file, packet.bytes, log)) {
        stop("the disk refused a write");
        return false;
    }
    written += packet.bytes.size();
    return true;
}

void SessionWriter::finish() {
    close();
}
