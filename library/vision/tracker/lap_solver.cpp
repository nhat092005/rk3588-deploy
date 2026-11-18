#include "lap_solver.h"

#include <algorithm>
#include <cstddef>
#include <utility>

// The names of lapjv.cpp are spelled out here: i, j are row and column, c is a reduced cost, v is
// `dual`, d is `distance`, cols is `scanOrder`, pred is `predecessor`.
namespace {
// LARGE of lapjv.h.
constexpr double largeCost{1'000'000.0};
// _carr_dense runs at most this many times before the augmentation (lapjv_internal).
constexpr int maxReductionPasses{2};
// `/ 2.` of the extension, written as a product (exact in binary floating point).
constexpr double half{0.5};
} // namespace

LapSolver::LapSolver(const LapCapacity& capacity) : limits{capacity} {
    const std::size_t side{capacity.rows + capacity.columns};
    extended.assign(side * side, 0.0);
    x.assign(side, -1);
    y.assign(side, -1);
    freeRows.assign(side, 0);
    scanOrder.assign(side, 0);
    predecessor.assign(side, 0);
    dual.assign(side, 0.0);
    distance.assign(side, 0.0);
    unique.assign(side, 0);
    rowMatch.assign(capacity.rows, -1);
    columnMatch.assign(capacity.columns, -1);
}

bool LapSolver::solve(const CostMatrix& cost, double limit) noexcept {
    std::fill(rowMatch.begin(), rowMatch.end(), -1);
    std::fill(columnMatch.begin(), columnMatch.end(), -1);
    if (cost.rows > limits.rows || cost.columns > limits.columns) {
        return false;
    }
    if (cost.rows == 0 || cost.columns == 0) {
        return true;
    }
    // The extended matrix of lap's lapjv(): cost_limit / 2 everywhere, 0 between the extra rows
    // and columns, the real costs in the top left corner.
    const std::size_t side{cost.rows + cost.columns};
    size = static_cast<int>(side);
    std::fill(extended.begin(), extended.begin() + static_cast<std::ptrdiff_t>(side * side),
              limit * half);
    for (std::size_t row = cost.rows; row < side; ++row) {
        std::fill_n(extended.begin() + static_cast<std::ptrdiff_t>((row * side) + cost.columns),
                    side - cost.columns, 0.0);
    }
    for (std::size_t row = 0; row < cost.rows; ++row) {
        for (std::size_t column = 0; column < cost.columns; ++column) {
            extended[(row * side) + column] =
                static_cast<double>(cost.values[(row * cost.columns) + column]);
        }
    }

    // lapjv_internal
    reduceColumns();
    int freeCount{transferReduction()};
    int pass{0};
    while (freeCount > 0 && pass < maxReductionPasses) {
        freeCount = reduceAugmentingRows(freeCount);
        ++pass;
    }
    if (freeCount > 0) {
        augment(freeCount);
    }

    for (std::size_t row = 0; row < cost.rows; ++row) {
        const int column{x[row]};
        rowMatch[row] = static_cast<std::size_t>(column) >= cost.columns ? -1 : column;
    }
    for (std::size_t column = 0; column < cost.columns; ++column) {
        const int row{y[column]};
        columnMatch[column] = static_cast<std::size_t>(row) >= cost.rows ? -1 : row;
    }
    return true;
}

// First half of _ccrrt_dense, column reduction: every column takes the row with its smallest cost,
// a row that is taken by several columns keeps the last of them.
void LapSolver::reduceColumns() noexcept {
    for (int row = 0; row < size; ++row) {
        x[row] = -1;
        dual[row] = largeCost;
        y[row] = 0;
    }
    for (int row = 0; row < size; ++row) {
        for (int column = 0; column < size; ++column) {
            const double value{cost(row, column)};
            if (value < dual[column]) {
                dual[column] = value;
                y[column] = row;
            }
        }
    }
    std::fill(unique.begin(), unique.begin() + size, static_cast<char>(1));
    for (int column = size - 1; column >= 0; --column) {
        const int row{y[column]};
        if (x[row] < 0) {
            x[row] = column;
        } else {
            unique[row] = 0;
            y[column] = -1;
        }
    }
}

// Second half of _ccrrt_dense, reduction transfer: lists the free rows and lowers the dual of the
// column of every row that is the only one in its column. Returns the number of free rows.
int LapSolver::transferReduction() noexcept {
    int freeCount{0};
    for (int row = 0; row < size; ++row) {
        if (x[row] < 0) {
            freeRows[freeCount++] = row;
        } else if (unique[row] != 0) {
            const int assigned{x[row]};
            double minimum{largeCost};
            for (int other = 0; other < size; ++other) {
                if (other == assigned) {
                    continue;
                }
                const double reduced{cost(row, other) - dual[other]};
                if (reduced < minimum) {
                    minimum = reduced;
                }
            }
            dual[assigned] -= minimum;
        }
    }
    return freeCount;
}

