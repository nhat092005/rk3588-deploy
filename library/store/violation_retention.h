#ifndef VIOLATION_RETENTION_H
#define VIOLATION_RETENTION_H

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "logger.h"

/** @brief Defaults of the retention limits. */
constexpr double defaultMaxStorageGb{4.0};
constexpr int defaultKeepDays{30};
/** @brief Free space that the volume must keep, in bytes (2 GB): below it the oldest violations
 *         go, and when nothing is left to delete no new evidence is written. */
constexpr std::uint64_t defaultMinFreeBytes{2'000'000'000};
/** @brief Bytes in a gigabyte as the option counts them. */
constexpr double bytesPerGigabyte{1'000'000'000.0};

/** @brief The limits to enforce. */
struct RetentionConfig {
    /** The folder of the violations (the root of ViolationStore). It must exist. */
    std::string_view root;
    /** Most bytes the violations may use together, above 0. */
    std::uint64_t maxBytes{0};
    /** Violations older than this many days are removed, at least 1. */
    int keepDays{defaultKeepDays};
    /** Free bytes that the volume of the root must keep. 0 turns this limit off. */
    std::uint64_t minFreeBytes{defaultMinFreeBytes};
};

/**
 * @brief Keeps the folder of the violations within three limits: the age of a violation, the bytes
 *        they use together, and the free space of the volume. It removes the oldest violations
 *        first.
 *
 * It only ever removes what ViolationStore wrote (and the temporary directory it leaves): a
 * directory `<root>/<YYYY-MM-DD>/<HHMMSS>_id<n>_ <NoHelmet|NoVest|NoHelmetNoVest>[_<k>]` that holds
 * files and no directory. Any other file or directory (a hidden temporary directory, a symbolic
 * link, something the user put there) is left alone and not counted. The age comes from the name of
 * the directory, not from its modification time. The violations are kept in an index in memory, so
 * enforce() reads the disk only to remove a violation and to ask the volume how much it has free.
 * Meant for the thread that writes the violations: it is not thread safe. Move-only.
 */
class ViolationRetention {
public:
    /**
     * @brief Removes the hidden temporary directories that ViolationStore leaves when the program
     *        stops while it writes a violation (`.<HHMMSS>_id<n>_<kind>[_<k>].tmp` with only files
     *        in it), then reads the violations that are in the folder.
     * @param config The limits. maxBytes above 0, keepDays at least 1.
     * @param log Logger that the object keeps for the rest of its life.
     * @return The object, or empty when the root is not a directory or a limit is out of range,
     *         which is logged.
     */
    static std::optional<ViolationRetention> create(const RetentionConfig& config,
                                                    const Logger& log);

    /**
     * @brief Reads the folder again: forgets the index and counts what is there now. Call it now
     *        and then to follow files that someone removed or added.
     */
    void rescan();

    /**
     * @brief Counts a violation that ViolationStore just saved, with the size of its files as
     *        they are on the disk.
     * @param directory The path that ViolationStore::save() returned.
     */
    void add(const std::string& directory);

    /**
     * @brief Counts bytes added to a violation (its clip arrived later).
     * @param directory The path that ViolationStore::save() returned.
     * @param bytes Size of the file.
     */
    void addBytes(const std::string& directory, std::uint64_t bytes);

    /**
     * @brief Removes the violations that break a limit, oldest first, and reports whether there is
     *        room to write.
     * @param now The time to measure the age against.
     * @return true when the volume has at least minFreeBytes free after the removal. false when
     *         it does not even after every violation was removed: the caller should not write.
     */
    [[nodiscard]] bool enforce(std::time_t now);

    /** @brief Violations in the index. */
    std::size_t count() const noexcept {
        return entries.size();
    }

    /** @brief Bytes of the violations in the index. */
    std::uint64_t totalBytes() const noexcept {
        return total;
    }

    /** @brief Leftover temporary directories removed by create(). */
    std::size_t sweptCount() const noexcept {
        return swept;
    }

    /** @brief Violations removed since create(). */
    std::size_t removedCount() const noexcept {
        return removed;
    }

private:
    struct Entry {
        std::string directory;
        std::time_t time{0};
        std::uint64_t bytes{0};
    };

    ViolationRetention(const RetentionConfig& config, Logger logger)
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

#endif // VIOLATION_RETENTION_H
