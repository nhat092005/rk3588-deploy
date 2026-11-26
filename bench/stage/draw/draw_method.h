#ifndef BENCH_DRAW_METHOD_H
#define BENCH_DRAW_METHOD_H

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include <opencv2/core.hpp>

#include "letterbox_layout.h"
#include "logger.h"
#include "preprocess_method.h"
#include "rga_overlay.h"
#include "synthetic_frame.h"

/** @brief The ways to draw the boxes that the bench times. */
enum class DrawKind : std::uint8_t {
    /** The two planes of the NV12 frame filled through OpenCV, one thread. */
    Cv1,
    /** The same on 8 threads, one band of rows each. */
    Cv8,
    /** RgaOverlay as the application calls it: the RGA2, 12 boxes per job. */
    Rga2
};

/** @brief Every method, in the order the bench runs them. Rga2 comes first: it is the reference
 *         of the pixel comparison, the way the application draws. */
constexpr std::array<DrawKind, 3> allDrawMethods{DrawKind::Rga2, DrawKind::Cv1, DrawKind::Cv8};

/**
 * @brief The short label of a method in tables and charts.
 * @param kind A method.
 * @return "cv1", "cv8" or "R2".
 */
std::string_view drawLabel(DrawKind kind) noexcept;

/** @brief One box to draw, in the pixels of the frame: even numbers, inside the frame. */
struct DrawBox {
    int left{0};
    int top{0};
    int width{0};
    int height{0};
    /** Index into the class styles (the colour), as a class index. */
    int classId{0};
};

/**
 * @brief The boxes of a point of the bench: the same boxes for the same size and count, so every
 *        method draws the same job. The sizes are those of people in a camera view (3% to 15% of
 *        the width, 10% to 50% of the height), the positions are spread over the frame, the
 *        classes cycle through the six of the model.
 * @param size Size of the frame, both even and at least 64.
 * @param count Number of boxes, at least 0.
 * @return The boxes, each at least 8 pixels on a side and inside the frame.
 */
std::vector<DrawBox> makeDrawBoxes(ImageSize size, int count);

/**
 * @brief One way to draw the box outlines on an NV12 frame in place. Owns its frame (the heap
 *        the RGA2 maps), filled with a pattern, so every method can be compared afterwards.
 *        Move-only.
 *
 * The outline is `defaultBoxThickness` pixels wide, inside the box. The CPU methods fill the Y
 * plane and the Cb Cr plane of the mapped frame with the colour as the RGA2 gets it (BT.601
 * limited range), with the cache sync a device buffer needs: before the write (a device may have
 * written the frame) and after it (a device reads it). The sync belongs to the cost of the method.
 */
class DrawMethod {
public:
    /** @brief What a method draws on. */
    struct Spec {
        /** Size of the frame in pixels, both even, the width a multiple of 16. */
        ImageSize size;
        /** The picture under the boxes. */
        Pattern pattern{Pattern::Gradient};
        /** The dma-heap of the frame: "reserved" lets the RGA2 map it. */
        std::string_view heap{lowHeap};
        /** Keep a second, untouched copy of the frame for refresh(). */
        bool fresh{false};
    };

    /**
     * @brief Builds a method with its frame.
     * @param kind Which method.
     * @param spec Frame size, pattern and heap.
     * @param boxes The boxes to draw, drawn by every run().
     * @param log Logger that the method keeps.
     * @return The method, or empty when the frame or the RGA setup fails, which is logged. A
     *         buffer that the RGA2 refuses is not a failure here: run() then returns false and
     *         failure() says Rejected.
     */
    static std::optional<DrawMethod> create(DrawKind kind, const Spec& spec,
                                            const std::vector<DrawBox>& boxes, const Logger& log);

    /** @brief Which method this is. */
    DrawKind kind() const noexcept {
        return method;
    }

    /**
     * @brief Sets the OpenCV thread count (global to OpenCV).
     * @return The number of threads OpenCV reports (1 for the RGA2).
     */
    int prepare() noexcept;

    /**
     * @brief Puts the untouched picture back into the frame with a device copy (RGA), as a new
     *        camera frame arrives: written by a device, so the CPU finds none of it in its caches.
     *        Not part of the timed work.
     * @return false when the copy fails, which is logged. True and a no-op when the method was
     *         built without `fresh`.
     */
    [[nodiscard]] bool refresh() noexcept;

    /**
     * @brief Draws every box once on the frame.
     * @return false when the work fails, which is logged.
     */
    [[nodiscard]] bool run() noexcept;

    /**
     * @brief Why the last run() or the setup failed.
     * @return Rejected when the RGA2 refused the buffer, Failed otherwise.
     */
    Failure failure() const;

    /**
     * @brief Copies the frame out, after the cache sync that a CPU read needs.
     * @param out Receives the `3 * height / 2` rows of `width` bytes of the NV12 frame.
     * @return false when the sync fails, which is logged.
     */
    [[nodiscard]] bool copyOutput(cv::Mat& out);

private:
    DrawMethod(DrawKind kind, SourceFrame&& owned, const Logger& logger)
        : method{kind}, frame{std::move(owned)}, log{logger} {}

    [[nodiscard]] bool runCpu() noexcept;

    DrawKind method;
    SourceFrame frame;
    Logger log;
    // The boxes in the order of RgaOverlay: grouped by colour, so overlaps resolve the same way.
    std::vector<DrawBox> ordered;
    cv::Mat view;
    int threads{1};
    // The untouched picture that refresh() copies into the frame (a fresh frame).
    std::optional<SourceFrame> pristine;
    std::optional<RgaOverlay> overlay;
    std::vector<OverlayBox> overlayBoxes;
    bool refused{false};
};

#endif // BENCH_DRAW_METHOD_H
