#ifndef KALMAN_FILTER_H
#define KALMAN_FILTER_H

#include <array>
#include <cstddef>

/** @brief Number of values of a measurement: centre x, centre y, aspect ratio, height. */
constexpr std::size_t measurementSize{4};
/** @brief Number of values of the state: the measurement and its velocity. */
constexpr std::size_t stateSize{8};

/** @brief A box as the filter sees it: centre x, centre y, aspect ratio (width / height), height.
 */
using Measurement = std::array<double, measurementSize>;

/**
 * @brief Mean and covariance of one track. The mean is (x, y, a, h, vx, vy, va, vh), the
 *        covariance is 8 x 8, row-major.
 */
struct KalmanState {
    std::array<double, stateSize> mean{};
    std::array<double, stateSize * stateSize> covariance{};
};

/**
 * Constant velocity Kalman filter of a bounding box, a port of `KalmanFilterXYAH` of Ultralytics
 * 8.4.154 (AGPL-3.0): the time step is 1 frame, the noise is relative to the box height
 * (1/20 for the position, 1/160 for the velocity). Functions of this namespace allocate nothing.
 */
namespace kalman {

/** @brief The state of a new track: the measurement, zero velocity, the initial covariance. */
KalmanState initiate(const Measurement& measurement) noexcept;

/** @brief Moves the state one frame forward (`predict` of the Python filter). */
void predict(KalmanState& state) noexcept;

/** @brief Corrects the state with a measurement (`update` of the Python filter). */
void update(KalmanState& state, const Measurement& measurement) noexcept;

} // namespace kalman

#endif // KALMAN_FILTER_H
