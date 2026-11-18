#ifndef VOLUME_H
#define VOLUME_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

/** @brief The limits of one folder, for checking that the limits of the folders fit the disk. */
struct FolderBudget {
    /** Most bytes the folder may use. */
    std::uint64_t budgetBytes{0};
    /** Free bytes that the volume must keep before this folder is trimmed. */
    std::uint64_t floorBytes{0};
    /** Bytes the folder uses now. */
    std::uint64_t usedBytes{0};
};

/**
 * @brief Whether the folders of one volume can reach their limits together: the sum of their
 *        budgets plus the highest floor is more than the free space plus what they use now.
 * @param folders The limits of the folders that share the volume.
 * @param freeBytes Free bytes of the volume now.
 * @return true when the limits cannot all be reached: the free-space floor would trim a folder
 *         before it reaches its own budget.
 */
bool overCommitted(const std::vector<FolderBudget>& folders, std::uint64_t freeBytes);

/**
 * @brief Free bytes that an ordinary user can use on the volume of a path (statvfs).
 * @param path A path on the volume.
 * @return The bytes, or empty when the volume cannot be read.
 */
std::optional<std::uint64_t> volumeFreeBytes(const std::string& path);

/**
 * @brief Whether two paths are on the same volume (the same device).
 * @return true when both exist and share a device.
 */
bool sameVolume(const std::string& first, const std::string& second);

#endif // VOLUME_H
