#ifndef HEVC_NAL_H
#define HEVC_NAL_H

#include <cstddef>
#include <cstdint>

/** @brief NAL unit types of H.265 that the pipeline looks at (ITU-T H.265 table 7-1). */
enum class NalType : std::uint8_t { IdrWithRadl = 19, IdrNoRadl = 20, Vps = 32 };

/**
 * @brief Whether an Annex B stream holds a NAL unit of the given type.
 * @param data Start of the bytes, may be null only when `length` is 0.
 * @param length Number of bytes.
 * @param type NAL unit type to look for.
 * @return True when a start code (00 00 01) is followed by a NAL header of this type.
 */
bool hasNalType(const std::uint8_t* data, std::size_t length, NalType type) noexcept;

/**
 * @brief Whether an Annex B stream holds an IDR picture (a key frame).
 * @param data Start of the bytes, may be null only when `length` is 0.
 * @param length Number of bytes.
 */
bool hasKeyFrame(const std::uint8_t* data, std::size_t length) noexcept;

#endif // HEVC_NAL_H
