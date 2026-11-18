#ifndef ROTATION_H
#define ROTATION_H

#include <cstdint>
#include <optional>

/**
 * @brief A rotation, clockwise, applied to the camera image before the letterbox.
 *
 * Set it where the camera is mounted: a camera fitted upside down needs Rotate180.
 */
enum class Rotation : std::uint8_t { None, Rotate90, Rotate180, Rotate270 };

/**
 * @brief Reads a rotation given in degrees clockwise, the text of `--rotation`.
 * @param text One of "0", "90", "180" or "270". Not null.
 * @return The rotation, or empty for any other text. The one place that turns the text into a
 *         Rotation.
 */
std::optional<Rotation> parseRotation(const char* text) noexcept;

#endif // ROTATION_H
