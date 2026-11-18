#ifndef LABEL_TEXT_H
#define LABEL_TEXT_H

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "box_mapper.h"
#include "letterbox_layout.h"

/** @brief The 8x16 font is drawn this many times larger, so a character is 24 x 48 pixels. */
constexpr int labelScale{3};
/** @brief Pixels between the text and the edge of the label. */
constexpr int labelPadding{6};
/** @brief Most characters of a label. A longer text is cut. */
constexpr std::size_t maxLabelChars{24};
/** @brief A label is placed on a multiple of this many pixels, which the RGA wants for a blit. */
constexpr int labelAlignment{4};
/** @brief Bytes of a pixel of the label bitmap: R, G, B, A. */
constexpr std::size_t labelPixelBytes{4};

/** @brief The size of a label in pixels. */
struct LabelSize {
    int width{0};
    int height{0};
};

/** @brief Size of the label of a text of `chars` characters (at most maxLabelChars). */
LabelSize labelSize(std::size_t chars) noexcept;

/** @brief What a label says. */
struct LabelInfo {
    /** Class name, for example "NoHelmet". */
    std::string_view name;
    /** Track identifier, or below 0 when the object is not tracked. */
    int trackId{-1};
    /** Confidence from 0 to 1, shown as a whole percent. */
    float score{0.0F};
    /** A violator starts with "! ". */
    bool violator{false};
};

/**
 * @brief Writes the text of a label: `[! ][#id ]name percent%`, for example `! #13 Person 95%` or
 *        `NoHelmet 87%`. The same structure for every box.
 * @param info The content.
 * @param out Output buffer, at least maxLabelChars + 1 bytes.
 * @param capacity Size of `out`.
 * @return The number of characters written (the text ends with a zero byte after them), at most
 *         maxLabelChars. 0 when the buffer is too small.
 */
std::size_t formatLabel(const LabelInfo& info, char* out, std::size_t capacity) noexcept;

/** @brief The colours of a label, 0xBBGGRR like the colours of the overlay. */
struct LabelColors {
    std::uint32_t background{0};
    std::uint32_t text{0};
};

/** @brief The text colour with the better contrast on a background: black on a light one, white
 *         on a dark one. */
std::uint32_t textColorFor(std::uint32_t background) noexcept;

/**
 * @brief Draws the label into a bitmap of R, G, B, A bytes (alpha 255).
 * @param text The characters. Only ASCII 32 to 126 have a glyph, anything else shows as '?'. At
 *        most maxLabelChars are drawn.
 * @param colors Background and text colour.
 * @param pixels Top-left of the bitmap. It must hold labelSize(text length) pixels.
 * @param strideBytes Bytes from a row to the next, at least the width x labelPixelBytes.
 */
void renderLabel(std::string_view text, const LabelColors& colors, std::uint8_t* pixels,
                 std::size_t strideBytes) noexcept;

/** @brief The top-left corner of a label in the image. */
struct LabelPosition {
    int x{0};
    int y{0};
};

/**
 * @brief Where a label goes: above the box (`below` false) or under it, at the left edge, kept
 *        inside the image, on a multiple of labelAlignment.
 * @param box The box in the pixels of the image.
 * @param below Place it under the box.
 * @param size Size of the label.
 * @param image Size of the image.
 */
LabelPosition placeLabel(const PixelBox& box, bool below, const LabelSize& size,
                         const ImageSize& image) noexcept;

#endif // LABEL_TEXT_H
