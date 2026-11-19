#include "violation_store.h"

#include <array>
#include <cerrno>
#include <cstdio>
#include <string>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "handles.h"

namespace {
constexpr int maxNameAttempts{100};
constexpr std::size_t timeBufferBytes{64};
constexpr std::size_t jsonBufferBytes{1024};
constexpr unsigned readBits{0444U};
constexpr unsigned executeShift{2U};
constexpr const char* originalName{"original.jpg"};
constexpr const char* markedName{"marked.jpg"};
constexpr const char* eventName{"event.json"};
constexpr const char* clipName{"clip.h265"};
constexpr const char* clipTempName{".clip.h265.tmp"};

// Directory permissions for a file mode: execute wherever there is read.
unsigned directoryMode(unsigned fileMode) noexcept {
    return fileMode | ((fileMode & readBits) >> executeShift);
}

// "NoHelmet", "NoVest" or "NoHelmetNoVest".
std::string kindName(const ViolationEvent& event) {
    std::string kind;
    if (event.noHelmet) {
        kind += "NoHelmet";
    }
    if (event.noVest) {
        kind += "NoVest";
    }
    return kind;
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

// mkdir with the wanted permissions. An existing directory is fine.
bool makeDirectory(const std::string& path, unsigned mode, const Logger& log) noexcept {
    if (::mkdir(path.c_str(), mode) != 0) {
        const std::error_code error{lastError()};
        if (error.value() != EEXIST) {
            log.error("mkdir ", path, ": ", error);
            return false;
        }
        return true;
    }
    if (::chmod(path.c_str(), mode) != 0) {
        log.error("chmod ", path, ": ", lastError());
        return false;
    }
    return true;
}

// Writes all bytes, flushes the file to disk, sets the permissions.
bool writeFile(const std::string& path, const StoredBytes& bytes, unsigned mode,
               const Logger& log) noexcept {
    FileDescriptor file{::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode), log};
    if (!file.valid()) {
        log.error("open ", path, ": ", lastError());
        return false;
    }
    std::size_t done{0};
    while (done < bytes.size) {
        const ssize_t count{::write(file.get(), bytes.data + done, bytes.size - done)};
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            log.error("write ", path, ": ", lastError());
            return false;
        }
        done += static_cast<std::size_t>(count);
    }
    if (::fchmod(file.get(), mode) != 0 || ::fsync(file.get()) != 0) {
        log.error("fchmod or fsync ", path, ": ", lastError());
        return false;
    }
    const std::error_code closed{file.close()};
    if (closed) {
        log.error("close ", path, ": ", closed);
        return false;
    }
    return true;
}

// Flushes a directory entry (a created or renamed name) to disk.
void syncDirectory(const std::string& path, const Logger& log) noexcept {
    const FileDescriptor directory{::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC), log};
    if (directory.valid() && ::fsync(directory.get()) != 0) {
        log.warn("fsync ", path, ": ", lastError());
    }
}

// The content of event.json.
std::string eventJson(const ViolationRecord& record) {
    const ViolationEvent& event{record.event};
    std::array<char, jsonBufferBytes> text{};
    const std::string wall{formatTime(record.wallTime, "%Y-%m-%dT%H:%M:%S%z")};
    const std::string original{record.original.size > 0 ? std::string{"\""} + originalName + "\""
                                                        : std::string{"null"}};
    const std::string marked{record.marked.size > 0 ? std::string{"\""} + markedName + "\""
                                                    : std::string{"null"}};
    const int length{std::snprintf(
        text.data(), text.size(),
        "{\n"
        "  \"track_id\": %d,\n"
        "  \"no_helmet\": %s,\n"
        "  \"no_vest\": %s,\n"
        "  \"time\": \"%s\",\n"
        "  \"start_seconds\": %.3f,\n"
        "  \"reported_seconds\": %.3f,\n"
        "  \"image\": {\"width\": %d, \"height\": %d},\n"
        "  \"person_box\": {\"left\": %.1f, \"top\": %.1f, \"right\": %.1f, \"bottom\": %.1f},\n"
        "  \"files\": {\"original\": %s, \"marked\": %s}\n"
        "}\n",
        event.trackId, event.noHelmet ? "true" : "false", event.noVest ? "true" : "false",
        wall.c_str(), event.startSeconds, event.nowSeconds, record.imageWidth, record.imageHeight,
        static_cast<double>(event.left), static_cast<double>(event.top),
        static_cast<double>(event.right), static_cast<double>(event.bottom), original.c_str(),
        marked.c_str())};
    return std::string{text.data(), length > 0 ? static_cast<std::size_t>(length) : 0};
}

