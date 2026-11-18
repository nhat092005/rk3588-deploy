#ifndef LAP_SOLVER_H
#define LAP_SOLVER_H

#include <cstddef>
#include <vector>

/** @brief Largest matrix a LapSolver accepts. */
struct LapCapacity {
    std::size_t rows{0};
    std::size_t columns{0};
};

/** @brief A row-major matrix of costs: `values[row * columns + column]`. */
struct CostMatrix {
    const float* values{nullptr};
    std::size_t rows{0};
    std::size_t columns{0};
};

/** @brief The columns `[low, high)` of the scan order that are being scanned. */
struct ScanRange {
    int low{0};
    int high{0};
};

/**
 * @brief Linear assignment with a cost limit. It gives the same result as
 *        `lap.lapjv(cost, extend_cost=True, cost_limit=limit)` of the lap package 0.5.14, which
 *        is what Ultralytics ByteTrack calls. The code is a port of lap's lapjv.cpp
 *        (Jonker-Volgenant, BSD-2-Clause, (c) 2012-2017 Tomas Kazmar and contributors), kept
 *        line for line so that ties between equal costs break the same way.
 *
 * The matrix is extended to (rows + columns) square with `limit / 2` for the pairs that do not
 * exist and 0 between the extra rows and columns, so a pair above the limit is never assigned.
 * All storage is allocated by the constructor: solve() allocates nothing.
 */
class LapSolver {
public:
    /** @brief Allocates the work arrays for matrices up to `capacity`. */
    explicit LapSolver(const LapCapacity& capacity);

    /**
     * @brief Solves one assignment problem.
     * @param cost The costs. The rows and columns are at most the capacity.
     * @param limit A pair is assigned only if the optimum needs it and its cost is within the
     *        limit. The value is the `thresh` of Ultralytics `linear_assignment`.
     * @return false when the matrix is larger than the capacity, nothing is then assigned. A
     *         matrix without rows or columns leaves everything unassigned.
     */
    [[nodiscard]] bool solve(const CostMatrix& cost, double limit) noexcept;

    /** @brief Column assigned to a row of the last solve(), or -1. */
    int columnOfRow(std::size_t row) const noexcept {
        return rowMatch[row];
    }

    /** @brief Row assigned to a column of the last solve(), or -1. */
    int rowOfColumn(std::size_t column) const noexcept {
        return columnMatch[column];
    }

private:
    // The functions below are the ones of lapjv.cpp, working on the extended matrix.
    void reduceColumns() noexcept;
    int transferReduction() noexcept;
    int reduceAugmentingRows(int freeCount) noexcept;
    void augment(int freeCount) noexcept;
    int findPath(int startRow) noexcept;
    int findMinimumColumns(int low) noexcept;
    int scanColumns(ScanRange& range) noexcept;

    // The two columns with the smallest reduced cost in a row (the first of them is `first`).
    struct BestTwo {
        int first{0};
        double firstValue{0.0};
        int second{-1};
        double secondValue{0.0};
    };
    BestTwo bestTwoColumns(int row) const noexcept;

    double cost(int row, int column) const noexcept {
        return extended[(static_cast<std::size_t>(row) * static_cast<std::size_t>(size)) +
                        static_cast<std::size_t>(column)];
    }

    LapCapacity limits;
    // Side of the extended matrix of the current problem.
    int size{0};
    std::vector<double> extended;
    // x: column of a row, y: row of a column, in the extended matrix.
    std::vector<int> x;
    std::vector<int> y;
    std::vector<int> freeRows;
    std::vector<int> scanOrder;
    std::vector<int> predecessor;
    std::vector<double> dual;
    std::vector<double> distance;
    std::vector<char> unique;
    std::vector<int> rowMatch;
    std::vector<int> columnMatch;
};

#endif // LAP_SOLVER_H
