#ifndef COLOR_MATRIX_H
#define COLOR_MATRIX_H

#include <cstdint>

/** @brief The matrix that turns the YUV of a frame into RGB. The RGA offers BT.601 (full or
 *         limited range) and BT.709 (limited range only). */
enum class ColorMatrix : std::uint8_t { Bt601, Bt709 };

#endif // COLOR_MATRIX_H