// Removes the files of a temporary directory and the directory. Used after a failure.
void discard(const std::string& directory) noexcept {
    for (const char* name : {originalName, markedName, eventName}) {
        (void)::unlink((directory + "/" + name).c_str());
    }
    (void)::rmdir(directory.c_str());
}
} // namespace

std::optional<ViolationStore> ViolationStore::create(const StoreConfig& config, const Logger& log) {
    const std::string root{config.root};
    struct stat info {};
    if (::stat(root.c_str(), &info) != 0) {
        log.error("violation root ", root, ": ", lastError());
        return std::nullopt;
    }
    if (!S_ISDIR(info.st_mode)) {
        log.error("violation root ", root, " is not a directory");
        return std::nullopt;
    }
    if (::access(root.c_str(), W_OK | X_OK) != 0) {
        log.error("violation root ", root, " is not writable: ", lastError());
        return std::nullopt;
    }
    log.info("violation root ", root);
    return std::optional<ViolationStore>{ViolationStore{root, config.fileMode, log}};
}

std::optional<std::string> ViolationStore::save(const ViolationRecord& record) const noexcept {
    const std::string kind{kindName(record.event)};
    if (kind.empty() || (record.original.size > 0 && record.original.data == nullptr) ||
        record.imageWidth <= 0 || record.imageHeight <= 0 ||
        (record.marked.size > 0 && record.marked.data == nullptr)) {
        log.error("bad violation record: kind '", kind, "' original ", record.original.size,
                  " bytes, image ", record.imageWidth, "x", record.imageHeight);
        return std::nullopt;
    }
    const unsigned dirMode{directoryMode(fileMode)};
    const std::string dayPath{root + "/" + formatTime(record.wallTime, "%Y-%m-%d")};
    if (!makeDirectory(dayPath, dirMode, log)) {
        return std::nullopt;
    }
    const std::string base{formatTime(record.wallTime, "%H%M%S") + "_id" +
                           std::to_string(record.event.trackId) + "_" + kind};
    // The first name that is free, as a final directory and as a hidden temporary one.
    std::string finalPath;
    std::string tempPath;
    for (int attempt{0}; attempt < maxNameAttempts; ++attempt) {
        const std::string name{attempt == 0 ? base : base + "_" + std::to_string(attempt + 1)};
        finalPath = dayPath;
        finalPath += '/';
        finalPath += name;
        tempPath = dayPath;
        tempPath += "/.";
        tempPath += name;
        tempPath += ".tmp";
        if (!exists(finalPath) && !exists(tempPath)) {
            break;
        }
        finalPath.clear();
    }
    if (finalPath.empty()) {
        log.error("no free name for ", base);
        return std::nullopt;
    }
    if (::mkdir(tempPath.c_str(), dirMode) != 0 || ::chmod(tempPath.c_str(), dirMode) != 0) {
        log.error("mkdir ", tempPath, ": ", lastError());
        return std::nullopt;
    }
    const std::string json{eventJson(record)};
    const bool written{
        (record.original.size == 0 ||
         writeFile(tempPath + "/" + originalName, record.original, fileMode, log)) &&
        (record.marked.size == 0 ||
         writeFile(tempPath + "/" + markedName, record.marked, fileMode, log)) &&
        writeFile(tempPath + "/" + eventName,
                  StoredBytes{reinterpret_cast<const std::uint8_t*>(json.data()), json.size()},
                  fileMode, log)};
    if (!written) {
        discard(tempPath);
        return std::nullopt;
    }
    syncDirectory(tempPath, log);
    if (::rename(tempPath.c_str(), finalPath.c_str()) != 0) {
        log.error("rename ", tempPath, " to ", finalPath, ": ", lastError());
        discard(tempPath);
        return std::nullopt;
    }
    syncDirectory(dayPath, log);
    return finalPath;
}

bool ViolationStore::saveClip(const std::string& directory,
                              const StoredBytes& clip) const noexcept {
    if (clip.size == 0 || clip.data == nullptr) {
        log.error("empty clip for ", directory);
        return false;
    }
    const std::string finalPath{directory + "/" + clipName};
    const std::string tempPath{directory + "/" + clipTempName};
    if (exists(finalPath)) {
        log.error(finalPath, " exists already");
        return false;
    }
    (void)::unlink(tempPath.c_str());
    if (!writeFile(tempPath, clip, fileMode, log)) {
        (void)::unlink(tempPath.c_str());
        return false;
    }
    if (::rename(tempPath.c_str(), finalPath.c_str()) != 0) {
        log.error("rename ", tempPath, " to ", finalPath, ": ", lastError());
        (void)::unlink(tempPath.c_str());
        return false;
    }
    syncDirectory(directory, log);
    return true;
}
