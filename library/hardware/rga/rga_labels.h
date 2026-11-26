#ifndef RGA_LABELS_H
#define RGA_LABELS_H

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "dmabuf.h"
#include "label_text.h"
#include "logger.h"
#include "rga.h"
#include "rga_overlay.h"

/** @brief Most labels of one draw() call. Labels beyond it are not drawn. */
constexpr std::size_t defaultLabelCapacity{64};

/** @brief The names and the rules of the labels. */
struct LabelConfig {
    /** Name by class index. The object keeps its own copy. */
    std::vector<std::string> classNames{"Helmet", "NoHelmet",   "NoVest",
                                        "Vest",   "RegularHat", "Person"};
    /** Class index of Person: its label goes under the box, the other labels go above. */
    int personClass{5};
    /** Most labels per draw() call, above 0. */
    std::size_t capacity{defaultLabelCapacity};
};

/**
 * @brief Draws the label of each box on an NV12 image: the CPU renders the labels into an atlas
 *        (one slot per label, RGBA), then the RGA2 copies each slot to its place in jobs.
 *
 * The text, the colours and the places come from label_text.h: `[! ][#id ]name percent%` on the
 * colour of the box, a violator and a Person under the box, the other boxes above it. The colours
 * are the styles of the OverlayConfig. Violator labels are drawn last. The atlas is allocated
 * from the lowHeap by create() (about 10 MB for 64 labels), so draw() allocates nothing.
 */
class RgaLabels {
public:
    /**
     * @brief Checks the configuration, allocates and imports the atlas.
     * @param overlay The geometry and the styles of the overlay, the same as for RgaOverlay.
     * @param labels Names, the class of Person and the capacity. At least one name.
     * @param log Logger that the object keeps for the rest of its life.
     * @return The labels, or empty on a bad configuration or when the atlas cannot be made, which
     *         is logged.
     */
    static std::optional<RgaLabels> create(const OverlayConfig& overlay, const LabelConfig& labels,
                                           const Logger& log);

    /**
     * @brief Draws the labels.
     * @param target The target buffer, of the size in the overlay configuration, from the
     *        lowHeap.
     * @param boxes The boxes, the same list as for RgaOverlay::draw().
     * @return false when the RGA fails, which is logged. Zero boxes draw nothing and succeed.
     */
    [[nodiscard]] bool draw(const RgaImage& target, const std::vector<OverlayBox>& boxes) noexcept;

    /** @brief Labels not drawn since create() because the capacity was full. */
    std::size_t overflowCount() const noexcept {
        return overflow;
    }

private:
    // A rendered label: its slot in the atlas and where it goes in the image.
    struct Placed {
        std::size_t slot{0};
        LabelSize size;
        LabelPosition position;
    };

    RgaLabels(const OverlayConfig& overlayConfig, LabelConfig labelConfig, Logger logger,
              DmaBuf atlasBuffer, RgaImage atlasHandle);

    const BoxStyle& styleOf(const OverlayBox& box) const noexcept;
    std::string_view nameOf(const OverlayBox& box) const noexcept;
    void renderOne(const OverlayBox& box) noexcept;
    void render(const std::vector<OverlayBox>& boxes) noexcept;
    bool blit(const RgaImage& target) noexcept;

    OverlayConfig overlay;
    LabelConfig labels;
    Logger log;
    // The buffer is declared first so that it outlives its handle.
    DmaBuf atlas;
    RgaImage atlasImage;
    std::vector<Placed> placed;
    std::size_t overflow{0};
};

#endif // RGA_LABELS_H
