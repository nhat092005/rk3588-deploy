#include "violation_retention.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <system_error>
#include <utility>

#include <sys/statvfs.h>

#include "store_names.h"

namespace {
namespace fs = std::filesystem;

constexpr std::time_t secondsPerDay{86'400};

// Whether the rest of a violation name after "HHMMSS" is "_id<digits>_<kind>" with an optional
// "_<digits>" suffix.
bool validTail(std::string_view tail) {
    constexpr std::string_view marker{"_id"};
    if (tail.substr(0, marker.size()) != marker) {
        return false;
    }
    tail.remove_prefix(marker.size());
    std::size_t digits{0};
    while (digits < tail.size() && std::isdigit(static_cast<unsigned char>(tail[digits])) != 0) {
        ++digits;
    }
    if (digits == 0 || digits >= tail.size() || tail[digits] != '_') {
        return false;
    }
    tail.remove_prefix(digits + 1);
    for (const std::string_view kind : {"NoHelmetNoVest", "NoHelmet", "NoVest"}) {
        if (tail.substr(0, kind.size()) != kind) {
            continue;
        }
        const std::string_view suffix{tail.substr(kind.size())};
        return suffix.empty() ||
               (suffix.size() > 1 && suffix[0] == '_' && allDigits(suffix.substr(1)));
    }
    return false;
}

// The bytes of the regular files of a directory, or empty when it holds anything else (a
// directory, a link): then it is not ours.
std::optional<std::uint64_t> sizeOfFiles(const fs::path& directory) {
    std::error_code error;
    std::uint64_t bytes{0};
    for (const fs::directory_entry& entry : fs::directory_iterator{directory, error}) {
        std::error_code status;
        if (!entry.is_regular_file(status) || entry.is_symlink(status)) {
            return std::nullopt;
        }
        const std::uintmax_t size{entry.file_size(status)};
        if (status) {
            return std::nullopt;
        }
        bytes += size;
    }
    if (error) {
        return std::nullopt;
    }
    return bytes;
}

// Whether `name` is ".<HHMMSS>_id<n>_<kind>[_<k>].tmp" under the day directory `day`.
bool isTemporaryViolation(const std::string& day, std::string_view name) {
    constexpr std::string_view suffix{".tmp"};
    if (name.size() <= suffix.size() + 1 || name.front() != '.' ||
        name.substr(name.size() - suffix.size()) != suffix) {
        return false;
    }
    const std::string_view inner{name.substr(1, name.size() - 1 - suffix.size())};
    return parseTime(day, inner).has_value() && inner.size() > timeNameLength &&
           validTail(inner.substr(timeNameLength));
}

bool isRealDirectory(const fs::directory_entry& entry) {
    std::error_code error;
    return entry.is_directory(error) && !entry.is_symlink(error) && !error;
}
} // namespace

std::optional<ViolationRetention> ViolationRetention::create(const RetentionConfig& config,
                                                             const Logger& log) {
    std::error_code error;
    if (config.maxBytes == 0 || config.keepDays < 1 ||
        !fs::is_directory(fs::path{std::string{config.root}}, error)) {
        log.error("bad retention config: root ", config.root, ", max ", config.maxBytes,
                  " bytes, keep ", config.keepDays, " days");
        return std::nullopt;
    }
    ViolationRetention retention{config, log};
    retention.sweepTemporaries();
    retention.rescan();
    return std::optional<ViolationRetention>{std::move(retention)};
}

// Removes the hidden temporary directories that an interrupted run left in the day directories.
void ViolationRetention::sweepTemporaries() {
    std::error_code error;
    for (const fs::directory_entry& day : fs::directory_iterator{fs::path{root}, error}) {
        if (!isRealDirectory(day)) {
            continue;
        }
        const std::string dayName{day.path().filename().string()};
        for (const fs::directory_entry& item : fs::directory_iterator{day.path(), error}) {
            if (!isRealDirectory(item) ||
                !isTemporaryViolation(dayName, item.path().filename().string()) ||
                !sizeOfFiles(item.path())) {
                continue;
            }
            std::error_code removing;
            fs::remove_all(item.path(), removing);
            if (!removing) {
                ++swept;
                log.warn("retention: removed leftover ", item.path().string());
            }
        }
    }
}

void ViolationRetention::insert(Entry entry) {
    total += entry.bytes;
    const auto place{
        std::upper_bound(entries.begin(), entries.end(), entry.time,
                         [](std::time_t time, const Entry& other) { return time < other.time; })};
    entries.insert(place, std::move(entry));
}

void ViolationRetention::rescan() {
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
            const std::optional<std::time_t> time{parseTime(dayName, name)};
            if (!time || !isRealDirectory(item) || !validTail(name.substr(timeNameLength))) {
                continue;
            }
            const std::optional<std::uint64_t> bytes{sizeOfFiles(item.path())};
            if (bytes) {
                insert(Entry{item.path().string(), *time, *bytes});
            }
        }
    }
    log.info("retention: ", entries.size(), " violations, ", total, " bytes in ", root);
}

void ViolationRetention::add(const std::string& directory) {
    const fs::path path{directory};
    const std::string name{path.filename().string()};
    const std::string day{path.parent_path().filename().string()};
    const std::optional<std::time_t> time{parseTime(day, name)};
    if (!time) {
        log.warn("retention: not a violation directory: ", directory);
        return;
    }
    const std::optional<std::uint64_t> bytes{sizeOfFiles(path)};
    if (!bytes) {
        log.warn("retention: cannot size ", directory);
        return;
    }
    insert(Entry{directory, *time, *bytes});
}

void ViolationRetention::addBytes(const std::string& directory, std::uint64_t bytes) {
    const auto found{std::find_if(entries.begin(), entries.end(), [&directory](const Entry& entry) {
        return entry.directory == directory;
    })};
    if (found != entries.end()) {
        found->bytes += bytes;
        total += bytes;
    }
}

// Removes the files of the oldest violation and its directory, and the day directory when it is
// empty. The entry leaves the index even when a removal fails, so that enforce() always ends.
void ViolationRetention::removeOldest() {
    const Entry oldest{std::move(entries.front())};
    entries.erase(entries.begin());
    total -= std::min(total, oldest.bytes);
    std::error_code error;
    const std::uintmax_t count{fs::remove_all(fs::path{oldest.directory}, error)};
    if (error) {
        log.error("retention: removing ", oldest.directory, " failed: ", error.message());
        return;
    }
    ++removed;
    log.info("retention: removed ", oldest.directory, " (", count, " entries, ", oldest.bytes,
             " bytes)");
    // An empty day directory goes too; a day directory with other content stays.
    std::error_code ignored;
    fs::remove(fs::path{oldest.directory}.parent_path(), ignored);
}

bool ViolationRetention::volumeHasRoom() const {
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

bool ViolationRetention::enforce(std::time_t now) {
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
