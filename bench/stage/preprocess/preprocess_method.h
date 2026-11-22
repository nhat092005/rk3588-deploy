#ifndef BENCH_PREPROCESS_METHOD_H
#define BENCH_PREPROCESS_METHOD_H

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

#include <opencv2/core.hpp>

#include "dmabuf.h"
#include "letterbox_layout.h"
#include "logger.h"
#include "rga.h"
#include "rga_forced.h"
#include "synthetic_frame.h"

/** @brief The ways to do the stage 1 job that the bench times. */
enum class MethodKind : std::uint8_t {
    /** cv::cvtColor, cv::resize, cv::rotate on one thread. */
    Cv1,
    /** The same on 8 threads. */
    Cv8,
    /** RGA2 only. */
    Rga2,
    /** RGA3 core 0 only. */
    Rga30,
    /** RGA3 core 1 only. */
    Rga31,
    /** RGA with the core left to the kernel: RgaLetterbox as the application calls it. */
    RgaAuto
};

/** @brief Every method, in the order the bench runs them. Cv1 comes first: it is the reference of
 *         the pixel comparison. */
constexpr std::array<MethodKind, 6> allMethods{MethodKind::Cv1,   MethodKind::Cv8,
                                               MethodKind::Rga2,  MethodKind::Rga30,
                                               MethodKind::Rga31, MethodKind::RgaAuto};

/**
 * @brief The short label of a method in tables and charts.
 * @param kind A method.
 * @return "cv1", "cv8", "R2", "R30", "R31" or "Ra".
 */
std::string_view methodLabel(MethodKind kind) noexcept;

/** @brief The NV12 input frame that every method reads. Move-only. */
struct SourceFrame {
    DmaBuf buffer;
    RgaImage image;
    /** Size in pixels. The stride equals the width. */
    ImageSize size;
};

/**
 * @brief Allocates an NV12 frame and draws a pattern into it.
 * @param pattern The picture.
 * @param size Size in pixels, both even, the width a multiple of 16 (the RGA stride rule).
 * @param heap The dma-heap of the frame. "reserved" lets the RGA2 map it, "system" is where the
 *        camera frames live.
 * @param log Logger of the buffer.
 * @return The frame, or empty when the size is rejected or the allocation fails, which is logged.
 */
std::optional<SourceFrame> makeSourceFrame(Pattern pattern, ImageSize size, std::string_view heap,
                                           const Logger& log);

/**
 * @brief One way to turn the frame into the 640 x 640 RGB model input: rotate 180 degrees, scale
 *        to fit, place at the centre of a target that is padded once. Owns its own target buffer
 *        (the "reserved" heap, as the NPU input in the application), so every method can be
 *        compared with the others afterwards. Move-only.
 *
 * The CPU methods read the frame through the mapping and write the target through its mapping,
 * with the cache sync that a device buffer needs: before the read of the frame (a device wrote
 * it) and after the write of the target (a device reads it). The sync belongs to the cost of the
 * method. The RGA methods need no CPU sync.
 */
class PreprocessMethod {
public:
    /**
     * @brief Builds a method for a frame.
     * @param kind Which method.
     * @param source The frame. It must outlive the method and not be moved.
     * @param log Logger that the method keeps.
     * @return The method, or empty when the layout, a buffer or the RGA setup fails, which is
     *         logged.
     */
    static std::optional<PreprocessMethod> create(MethodKind kind, SourceFrame& source,
                                                  const Logger& log);

    /** @brief Which method this is. */
    MethodKind kind() const noexcept {
        return method;
    }

    /**
     * @brief Gets the method ready to run: sets the OpenCV thread count and pads the target.
     * @return The number of threads OpenCV reports (1 for an RGA method), or empty when the
     *         padding fails, which is logged. Call it before a method runs, as the thread count
     *         is global to OpenCV.
     */
    [[nodiscard]] std::optional<int> prepare() noexcept;

    /**
     * @brief Converts one frame.
     * @return false when the work fails, which is logged.
     */
    [[nodiscard]] bool run() noexcept;

    /**
     * @brief Copies the target out, after the cache sync that a CPU read needs.
     * @param out Receives a 640 x 640 image of 3 channels, 8 bits, in the order the NPU reads.
     * @return false when the sync fails, which is logged.
     */
    [[nodiscard]] bool copyOutput(cv::Mat& out) const;

private:
    PreprocessMethod(MethodKind kind, const SourceFrame& frame, DmaBuf&& buffer, RgaImage&& image)
        : method{kind}, source{&frame}, target{std::move(buffer)}, targetImage{std::move(image)} {}

    [[nodiscard]] bool runCpu();

    MethodKind method;
    const SourceFrame* source;
    // The buffer is declared before its RGA handle, which is released first.
    DmaBuf target;
    RgaImage targetImage;
    cv::Mat targetView;
    int threads{1};
    // CPU work: the NV12 frame, the scratch images and where the result goes in the target.
    cv::Mat frameView;
    cv::Mat fullRgb;
    cv::Mat smallRgb;
    cv::Mat placed;
    // RGA work.
    std::optional<RgaLetterbox> kernelChoice;
    std::optional<RgaForcedLetterbox> forcedCore;
};

#endif // BENCH_PREPROCESS_METHOD_H