LapSolver::BestTwo LapSolver::bestTwoColumns(int row) const noexcept {
    BestTwo best;
    best.first = 0;
    best.firstValue = cost(row, 0) - dual[0];
    best.second = -1;
    best.secondValue = largeCost;
    for (int column = 1; column < size; ++column) {
        const double reduced{cost(row, column) - dual[column]};
        if (reduced < best.secondValue) {
            if (reduced >= best.firstValue) {
                best.secondValue = reduced;
                best.second = column;
            } else {
                best.secondValue = best.firstValue;
                best.firstValue = reduced;
                best.second = best.first;
                best.first = column;
            }
        }
    }
    return best;
}

// _carr_dense: augmenting row reduction.
int LapSolver::reduceAugmentingRows(int freeCount) noexcept {
    int current{0};
    int newFreeCount{0};
    int roundCount{0};
    while (current < freeCount) {
        ++roundCount;
        const int freeRow{freeRows[current++]};
        BestTwo best{bestTwoColumns(freeRow)};
        int previousRow{y[best.first]};
        const double newDual{dual[best.first] - (best.secondValue - best.firstValue)};
        const bool dualLowers{newDual < dual[best.first]};
        if (roundCount < current * size) {
            if (dualLowers) {
                dual[best.first] = newDual;
            } else if (previousRow >= 0 && best.second >= 0) {
                best.first = best.second;
                previousRow = y[best.second];
            }
            if (previousRow >= 0) {
                if (dualLowers) {
                    freeRows[--current] = previousRow;
                } else {
                    freeRows[newFreeCount++] = previousRow;
                }
            }
        } else if (previousRow >= 0) {
            freeRows[newFreeCount++] = previousRow;
        }
        x[freeRow] = best.first;
        y[best.first] = freeRow;
    }
    return newFreeCount;
}

// _find_dense: moves the columns with the smallest distance to the front of the unscanned part.
int LapSolver::findMinimumColumns(int low) noexcept {
    int high{low + 1};
    double minimum{distance[scanOrder[low]]};
    for (int position = high; position < size; ++position) {
        const int column{scanOrder[position]};
        if (distance[column] <= minimum) {
            if (distance[column] < minimum) {
                high = low;
                minimum = distance[column];
            }
            scanOrder[position] = scanOrder[high];
            scanOrder[high++] = column;
        }
    }
    return high;
}

// _scan_dense: scans the columns on the SCAN list and lowers the distance of the TODO columns.
int LapSolver::scanColumns(ScanRange& range) noexcept {
    int low{range.low};
    int high{range.high};
    while (low != high) {
        int column{scanOrder[low++]};
        const int row{y[column]};
        const double minimum{distance[column]};
        const double slack{cost(row, column) - dual[column] - minimum};
        for (int position = high; position < size; ++position) {
            column = scanOrder[position];
            const double reduced{cost(row, column) - dual[column] - slack};
            if (reduced < distance[column]) {
                distance[column] = reduced;
                predecessor[column] = row;
                if (reduced == minimum) {
                    if (y[column] < 0) {
                        return column;
                    }
                    scanOrder[position] = scanOrder[high];
                    scanOrder[high++] = column;
                }
            }
        }
    }
    range.low = low;
    range.high = high;
    return -1;
}

// find_path_dense: one iteration of the modified Dijkstra shortest path, returns the free column.
int LapSolver::findPath(int startRow) noexcept {
    ScanRange range;
    int finalColumn{-1};
    int readyCount{0};
    for (int column = 0; column < size; ++column) {
        scanOrder[column] = column;
        predecessor[column] = startRow;
        distance[column] = cost(startRow, column) - dual[column];
    }
    while (finalColumn == -1) {
        if (range.low == range.high) {
            readyCount = range.low;
            range.high = findMinimumColumns(range.low);
            for (int position = range.low; position < range.high; ++position) {
                const int column{scanOrder[position]};
                if (y[column] < 0) {
                    finalColumn = column;
                }
            }
        }
        if (finalColumn == -1) {
            finalColumn = scanColumns(range);
        }
    }
    const double minimum{distance[scanOrder[range.low]]};
    for (int position = 0; position < readyCount; ++position) {
        const int column{scanOrder[position]};
        dual[column] += distance[column] - minimum;
    }
    return finalColumn;
}

// _ca_dense: augmentation of the free rows.
void LapSolver::augment(int freeCount) noexcept {
    for (int index = 0; index < freeCount; ++index) {
        const int startRow{freeRows[index]};
        int column{findPath(startRow)};
        int row{-1};
        while (row != startRow) {
            row = predecessor[column];
            y[column] = row;
            std::swap(column, x[row]);
        }
    }
}
