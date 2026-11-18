#include "kalman_filter.h"

#include <cmath>
#include <utility>

namespace kalman {
namespace {
constexpr double positionWeight{1.0 / 20.0};
constexpr double velocityWeight{1.0 / 160.0};
// Standard deviations that do not depend on the height.
constexpr double aspectProcessNoise{1e-2};
constexpr double aspectVelocityProcessNoise{1e-5};
constexpr double aspectMeasurementNoise{1e-1};
// Initial covariance factors.
constexpr double initialPositionFactor{2.0};
constexpr double initialVelocityFactor{10.0};

using Matrix = std::array<double, stateSize * stateSize>;
using Projection = std::array<double, measurementSize * measurementSize>;

double& at(Matrix& matrix, std::size_t row, std::size_t column) noexcept {
    return matrix[(row * stateSize) + column];
}

using Rhs = std::array<double, measurementSize * stateSize>;

// Forward elimination with partial pivoting of `projected * result = rhs` (np.linalg.solve is
// LAPACK gesv). The rows of rhs follow the row swaps.
void eliminate(Projection& projected, Rhs& rhs) noexcept {
    constexpr std::size_t order{measurementSize};
    constexpr std::size_t rhsColumns{stateSize};
    for (std::size_t column = 0; column < order; ++column) {
        std::size_t pivot{column};
        for (std::size_t row = column + 1; row < order; ++row) {
            if (std::fabs(projected[(row * order) + column]) >
                std::fabs(projected[(pivot * order) + column])) {
                pivot = row;
            }
        }
        if (pivot != column) {
            for (std::size_t entry = 0; entry < order; ++entry) {
                std::swap(projected[(pivot * order) + entry], projected[(column * order) + entry]);
            }
            for (std::size_t entry = 0; entry < rhsColumns; ++entry) {
                std::swap(rhs[(pivot * rhsColumns) + entry], rhs[(column * rhsColumns) + entry]);
            }
        }
        for (std::size_t row = column + 1; row < order; ++row) {
            const double factor{projected[(row * order) + column] /
                                projected[(column * order) + column]};
            for (std::size_t entry = column; entry < order; ++entry) {
                projected[(row * order) + entry] -= factor * projected[(column * order) + entry];
            }
            for (std::size_t entry = 0; entry < rhsColumns; ++entry) {
                rhs[(row * rhsColumns) + entry] -= factor * rhs[(column * rhsColumns) + entry];
            }
        }
    }
}

// Back substitution of the triangular system left by eliminate(). The result replaces rhs.
void backSubstitute(const Projection& projected, Rhs& rhs) noexcept {
    constexpr std::size_t order{measurementSize};
    constexpr std::size_t rhsColumns{stateSize};
    for (std::size_t back = order; back > 0; --back) {
        const std::size_t row{back - 1};
        for (std::size_t entry = 0; entry < rhsColumns; ++entry) {
            double value{rhs[(row * rhsColumns) + entry]};
            for (std::size_t column = row + 1; column < order; ++column) {
                value -= projected[(row * order) + column] * rhs[(column * rhsColumns) + entry];
            }
            rhs[(row * rhsColumns) + entry] = value / projected[(row * order) + row];
        }
    }
}

// Solves `projected * result = rhs` for the 8 right-hand sides in the columns of rhs (4 x 8). The
// result replaces rhs.
void solveProjected(Projection projected, Rhs& rhs) noexcept {
    eliminate(projected, rhs);
    backSubstitute(projected, rhs);
}
} // namespace

KalmanState initiate(const Measurement& measurement) noexcept {
    KalmanState state;
    const double height{measurement[3]};
    for (std::size_t i = 0; i < measurementSize; ++i) {
        state.mean[i] = measurement[i];
    }
    const double position{initialPositionFactor * positionWeight * height};
    const double velocity{initialVelocityFactor * velocityWeight * height};
    const std::array<double, stateSize> deviation{
        position, position, aspectProcessNoise,         position,
        velocity, velocity, aspectVelocityProcessNoise, velocity};
    for (std::size_t i = 0; i < stateSize; ++i) {
        at(state.covariance, i, i) = deviation[i] * deviation[i];
    }
    return state;
}

void predict(KalmanState& state) noexcept {
    const double height{state.mean[3]};
    const double position{positionWeight * height};
    const double velocity{velocityWeight * height};
    const std::array<double, stateSize> deviation{
        position, position, aspectProcessNoise,         position,
        velocity, velocity, aspectVelocityProcessNoise, velocity};

    // mean = F * mean with F = [[I, I], [0, I]].
    for (std::size_t i = 0; i < measurementSize; ++i) {
        state.mean[i] += state.mean[i + measurementSize];
    }
    // covariance = F * P * F^T + Q.
    Matrix left{};
    for (std::size_t row = 0; row < stateSize; ++row) {
        for (std::size_t column = 0; column < stateSize; ++column) {
            double value{at(state.covariance, row, column)};
            if (row < measurementSize) {
                value += at(state.covariance, row + measurementSize, column);
            }
            at(left, row, column) = value;
        }
    }
    for (std::size_t row = 0; row < stateSize; ++row) {
        for (std::size_t column = 0; column < stateSize; ++column) {
            double value{at(left, row, column)};
            if (column < measurementSize) {
                value += at(left, row, column + measurementSize);
            }
            at(state.covariance, row, column) = value;
        }
    }
    for (std::size_t i = 0; i < stateSize; ++i) {
        at(state.covariance, i, i) += deviation[i] * deviation[i];
    }
}

void update(KalmanState& state, const Measurement& measurement) noexcept {
    // project: the measurement noise is relative to the height of the predicted box.
    const double position{positionWeight * state.mean[3]};
    const std::array<double, measurementSize> deviation{position, position, aspectMeasurementNoise,
                                                        position};
    Projection projected{};
    for (std::size_t row = 0; row < measurementSize; ++row) {
        for (std::size_t column = 0; column < measurementSize; ++column) {
            projected[(row * measurementSize) + column] = at(state.covariance, row, column);
        }
        projected[(row * measurementSize) + row] += deviation[row] * deviation[row];
    }

    // gain = P[:, :4] * projected^-1, from projected * gain^T = P[:, :4]^T (projected is
    // symmetric).
    Rhs rhs{};
    for (std::size_t measuredRow = 0; measuredRow < measurementSize; ++measuredRow) {
        for (std::size_t stateRow = 0; stateRow < stateSize; ++stateRow) {
            rhs[(measuredRow * stateSize) + stateRow] = at(state.covariance, stateRow, measuredRow);
        }
    }
    solveProjected(projected, rhs);
    // gain(stateRow, measured) = rhs(measured, stateRow), an 8 x 4 matrix.
    const auto gain = [&rhs](std::size_t stateRow, std::size_t measured) {
        return rhs[(measured * stateSize) + stateRow];
    };

    std::array<double, measurementSize> innovation{};
    for (std::size_t i = 0; i < measurementSize; ++i) {
        innovation[i] = measurement[i] - state.mean[i];
    }
    for (std::size_t row = 0; row < stateSize; ++row) {
        double correction{0.0};
        for (std::size_t k = 0; k < measurementSize; ++k) {
            correction += innovation[k] * gain(row, k);
        }
        state.mean[row] += correction;
    }

    // covariance -= gain * projected * gain^T.
    std::array<double, stateSize * measurementSize> gainProjected{};
    for (std::size_t row = 0; row < stateSize; ++row) {
        for (std::size_t column = 0; column < measurementSize; ++column) {
            double value{0.0};
            for (std::size_t k = 0; k < measurementSize; ++k) {
                value += gain(row, k) * projected[(k * measurementSize) + column];
            }
            gainProjected[(row * measurementSize) + column] = value;
        }
    }
    for (std::size_t row = 0; row < stateSize; ++row) {
        for (std::size_t column = 0; column < stateSize; ++column) {
            double value{0.0};
            for (std::size_t k = 0; k < measurementSize; ++k) {
                value += gainProjected[(row * measurementSize) + k] * gain(column, k);
            }
            at(state.covariance, row, column) -= value;
        }
    }
}

} // namespace kalman
