#include "recording_retention.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <system_error>
#include <utility>

#include <sys/statvfs.h>

#include "store_names.h"

namespace {
namespace fs = std::filesystem;

constexpr std::time_t secondsPerDay{86'400};
constexpr std::string_view segmentSuffix{".h265"};
constexpr std::string_view tempSuffix{".h265.tmp"};

bool endsWith(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}

// Whether `stem` is "HHMMSS" or "HHMMSS_<digits>".
bool validStem(std::string_view stem) {
    if (stem.size() < timeNameLength || !allDigits(stem.substr(0, timeNameLength))) {
        return false;
    }
    stem.remove_prefix(timeNameLength);
    return stem.empty() || (stem.size() > 1 && stem[0] == '_' && allDigits(stem.substr(1)));
}

// The "HHMMSS[_k]" of a final name "HHMMSS[_k].h265", or empty.
std::optional<std::string_view> finalStem(std::string_view name) {
    if (!endsWith(name, segmentSuffix)) {
        return std::nullopt;
    }
    const std::string_view stem{name.substr(0, name.size() - segmentSuffix.size())};
    return validStem(stem) ? std::optional<std::string_view>{stem} : std::nullopt;
}

// Whether `name` is ".HHMMSS[_k].h265.tmp".
bool isTemporary(std::string_view name) {
    if (name.empty() || name.front() != '.' || !endsWith(name, tempSuffix)) {
        return false;
    }
    return validStem(name.substr(1, name.size() - 1 - tempSuffix.size()));
}

bool isRealDirectory(const fs::directory_entry& entry) {
    std::error_code error;
    return entry.is_directory(error) && !entry.is_symlink(error) && !error;
}

bool isRealFile(const fs::directory_entry& entry) {
    std::error_code error;
    return entry.is_regular_file(error) && !entry.is_symlink(error) && !error;
}
} // namespace

std::optional<RecordingRetention> RecordingRetention::create(const RecordingRetentionConfig& config,
                                                             const Logger& log) {
    std::error_code error;
    if (config.maxBytes == 0 || config.keepDays < 1 ||
        !fs::is_directory(fs::path{std::string{config.root}}, error)) {
        log.error("bad recording retention config: root ", config.root, ", max ", config.maxBytes,
                  " bytes, keep ", config.keepDays, " days");
        return std::nullopt;
    }
    RecordingRetention retention{config, log};
    retention.sweepTemporaries();
    retention.rescan();
    return std::optional<RecordingRetention>{std::move(retention)};
}

// Removes the hidden temporary files that an interrupted run left in the day directories.
void RecordingRetention::sweepTemporaries() {
    std::error_code error;
    for (const fs::directory_entry& day : fs::directory_iterator{fs::path{root}, error}) {
        if (!isRealDirectory(day) ||
            !parseTime(day.path().filename().string(), "000000").has_value()) {
            continue;
        }
        for (const fs::directory_entry& item : fs::directory_iterator{day.path(), error}) {
            if (!isRealFile(item) || !isTemporary(item.path().filename().string())) {
                continue;
            }
            std::error_code removing;
            if (fs::remove(item.path(), removing) && !removing) {
                ++swept;
                log.warn("recording retention: removed leftover ", item.path().string());
            }
        }
    }
}

void RecordingRetention::insert(Entry entry) {
    total += entry.bytes;
    const auto place{
        std::upper_bound(entries.begin(), entries.end(), entry.time,
                         [](std::time_t time, const Entry& other) { return time < other.time; })};
    entries.insert(place, std::move(entry));
}

void RecordingRetention::rescan() {
    entries.clear();
    total = 0;
    std::error_code error;
    for (const fs::directory_entry& day : fs::directory_iterator{fs::path{root}, error}) {
        if (!isRealDirectory(day)) {
            continue;
        }
        const std::string dayName{day.path().filename().string()};
        for (const fs::directory_entry& item : fs::directory_iterator{day.path(), error}) {
            const std::string name{item.path().filename().string()};
            if (!isRealFile(item) || !finalStem(name)) {
                continue;
            }
            const std::optional<std::time_t> time{parseTime(dayName, name)};
            std::error_code sizing;
            const std::uintmax_t bytes{item.file_size(sizing)};
            if (time && !sizing) {
                insert(Entry{item.path().string(), *time, bytes});
            }
        }
    }
    log.info("recording retention: ", entries.size(), " segments, ", total, " bytes in ", root);
}

void RecordingRetention::add(const std::string& path) {
    const fs::path file{path};
    const std::string name{file.filename().string()};
    const std::string day{file.parent_path().filename().string()};
    std::error_code error;
    const std::uintmax_t bytes{fs::file_size(file, error)};
    const std::optional<std::time_t> time{parseTime(day, name)};
    if (!finalStem(name) || !time || error) {
        log.warn("recording retention: not a segment: ", path);
        return;
    }
    insert(Entry{path, *time, bytes});
}

// Removes the oldest segment and its day directory when that is empty. The entry leaves the index
// even when the removal fails, so that enforce() always ends.
void RecordingRetention::removeOldest() {
    const Entry oldest{std::move(entries.front())};
    entries.erase(entries.begin());
    total -= std::min(total, oldest.bytes);
    std::error_code error;
    if (!fs::remove(fs::path{oldest.path}, error) || error) {
        log.error("recording retention: removing ", oldest.path, " failed: ", error.message());
        return;
    }
    ++removed;
    log.info("recording retention: removed ", oldest.path, " (", oldest.bytes, " bytes)");
    std::error_code ignored;
    fs::remove(fs::path{oldest.path}.parent_path(), ignored);
}

bool RecordingRetention::volumeHasRoom() const {
    if (minFreeBytes == 0) {
        return true;
    }
    struct statvfs info {};
    if (::statvfs(root.c_str(), &info) != 0) {
        return false;
    }
    const std::uint64_t free{static_cast<std::uint64_t>(info.f_bavail) * info.f_frsize};
    return free >= minFreeBytes;
}

bool RecordingRetention::enforce(std::time_t now) {
    const std::time_t cutoff{now - (static_cast<std::time_t>(keepDays) * secondsPerDay)};
    while (!entries.empty() && entries.front().time < cutoff) {
        removeOldest();
    }
    while (!entries.empty() && total > maxBytes) {
        removeOldest();
    }
    while (!entries.empty() && !volumeHasRoom()) {
        removeOldest();
    }
    return volumeHasRoom();
}
