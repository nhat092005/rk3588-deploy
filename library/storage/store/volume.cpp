#include "volume.h"

#include <algorithm>

#include <sys/stat.h>
#include <sys/statvfs.h>

bool overCommitted(const std::vector<FolderBudget>& folders, std::uint64_t freeBytes) {
    std::uint64_t budgets{0};
    std::uint64_t used{0};
    std::uint64_t floor{0};
    for (const FolderBudget& folder : folders) {
        budgets += folder.budgetBytes;
        used += folder.usedBytes;
        floor = std::max(floor, folder.floorBytes);
    }
    return budgets + floor > freeBytes + used;
}

std::optional<std::uint64_t> volumeFreeBytes(const std::string& path) {
    struct statvfs info {};
    if (::statvfs(path.c_str(), &info) != 0) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(info.f_bavail) * info.f_frsize;
}

bool sameVolume(const std::string& first, const std::string& second) {
    struct stat firstInfo {};
    struct stat secondInfo {};
    return ::stat(first.c_str(), &firstInfo) == 0 && ::stat(second.c_str(), &secondInfo) == 0 &&
           firstInfo.st_dev == secondInfo.st_dev;
}
