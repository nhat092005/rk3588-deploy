#ifndef FRAME_SOURCE_H
#define FRAME_SOURCE_H

#include <array>
#include <cstddef>
#include <cstdint>

#include "color_matrix.h"
#include "dmabuf.h"
#include "handles.h"

// The header shows project types only. Every V4L2 type stays in camera.cpp, which checks that
// maxPlanes equals VIDEO_MAX_PLANES.
constexpr std::size_t maxPlanes{8};

/** @brief The frame format that a source delivers. */
struct CameraFormat {
    /** Width in pixels. */
    int width{0};
    /** Height in pixels. */
    int height{0};
    /** Bytes from the start of one line to the next, in plane 0. */
    int bytesPerLine{0};
    /** Whether the driver says that the YUV values use the full range 0 to 255 (V4L2
     *  quantization). false means the limited range, 16 to 235 for luma. */
    bool fullRange{false};
    /** Lines of the luma plane in the buffer, or 0 for `height`. A decoder pads the height (1088
     *  lines for 1080). */
    int verticalStride{0};
    /** The matrix that turns the YUV into RGB. */
    ColorMatrix matrix{ColorMatrix::Bt601};
};

/** @brief One dequeued frame. `bytesUsed` holds the used bytes of each plane. */
struct Frame {
    std::size_t index{0};
    std::int64_t sequence{0};
    /** Time the frame was captured, in seconds of CLOCK_MONOTONIC (the driver's timestamp), 0 when
     *  the driver gives no monotonic timestamp. */
    double captureSeconds{0.0};
    bool flaggedError{false};
    std::array<std::size_t, maxPlanes> bytesUsed{};
};

/** @brief Result of FrameSource::waitFrame. End: the source has no more frames, which is not an
 *         error. A camera never ends. */
enum class WaitStatus : std::uint8_t { Ready, Timeout, Stopped, Failed, End };

/** @brief Result of FrameSource::dequeue. */
enum class DequeueStatus : std::uint8_t { Frame, NotReady, Failed };

/** @brief A dequeue result. `frame` is valid only when `status` is Frame. */
struct Dequeued {
    DequeueStatus status{DequeueStatus::Failed};
    Frame frame{};
};

/**
 * @brief Where the pipeline takes its frames from: a fixed set of NV12 DMA-BUFs that are filled
 *        one after the other. A frame is dequeued, used and then queued back by its index. The
 *        pipeline is the only user of one source and calls it as the camera does: see Camera.
 */
class FrameSource {
public:
    virtual ~FrameSource() = default;

    /** @brief Width, height and line stride of the frames. */
    [[nodiscard]] virtual const CameraFormat& format() const noexcept = 0;

    /** @brief Number of frame buffers in use: dequeue() returns an index below it. */
    [[nodiscard]] virtual std::size_t bufferCount() const noexcept = 0;

    /**
     * @brief The DMA-BUF of one plane of one buffer, to hand to a device API.
     * @param index Buffer index below bufferCount().
     * @param plane Plane index.
     * @return The buffer, owned by the source and valid until close().
     */
    [[nodiscard]] virtual const DmaBuf& planeBuffer(std::size_t index, std::size_t plane) const = 0;

    /**
     * @brief Waits for a frame, the end of the source or a stop signal.
     * @param stop Signal that ends the wait early.
     * @return Ready when a frame can be dequeued. Timeout when none came in time. Stopped when
     *         `stop` fired, which wins over Ready. End when there are no more frames. Failed on an
     *         error, which is logged.
     */
    [[nodiscard]] virtual WaitStatus waitFrame(const StopSignal& stop) const noexcept = 0;

    /**
     * @brief Takes one filled buffer without blocking.
     * @return Frame with the buffer filled in; the buffer belongs to the caller until queue().
     *         NotReady when there is no frame yet, which is not an error. Failed on an error,
     *         which is logged.
     */
    [[nodiscard]] virtual Dequeued dequeue() const noexcept = 0;

    /**
     * @brief Gives a buffer back to the source.
     * @param index Buffer index of a dequeued Frame.
     * @return false when the source refused it, which is logged.
     */
    [[nodiscard]] virtual bool queue(std::size_t index) const noexcept = 0;

    /**
     * @brief Stops the source and releases every buffer. Safe to call twice.
     * @return true when every step succeeded. A failed step is logged and does not stop the
     *         others.
     */
    [[nodiscard]] virtual bool close() noexcept = 0;

protected:
    FrameSource() = default;
    FrameSource(const FrameSource&) = default;
    FrameSource& operator=(const FrameSource&) = default;
    FrameSource(FrameSource&&) noexcept = default;
    FrameSource& operator=(FrameSource&&) noexcept = default;
};

#endif // FRAME_SOURCE_H
