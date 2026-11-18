#ifndef RGA_OVERLAY_H
#define RGA_OVERLAY_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "box_mapper.h"
#include "letterbox_layout.h"
#include "logger.h"
#include "rga.h"

/** @brief Line width of a box outline in pixels, at 4K. */
constexpr int defaultBoxThickness{4};
/** @brief Line width of the outline of a violator, in pixels, at 4K. */
constexpr int violatorThickness{8};
/** @brief Most boxes of one draw() call. Boxes beyond it are not drawn. */
constexpr std::size_t defaultOverlayCapacity{128};

/**
 * @brief How a box is drawn: the colour as the RGA takes it and the line width.
 *
 * The colour is 0xBBGGRR (red in the low byte), the order measured on the board. The RGA turns it
 * into limited range YUV, so on the full range camera image it comes out a little paler.
 */
struct BoxStyle {
    std::uint32_t color{0};
    int thickness{defaultBoxThickness};
};

// Colours by meaning: green for what is in order, red and orange for what is missing, white for
// the person, yellow for the violator. Colour is one channel only: the label names the class.
/** @brief Helmet: green (0, 200, 0). */
constexpr std::uint32_t helmetColor{0x00C800U};
/** @brief NoHelmet: red (255, 40, 40). */
constexpr std::uint32_t noHelmetColor{0x2828FFU};
/** @brief NoVest: orange (255, 140, 0). */
constexpr std::uint32_t noVestColor{0x008CFFU};
/** @brief Vest: teal (0, 190, 170). */
constexpr std::uint32_t vestColor{0xAABE00U};
/** @brief Person: white. */
constexpr std::uint32_t personColor{0xFFFFFFU};
/** @brief A person with a confirmed violation: yellow (255, 230, 0). */
constexpr std::uint32_t violatorColor{0x00E6FFU};
/** @brief A class without a style: gray. */
constexpr std::uint32_t unknownColor{0x808080U};

/** @brief The styles of this project's model by class index: Helmet, NoHelmet, NoVest, Vest,
 *         Person. */
constexpr std::array<BoxStyle, 5> defaultClassStyles{{
    {helmetColor, defaultBoxThickness},
    {noHelmetColor, defaultBoxThickness},
    {noVestColor, defaultBoxThickness},
    {vestColor, defaultBoxThickness},
    {personColor, defaultBoxThickness},
}};

/** @brief What to draw on and with which styles. */
struct OverlayConfig {
    /** Size of the NV12 target image in pixels, both even and above 0. */
    ImageSize size{};
    /** Bytes per line of the luma plane of the target (RgaSnapshot::stride()). */
    int stride{0};
    /** Style by class index. */
    std::vector<BoxStyle> classStyles{defaultClassStyles.begin(), defaultClassStyles.end()};
    /** Style of a violator, which is drawn on top of the other boxes. */
    BoxStyle violatorStyle{violatorColor, violatorThickness};
    /** Style of a class index outside classStyles. */
    BoxStyle unknownStyle{unknownColor, defaultBoxThickness};
    /** Most boxes per draw() call, above 0. */
    std::size_t capacity{defaultOverlayCapacity};
};

/** @brief One box to draw. */
struct OverlayBox {
    /** The box in the pixels of the target image (BoxMapper::toDestination). */
    PixelBox box;
    /** Class index of the detection. */
    int classId{0};
    /** Draw it as a violator: the violator style, on top of everything else. */
    bool violator{false};
    /** Confidence from 0 to 1, for the label (RgaLabels). */
    float score{0.0F};
    /** Track identifier for the label, or below 0 for an object that is not tracked. */
    int trackId{-1};
};

/**
 * @brief Draws box outlines on an NV12 image on the RGA2, the only RGA core that draws. A job
 *        holds 12 boxes, so 34 boxes cost three jobs, about 2 ms at 4K (measured).
 *
 * The coordinates are rounded outwards to even numbers (NV12 chroma is half size), clipped to the
 * image, and a box under 4 pixels on a side is skipped. All storage is allocated by create():
 * draw() allocates nothing.
 *
 * The RGA2 maps only memory below 4 GB, so the target must come from `lowHeap`: a buffer of the
 * `system` heap often has pages above it and is then refused. check() tells at start-up.
 */
class RgaOverlay {
public:
    /**
     * @brief Checks the configuration and allocates the storage.
     * @param config Target geometry, styles and capacity. Every thickness is above 0.
     * @param log Logger that the object keeps for the rest of its life.
     * @return The overlay, or empty on a bad configuration, which is logged.
     */
    static std::optional<RgaOverlay> create(const OverlayConfig& config, const Logger& log);

    /**
     * @brief Tells whether the RGA2 accepts a buffer, by drawing a 4 x 4 black block in its
     *        top-left corner. Call it once per target buffer at start-up.
     * @param target The target buffer.
     * @return false when the RGA refuses it, which is logged with the likely cause.
     */
    [[nodiscard]] bool check(const RgaImage& target) const noexcept;

    /**
     * @brief Draws the outlines.
     * @param target The target buffer, of the size in the configuration.
     * @param boxes The boxes. Violators are drawn last, so they are on top.
     * @return false when the RGA fails, which is logged. Zero boxes draw nothing and succeed.
     */
    [[nodiscard]] bool draw(const RgaImage& target, const std::vector<OverlayBox>& boxes) noexcept;

    /** @brief Boxes not drawn since create() because the capacity was full. */
    std::size_t overflowCount() const noexcept {
        return overflow;
    }

private:
    // A box ready for the RGA, with its style.
    struct Placed {
        int left{0};
        int top{0};
        int width{0};
        int height{0};
        BoxStyle style;
        bool violator{false};
    };

    RgaOverlay(const OverlayConfig& config, const Logger& logger);

    const BoxStyle& styleOf(const OverlayBox& box) const noexcept;
    bool place(const OverlayBox& box) noexcept;
    bool submit(const RgaImage& target) noexcept;

    OverlayConfig settings;
    Logger log;
    std::vector<Placed> placed;
    std::size_t overflow{0};
};

#endif // RGA_OVERLAY_H
