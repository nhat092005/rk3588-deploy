#ifndef RGA_H
#define RGA_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

#include "dmabuf.h"
#include "letterbox_layout.h"
#include "logger.h"
#include "rotation.h"

/**
 * @brief A DmaBuf imported into the RGA driver. Move-only, the destructor releases the handle.
 *
 * Import once per buffer and keep the object: an import per frame costs a driver call.
 */
class RgaImage {
public:
    /**
     * @brief Imports the DMA-BUF descriptor of a buffer (importbuffer_fd).
     * @param buffer The buffer. It must outlive the returned object.
     * @param log Logger that reports a failure to release the handle.
     * @return The image, or empty when RGA rejects the descriptor, which is logged.
     */
    static std::optional<RgaImage> import(const DmaBuf& buffer, const Logger& log);

    RgaImage(const RgaImage&) = delete;
    RgaImage& operator=(const RgaImage&) = delete;
    RgaImage(RgaImage&& other) noexcept;
    RgaImage& operator=(RgaImage&& other) noexcept;
    ~RgaImage();

    /** @brief The RGA driver handle of the buffer. 0 only in a moved-from image. */
    std::uint32_t handle() const noexcept {
        return imported;
    }

private:
    RgaImage(std::uint32_t handleValue, Logger logger) noexcept
        : imported{handleValue}, log{logger} {}

    void release() noexcept;

    std::uint32_t imported{0};
    Logger log;
};

/** @brief Side of the model input in pixels (rk3588_common.model_io.IMGSZ). */
constexpr int defaultModelSize{640};

/** @brief What RgaLetterbox converts. */
struct LetterboxConfig {
    /** Size of the NV12 camera frame in pixels. */
    ImageSize source{};
    /** Bytes per line of the NV12 frame (Camera::format().bytesPerLine). */
    int sourceStride{0};
    /** Side of the square RGB output in pixels, the model input size. */
    int targetSize{defaultModelSize};
    /** Rotation of the camera image. */
    Rotation rotation{Rotation::None};
    /** Whether the YUV values use the full range 0 to 255 (CameraFormat::fullRange). The
     *  conversion to RGB differs: a full range image decoded as limited loses the dark and bright
     *  ends. The RGA improcess() always decodes as limited, so a full range source costs a second
     *  call (imcvtcolor into a staging buffer of the source size). */
    bool fullRange{false};
};

/**
 * @brief Turns an NV12 camera frame into the letterboxed RGB input of the model on the RGA: one
 *        call does the rotation, the scale, the colour conversion and the placement. No CPU work.
 *
 * The output is RGB888, `targetSize` x `targetSize`, with the image placed as
 * rk3588_common.model_io.preprocess places it and the padding set to 114. Move-only: a full
 * range source needs a staging buffer, which the object owns.
 */
class RgaLetterbox {
public:
    /**
     * @brief Checks the configuration and computes the layout.
     * @param config Source geometry, target size and rotation.
     * @param log Logger that the object keeps for the rest of its life.
     * @return The converter, or empty when a size is not above 0 or the stride is shorter than
     *         the width, which is logged.
     */
    static std::optional<RgaLetterbox> create(const LetterboxConfig& config, const Logger& log);

    /**
     * @brief Where the rotated source sits in the output.
     * @return The layout of the source after rotation. For Rotate90 and Rotate270 the width and
     *         height of the source are swapped first.
     */
    const LetterboxLayout& layout() const noexcept {
        return placement;
    }

    /**
     * @brief Fills the whole target with the padding value 114.
     * @param target RGB output buffer of targetSize x targetSize x 3 bytes.
     * @return false when RGA fails, which is logged.
     *
     * Call it once per target buffer: run() never writes the padding, so it stays.
     */
    [[nodiscard]] bool fillPadding(const RgaImage& target) const noexcept;

