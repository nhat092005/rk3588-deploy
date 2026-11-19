#ifndef RECORDING_RETENTION_H
#define RECORDING_RETENTION_H

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "logger.h"

/** @brief Defaults of the limits of the recordings. The default free-space floor is higher than
 * that of the violations (2 GB), so that the video is trimmed before a violation is. */
constexpr double defaultArchiveMaxStorageGb{5.0};
constexpr int defaultArchiveKeepDays{7};
constexpr std::uint64_t defaultArchiveMinFreeBytes{3'000'000'000};

/** @brief The limits to enforce. */
struct RecordingRetentionConfig {
    /** The folder of the recordings (the root of SegmentWriter). It must exist. */
    std::string_view root;
    /** Most bytes the segments may use together, above 0. */
    std::uint64_t maxBytes{0};
    /** Segments older than this many days are removed, at least 1. */
    int keepDays{defaultArchiveKeepDays};
    /** Free bytes that the volume of the root must keep. 0 turns this limit off. */
    std::uint64_t minFreeBytes{defaultArchiveMinFreeBytes};
};

/**
 * @brief Keeps the folder of the recordings within an age, a size and a free-space limit, removing
 *        the oldest segment first. The sibling of ViolationRetention for the files of
 *        SegmentWriter.
 *
 * It only ever removes what SegmentWriter wrote: a regular file `<root>/<YYYY-MM-DD>/<HHMMSS>.h265`
 * (or `<HHMMSS>_<k>.h265`). Any other file or directory, and any symbolic link, is left alone and
 * not counted. At creation it also removes the hidden temporary files that SegmentWriter leaves
 * when the program stops while a segment is open (`.<HHMMSS>[_<k>].h265.tmp`): nothing else writes
 * in the folder, so such a file is garbage. The age comes from the name, not from the
 * modification time. The segments are kept in an index in memory. Not thread safe: use it from the
 * thread that writes the segments. Move-only.
 */
class RecordingRetention {
public:
    /**
     * @brief Removes the leftover temporary files and reads the segments that are in the folder.
     * @param config The limits. maxBytes above 0, keepDays at least 1.
     * @param log Logger that the object keeps for the rest of its life.
     * @return The object, or empty when the root is not a directory or a limit is out of range,
     *         which is logged.
     */
    static std::optional<RecordingRetention> create(const RecordingRetentionConfig& config,
                                                    const Logger& log);

    /** @brief Reads the folder again, forgetting the index (the temporary files stay). */
    void rescan();

    /**
     * @brief Counts a segment that SegmentWriter completed, with its size as it is on the disk.
     * @param path The path of the final file.
     */
    void add(const std::string& path);

    /**
     * @brief Removes the segments that break a limit, oldest first, and reports whether there is
     *        room to write.
     * @param now The time to measure the age against.
     * @return true when the volume has at least minFreeBytes free after the removal. false when
     *         it does not even after every segment was removed.
     */
    [[nodiscard]] bool enforce(std::time_t now);

    /** @brief Segments in the index. */
    std::size_t count() const noexcept {
        return entries.size();
    }

    /** @brief Bytes of the segments in the index. */
    std::uint64_t totalBytes() const noexcept {
        return total;
    }

    /** @brief Segments removed since create(). */
    std::size_t removedCount() const noexcept {
        return removed;
    }

    /** @brief Leftover temporary files removed by create(). */
    std::size_t sweptCount() const noexcept {
        return swept;
    }

private:
    struct Entry {
        std::string path;
        std::time_t time{0};
        std::uint64_t bytes{0};
    };

    RecordingRetention(const RecordingRetentionConfig& config, Logger logger)
        : root{config.root}, maxBytes{config.maxBytes}, keepDays{config.keepDays},
          minFreeBytes{config.minFreeBytes}, log{logger} {}

    void sweepTemporaries();
    void insert(Entry entry);
    void removeOldest();
    bool volumeHasRoom() const;

    std::string root;
    std::uint64_t maxBytes;
    int keepDays;
    std::uint64_t minFreeBytes;
    Logger log;
    // Sorted by time, the oldest first.
    std::vector<Entry> entries;
    std::uint64_t total{0};
    std::size_t removed{0};
    std::size_t swept{0};
};

#endif // RECORDING_RETENTION_H
