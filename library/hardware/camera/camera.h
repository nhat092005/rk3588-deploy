#ifndef CAMERA_H
#define CAMERA_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "dmabuf.h"
#include "frame_source.h"
#include "handles.h"
#include "logger.h"

// The header shows project types only. Every V4L2 type stays in camera.cpp.
/** @brief Default number of frame buffers, and the most a CameraConfig may ask for. */
constexpr std::size_t defaultBufferCount{4};
constexpr std::size_t maxBufferCount{16};

/** @brief A rectangle in sensor pixels. */
struct CropRect {
    int left{0};
    int top{0};
    int width{0};
    int height{0};

    /** @brief Whether all four fields are equal. */
    friend bool operator==(const CropRect& first, const CropRect& second) noexcept {
        return first.left == second.left && first.top == second.top &&
               first.width == second.width && first.height == second.height;
    }

    /** @brief Whether any field differs. */
    friend bool operator!=(const CropRect& first, const CropRect& second) noexcept {
        return !(first == second);
    }
};

/** @brief What to open and the size to ask for. */
struct CameraConfig {
    /** Path of the capture node, for example /dev/video11. */
    const char* node{nullptr};
    /** Width of the frame to ask for, in pixels. */
    int width{0};
    /** Height of the frame to ask for, in pixels. */
    int height{0};
    /** dma-heap that the frame buffers come from. */
    std::string_view heap{defaultHeap};
    /** Number of frame buffers to ask for, 2 to maxBufferCount. The driver may grant fewer. A
     *  frame that other threads hold longer needs more buffers. */
    std::size_t buffers{defaultBufferCount};
};

/**
 * @brief One V4L2 capture node, streaming NV12 into DMA-BUFs of a dma-heap (V4L2_MEMORY_DMABUF).
 *        The buffers are DmaBuf objects, so a frame can go to RGA, RKNN or MPP by descriptor
 *        without a copy. All storage is fixed size: nothing is allocated per frame. Move-only.
 *        The ISP crop is a state of the node that outlives the process, so the Camera sets it to
 *        the aspect ratio of the requested size and puts the original one back in close() or the
 *        destructor. It is a FrameSource that never ends. The class is final, so the close() that
 *        the destructor calls is the one of Camera. The move constructor is for the optional that
 *        open() returns: a Camera is moved into its owner once and then stays where it is.
 */
class Camera final : public FrameSource {
public:
    /**
     * @brief Opens the node, crops the ISP, sets the format, allocates and queues the buffers and
     *        starts streaming.
     * @param config Node path, heap and the size to ask for. The sensor frame is cropped, centred,
     * to the aspect ratio of that size, so it is scaled and not stretched. The driver may answer
     * with another size.
     * @param log Logger that the Camera copies and uses for the rest of its life.
     * @return The streaming Camera, or empty on failure. A failure is logged and everything
     *         acquired so far is released.
     */
    static std::optional<Camera> open(const CameraConfig& config, const Logger& log);

    Camera(const Camera&) = delete;
    Camera& operator=(const Camera&) = delete;
    Camera(Camera&&) noexcept = default;
    Camera& operator=(Camera&&) noexcept = default;

    /** @brief Same as close(). A failure is logged. */
    ~Camera() override;

    /** @brief Width, height and line stride that the driver accepted. */
    const CameraFormat& format() const noexcept override {
        return acceptedFormat;
    }

    /** @brief Number of frame buffers in use: dequeue() returns an index below it. */
    std::size_t bufferCount() const noexcept override {
        return buffersInUse;
    }

    /**
     * @brief Number of planes of the format the driver accepted.
     * @return A value from 1 to maxPlanes.
     */
    std::size_t planeCount() const noexcept {
        return planes;
    }

    /**
     * @brief Size the driver reported for one plane.
     * @param plane Plane index, below planeCount().
     * @return The sizeimage of the plane, in bytes.
     */
    std::size_t planeSize(std::size_t plane) const {
        return sizeImage.at(plane);
    }

    /**
     * @brief Waits for a frame or a stop signal, at most 2 seconds.
     * @param stop Signal that ends the wait early.
     * @return Ready when a frame can be dequeued. Timeout when none came in time. Stopped when
     *         `stop` fired, which wins over Ready. Failed when poll() failed or reported an error
     *         on the node, and the failure is logged.
     */
    [[nodiscard]] WaitStatus waitFrame(const StopSignal& stop) const noexcept override;

    /**
     * @brief Takes one filled buffer from the driver without blocking.
     * @return Frame with the buffer filled in; the buffer belongs to the caller until queue().
     *         NotReady when the driver has no frame yet, which is not an error. Failed on a driver
     *         error, which is logged.
     */
    [[nodiscard]] Dequeued dequeue() const noexcept override;

    /**
     * @brief Gives a buffer back to the driver.
     * @param index Buffer index of a dequeued Frame.
     * @return false when the driver refused it, which is logged.
     */
    [[nodiscard]] bool queue(std::size_t index) const noexcept override;

    /**
     * @brief The DMA-BUF of one plane of one buffer, to hand to a device API.
     * @param index Buffer index of a dequeued Frame, below the number of buffers in use.
     * @param plane Plane index, below planeCount().
     * @return The buffer, owned by the Camera and valid until close(). The CPU must wrap its own
     *         access in DmaBuf::beginCpuAccess() and endCpuAccess(), and only while the frame is
     *         dequeued.
     */
    const DmaBuf& planeBuffer(std::size_t index, std::size_t plane) const override {
        return memories.at(index * planes + plane);
    }

    /**
     * @brief Writes the used bytes of every plane of a frame, one plane after the other.
     * @param frame A dequeued frame that has not been queued back yet.
     * @param path File to create or truncate. A used size above the buffer length is clamped,
     *        with a warning.
     * @return false when the file cannot be opened or written, which is logged.
     */
    [[nodiscard]] bool writeFrame(const Frame& frame, const char* path) const;

    /**
     * @brief Stops streaming, releases every buffer, restores the ISP crop and closes the node.
     *        Safe to call twice.
     * @return true when every step succeeded. A failed step is logged and does not stop the
     *         others. A buffer that fails to unmap or close is logged by its DmaBuf and is not
     *         counted here.
     */
    [[nodiscard]] bool close() noexcept override;

private:
    Camera(FileDescriptor videoDevice, Logger logger) noexcept
        : device{std::move(videoDevice)}, log{logger} {}

    [[nodiscard]] bool queryCapabilities() const noexcept;
    [[nodiscard]] bool enumerateFormats() const noexcept;
    [[nodiscard]] bool setCrop(const CameraConfig& config) noexcept;
    [[nodiscard]] bool setFormat(const CameraConfig& config) noexcept;
    [[nodiscard]] bool queryBufferLengths() noexcept;
    [[nodiscard]] bool allocateBuffers(const CameraConfig& config) noexcept;
    [[nodiscard]] bool startStreaming() noexcept;

    FileDescriptor device;
    Logger log;
    CameraFormat acceptedFormat{};
    std::size_t planes{0};
    std::size_t buffersInUse{0};
    bool streaming{false};
    bool cropChanged{false};
    CropRect originalCrop{};
    std::array<std::size_t, maxPlanes> sizeImage{};
    // Bytes the driver needs per plane. It can exceed sizeImage: the height is aligned to 16.
    std::array<std::size_t, maxPlanes> bufferLength{};
    // Plane `plane` of buffer `index` is at `index * planes + plane`. Filled once by open().
    std::vector<DmaBuf> memories;
};

#endif // CAMERA_H
