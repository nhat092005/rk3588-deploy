#ifndef BOX_MAPPER_H
#define BOX_MAPPER_H

#include <optional>

#include "letterbox_layout.h"

/** @brief A box in pixels: the corners (left, top) and (right, bottom). */
struct PixelBox {
    float left{0.0F};
    float top{0.0F};
    float right{0.0F};
    float bottom{0.0F};
};

/**
 * @brief Turns a box of the model (a pixel position in the letterboxed model input) back into a
 *        box of an upright image. The one place for this conversion: the overlay, the event file
 *        and the display all use it.
 *
 * The camera image is rotated, scaled and padded by RgaLetterbox, so a detection lives in the
 * model input. Undoing the padding and the scale gives the upright source image, the same
 * numbers as rk3588_common.model_io.postprocess ((x - pad) / gain, clipped to the image). The
 * destination is an upright image of the same scene at another size, for example the 4K frame
 * when the model saw the 640x360 stream. The rotation is not handled here: the letterbox layout
 * already describes the rotated source, and the destination must be upright too.
 */
class BoxMapper {
public:
    /**
     * @brief Checks the arguments.
     * @param layout The layout of the letterbox that made the model input, with its source size.
     * @param destination Size of the upright image to map to, both sides above 0.
     * @return The mapper, or empty when the layout has no source, no positive gain or the
     *         destination is not above 0.
     */
    static std::optional<BoxMapper> create(const LetterboxLayout& layout,
                                           ImageSize destination) noexcept;

    /**
     * @brief The box in the pixels of the upright source image.
     * @param model Box in the model input.
     * @return The box, clipped to the source image.
     */
    PixelBox toSource(const PixelBox& model) const noexcept;

    /**
     * @brief The box in the pixels of the destination image.
     * @param model Box in the model input.
     * @return The box, clipped to the destination image.
     */
    PixelBox toDestination(const PixelBox& model) const noexcept;

private:
    BoxMapper(const LetterboxLayout& layoutValue, ImageSize destinationSize) noexcept
        : layout{layoutValue}, destination{destinationSize} {}

    LetterboxLayout layout;
    ImageSize destination;
};

#endif // BOX_MAPPER_H