    /**
     * @brief Converts one frame.
     * @param source NV12 frame buffer, of the size in the configuration.
     * @param target Output buffer, already padded by fillPadding().
     * @return false when RGA fails, which is logged. The call waits until the RGA is done.
     */
    [[nodiscard]] bool run(const RgaImage& source, const RgaImage& target) const noexcept;

private:
    RgaLetterbox(const LetterboxConfig& config, const LetterboxLayout& layout, Logger logger,
                 std::optional<DmaBuf> stagingBuffer, std::optional<RgaImage> stagingHandle)
        : settings{config}, placement{layout}, log{logger}, staging{std::move(stagingBuffer)},
          stagingImage{std::move(stagingHandle)} {}

    // NV12 full range to RGB888 of the source size, into the staging buffer.
    [[nodiscard]] bool convertFullRange(const RgaImage& source,
                                        const RgaImage& staged) const noexcept;

    LetterboxConfig settings;
    LetterboxLayout placement;
    Logger log;
    // Only for a full range source. The buffer is declared first so that it outlives its handle.
    std::optional<DmaBuf> staging;
    std::optional<RgaImage> stagingImage;
};

/** @brief What RgaSnapshot copies. */
struct SnapshotConfig {
    /** Size of the NV12 camera frame in pixels. */
    ImageSize source{};
    /** Bytes per line of the NV12 frame (Camera::format().bytesPerLine). */
    int sourceStride{0};
    /** Rotation of the camera image. */
    Rotation rotation{Rotation::None};
};

/**
 * @brief Copies an NV12 camera frame into a buffer of its own, rotated upright, on the RGA: one
 *        call, no CPU work. The camera buffer can go back to the camera as soon as run() returns.
 *
 * The copy is NV12: the luma plane is `stride()` bytes per line and `size().height` lines, the
 * chroma plane follows it. The RGA wants the stride to be a multiple of 16, so a width that is not
 * one (a 360 wide image after a quarter turn) gets padding bytes at the end of each line.
 */
class RgaSnapshot {
public:
    /**
     * @brief Checks the configuration.
     * @param config Source geometry and rotation.
     * @param log Logger that the object keeps for the rest of its life.
     * @return The copier, or empty when a size is not above 0, the stride is shorter than the
     *         width or the width or height is odd, which is logged.
     */
    static std::optional<RgaSnapshot> create(const SnapshotConfig& config, const Logger& log);

    /** @brief Size of the copy in pixels: the source size, swapped for Rotate90 and Rotate270. */
    ImageSize size() const noexcept {
        return copySize;
    }

    /** @brief Bytes per line of the copy: the width rounded up to a multiple of 16. */
    int stride() const noexcept;

    /** @brief Bytes that the target buffer needs: stride x height x 3 / 2. */
    std::size_t bytes() const noexcept;

    /**
     * @brief Copies and rotates one frame.
     * @param source NV12 frame buffer, of the size in the configuration.
     * @param target Buffer of at least bytes() bytes.
     * @return false when RGA fails, which is logged. The call waits until the RGA is done.
     */
    [[nodiscard]] bool run(const RgaImage& source, const RgaImage& target) const noexcept;

    /**
     * @brief Starts the copy and returns at once, the RGA works while the caller does something
     *        else.
     * @param source NV12 frame buffer, of the size in the configuration. It must not change and
     *        must not go back to the camera before finish() returns.
     * @param target Buffer of at least bytes() bytes, not used before finish() returns.
     * @return The release fence of the job, to pass to finish(), or empty when RGA fails, which is
     *         logged.
     */
    [[nodiscard]] std::optional<int> start(const RgaImage& source,
                                           const RgaImage& target) const noexcept;

    /**
     * @brief Waits until a copy started by start() is done and releases its fence.
     * @param fence The value start() returned, used once.
     * @return false when the wait fails, which is logged.
     */
    [[nodiscard]] bool finish(int fence) const noexcept;

private:
    RgaSnapshot(const SnapshotConfig& config, ImageSize rotated, Logger logger)
        : settings{config}, copySize{rotated}, log{logger} {}

    SnapshotConfig settings;
    ImageSize copySize;
    Logger log;
};

#endif // RGA_H
