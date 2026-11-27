#ifndef FILE_SOURCE_H
#define FILE_SOURCE_H

#include <cstddef>
#include <memory>
#include <string>

#include "frame_source.h"
#include "logger.h"

/** @brief Number of decoder buffers of a FileSource. The decoder keeps the frames that later
 *         frames refer to (up to 16 for H.264), the pipeline holds up to 6, and the rest keeps the
 *         decoder going. Measured on the board: a stream with B-frames hangs below 8, and hangs
 *         with 6 frames held below 10. */
constexpr std::size_t fileSourceBuffers{24};
/** @brief Most bytes of decoder buffers a FileSource may take. A source above 4K is not measured.
 */
constexpr std::size_t fileSourceMaxBytes{1'073'741'824};

/**
 * @brief The frames of an H.264 or H.265 video file, as a FrameSource: libavformat reads the
 *        packets, a bitstream filter turns them into Annex-B, and the MPP decoder writes NV12 into
 *        DMA-BUFs of the "system" heap that this object allocates (the buffer group of the decoder
 *        is external). A thread of its own feeds the decoder and releases the frames at the pace of
 *        the video (the presentation times, or the frame rate when the file has none), so that the
 *        pipeline sees what the camera shows: a frame at a time, stamped with the monotonic clock
 *        when it is released. The frames are never dropped here.
 *
 * waitFrame() returns End after the last frame was dequeued. A frame stays valid until queue(): the
 * decoder buffer is held until then. The format is known as soon as open() returns, which reads
 * ahead until the decoder reports it. A change of the picture size in the middle of the video is
 * not supported and fails the source. The video must be 8-bit 4:2:0, not 10-bit and not a
 * compressed (AFBC) output. The rotation flag of a file is not applied. Not copyable or movable.
 */
class FileSource final : public FrameSource {
public:
    /**
     * @brief Opens the file, reads ahead until the decoder knows the picture size, allocates the
     *        buffers and starts the thread that decodes.
     * @param path The video file, `.mp4` or another that libavformat reads.
     * @param log Logger that the object keeps for the rest of its life.
     * @return The source, or empty when the file cannot be read, has no H.264 or H.265 video, is
     *         not 8-bit NV12 or needs more than fileSourceMaxBytes, or the decoder fails, which is
     *         logged.
     */
    static std::unique_ptr<FileSource> open(const std::string& path, const Logger& log);

    FileSource(const FileSource&) = delete;
    FileSource& operator=(const FileSource&) = delete;
    FileSource(FileSource&&) = delete;
    FileSource& operator=(FileSource&&) = delete;

    /** @brief Same as close(). */
    ~FileSource() override;

    /** @brief The size, strides, range and matrix of the decoded frames. The matrix is the one
     *         the file says, or by the height (720 and above BT.709) when it says nothing. */
    [[nodiscard]] const CameraFormat& format() const noexcept override;

    /** @brief fileSourceBuffers. */
    [[nodiscard]] std::size_t bufferCount() const noexcept override;

    /** @brief The DMA-BUF of a decoder buffer. Plane 0 holds both planes (NV12). */
    [[nodiscard]] const DmaBuf& planeBuffer(std::size_t index, std::size_t plane) const override;

    /**
     * @brief Waits for a released frame, the end of the video or a stop signal, at most 2 seconds.
     * @return Ready, Timeout, Stopped (wins over Ready), End when every frame was dequeued, or
     *         Failed when the decoder failed or the picture size changed (logged).
     */
    [[nodiscard]] WaitStatus waitFrame(const StopSignal& stop) const noexcept override;

    /** @brief Takes the oldest released frame, NotReady when there is none. */
    [[nodiscard]] Dequeued dequeue() const noexcept override;

    /** @brief Gives the decoder buffer of a dequeued frame back to the decoder. */
    [[nodiscard]] bool queue(std::size_t index) const noexcept override;

    /** @brief Stops the thread, resets and destroys the decoder and frees the buffers. Safe to
     *         call twice. Call it when the users of the frames are done. */
    [[nodiscard]] bool close() noexcept override;

private:
    struct State;
    explicit FileSource(std::unique_ptr<State> initial) noexcept;

    std::unique_ptr<State> state;
};

#endif // FILE_SOURCE_H
