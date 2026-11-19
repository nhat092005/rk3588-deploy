#include "segment_writer.h"

#include <array>
#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
namespace fs = std::filesystem;

constexpr std::size_t timeBufferBytes{64};
constexpr int maxNameAttempts{100};
constexpr unsigned readBits{0444U};
constexpr unsigned executeShift{2U};
constexpr const char* tempEnding{".tmp"};

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

// Flushes a directory entry (a created or renamed name) to disk.
void syncDirectory(const std::string& path, const Logger& log) noexcept {
    const FileDescriptor directory{::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC), log};
    if (directory.valid() && ::fsync(directory.get()) != 0) {
        log.warn("fsync ", path, ": ", lastError());
    }
}
} // namespace

std::optional<SegmentWriter> SegmentWriter::create(const SegmentConfig& config, const Logger& log) {
    const std::string root{config.root};
    struct stat info {};
    if (config.segmentSeconds <= 0.0) {
        log.error("segment length must be above 0");
        return std::nullopt;
    }
    if (config.output.open == nullptr) {
        log.error("recording: no writer for the format ", config.output.suffix);
        return std::nullopt;
    }
    if (::stat(root.c_str(), &info) != 0) {
        log.error("recording root ", root, ": ", lastError());
        return std::nullopt;
    }
    if (!S_ISDIR(info.st_mode)) {
        log.error("recording root ", root, " is not a directory");
        return std::nullopt;
    }
    if (::access(root.c_str(), W_OK | X_OK) != 0) {
        log.error("recording root ", root, " is not writable: ", lastError());
        return std::nullopt;
    }
    log.info("recording root ", root);
    return std::optional<SegmentWriter>{SegmentWriter{config, log}};
}

bool SegmentWriter::startsSegment(const QueuedPacket& packet) const noexcept {
    if (!packet.keyFrame) {
        return false;
    }
    return !file.valid() || packet.afterGap || packet.seconds - segmentStart >= segmentSeconds;
}

// Opens the next segment under its hidden name. The first name that is free.
bool SegmentWriter::open(std::time_t wallNow, const QueuedPacket& first) {
    const unsigned dirMode{directoryMode(fileMode)};
    std::string dayPath{root};
    dayPath += '/';
    dayPath += formatTime(wallNow, "%Y-%m-%d");
    if (::mkdir(dayPath.c_str(), dirMode) != 0 && errno != EEXIST) {
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
        finalPath = dayPath;
        finalPath += '/';
        finalPath += name;
        finalPath += output.suffix;
        tempPath = dayPath;
        tempPath += "/.";
        tempPath += name;
        tempPath += output.suffix;
        tempPath += tempEnding;
        if (!exists(finalPath) && !exists(tempPath)) {
            file = FileDescriptor{
                ::open(tempPath.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, fileMode), log};
            if (!file.valid()) {
                log.error("open ", tempPath, ": ", lastError());
                return false;
            }
            video = output.open(file.get(), log);
            if (!video) {
                const std::error_code ignored{file.close()};
                (void)ignored;
                (void)::unlink(tempPath.c_str());
                return false;
            }
            segmentStart = first.seconds;
            written = 0;
            return true;
        }
    }
    log.error("no free name for the segment of ", base);
    return false;
}

// Flushes the open file and gives it its final name. false on a failure (the file is removed).
bool SegmentWriter::close() {
    if (!file.valid()) {
        return true;
    }
    const bool ended{video->finish()};
    video.reset();
    const bool flushed{ended && ::fchmod(file.get(), fileMode) == 0 && ::fsync(file.get()) == 0};
    const std::error_code closing{file.close()};
    if (!flushed || closing) {
        std::string cause{"the writer failed"};
        if (ended) {
            cause = flushed ? closing.message() : lastError().message();
        }
        log.error("closing ", tempPath, ": ", cause);
        (void)::unlink(tempPath.c_str());
        return false;
    }
    if (::rename(tempPath.c_str(), finalPath.c_str()) != 0) {
        log.error("rename ", tempPath, " to ", finalPath, ": ", lastError());
        (void)::unlink(tempPath.c_str());
        return false;
    }
    syncDirectory(fs::path{finalPath}.parent_path().string(), log);
    closed.push_back(ClosedSegment{finalPath, written});
    ++segments;
    return true;
}

// Throws away the open file after a failure.
void SegmentWriter::discard() {
    video.reset();
    if (file.valid()) {
        const std::error_code ignored{file.close()};
        (void)ignored;
        (void)::unlink(tempPath.c_str());
    }
}

bool SegmentWriter::consume(const QueuedPacket& packet, std::time_t wallNow, bool allowNew) {
    if (startsSegment(packet)) {
        const bool completed{close()};
        if (!allowNew) {
            ++skipped;
            return completed;
        }
        if (!open(wallNow, packet)) {
            return false;
        }
    }
    if (!file.valid()) {
        ++skipped;
        return true;
    }
    if (!video->write(packet.bytes.data(), packet.bytes.size(), packet.keyFrame, packet.seconds)) {
        discard();
        return false;
    }
    written += packet.bytes.size();
    return true;
}

void SegmentWriter::finish() {
    (void)close();
}

std::vector<ClosedSegment> SegmentWriter::takeClosed() {
    std::vector<ClosedSegment> result;
    result.swap(closed);
    return result;
}
