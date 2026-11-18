#ifndef STAGE_STATS_H
#define STAGE_STATS_H

#include <cstddef>
#include <vector>

/**
 * @brief The times of one stage over a window of frames: the median and the largest. All storage
 *        is allocated by the constructor: add() and median() allocate nothing.
 */
class StageStats {
public:
    /**
     * @brief Reserves room for `capacity` samples.
     * @param capacity Most samples of a window, 0 is treated as 1. A sample added to a full window
     *        is ignored.
     */
    explicit StageStats(std::size_t capacity);

    /** @brief Adds one sample, for example a time in milliseconds. */
    void add(double value) noexcept;

    /** @brief Number of samples since the last clear(). */
    std::size_t count() const noexcept {
        return samples.size();
    }

    /** @brief The median (the upper middle one for an even count), 0 for an empty window. The
     *         samples are reordered. */
    double median() noexcept;

    /** @brief The largest sample, 0 for an empty window. */
    double maximum() const noexcept {
        return largest;
    }

    /** @brief Empties the window. */
    void clear() noexcept;

private:
    std::size_t limit;
    std::vector<double> samples;
    double largest{0.0};
};

#endif // STAGE_STATS_H
