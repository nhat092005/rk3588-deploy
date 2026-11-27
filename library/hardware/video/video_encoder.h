#ifndef VIDEO_ENCODER_H
#define VIDEO_ENCODER_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

#include "logger.h"
#include "mpp_source.h"

/** @brief Limits and defaults of the video settings. */
constexpr int maxVideoKbps{200'000};
constexpr int maxVideoFps{120};
constexpr int defaultVideoKbps{20'000};
constexpr int defaultVideoFps{30};
/** @brief Bytes of the output buffer. A key frame of 4K at 20 Mbps is well below it. */
constexpr std::size_t defaultVideoCapacity{4'194'304};

/** @brief The NV12 images to encode and the stream settings. */
struct VideoConfig {
    /** Width and height of the image in pixels, both even and above 0. */
    int width{0};
    int height{0};
    /** Bytes per line of the luma plane, at least the width. */
    int horStride{0};
    /** Lines of the luma plane including padding, at least the height. */
    int verStride{0};
    /** Whether the YUV values use the full range 0 to 255 (CameraFormat::fullRange). It is written
     *  to the stream. */
    bool fullRange{false};
    /** Whether the stream says that its colours use the BT.709 matrix. false leaves the matrix
     *  out, as the camera stream always did. */
    bool bt709{false};
    /** Target bitrate in kilobits per second (constant bitrate mode), 1 to maxVideoKbps. */
    int kbps{defaultVideoKbps};
    /** Frame rate of the stream, 1 to maxVideoFps. */
    int framesPerSecond{defaultVideoFps};
    /** A key frame every this many frames, at least 1. With the same number as the frame rate a
     *  player can start at any whole second. */
    int gopFrames{defaultVideoFps};
    /** Bytes of the output buffer, above 0. A frame that encodes to more is an error. */
    std::size_t capacity{defaultVideoCapacity};
};

/** @brief One encoded frame, valid until the next encode() call. */
struct VideoPacket {
    /** Annex B bytes (start codes). A key frame starts with VPS, SPS and PPS. */
    const std::uint8_t* data{nullptr};
    std::size_t size{0};
    bool keyFrame{false};
};

/** @brief The MPP context and the output buffer. Defined in video_encoder.cpp. */
struct VideoState;

/**
 * @brief The hardware H.265 encoder of the RK3588 (MPP, HEVC). Encodes the frames of one stream,
 *        one at a time, and keeps the result in a buffer of its own. Move-only.
 *
 * The image must be upright already: the RGA rotates while it copies the frame, which costs
 * nothing extra (measured on the board), and the overlay draws on the upright image. encode()
 * waits for the result, so call it from a worker thread.
 */
class VideoEncoder {
public:
    /**
     * @brief Checks the configuration, creates the MPP encoder and the output buffer.
     * @param config Image geometry and stream settings.
     * @param log Logger that the object keeps for the rest of its life.
     * @return The encoder, or empty on a bad configuration or an MPP failure, which is logged.
     */
    static std::optional<VideoEncoder> create(const VideoConfig& config, const Logger& log);

    VideoEncoder(const VideoEncoder&) = delete;
    VideoEncoder& operator=(const VideoEncoder&) = delete;
    VideoEncoder(VideoEncoder&& other) noexcept;
    VideoEncoder& operator=(VideoEncoder&& other) noexcept;
    ~VideoEncoder();

    /**
     * @brief Encodes the next frame of the stream.
     * @param source NV12 buffer laid out as in the configuration.
     * @return The packet, or empty on failure (logged). Its bytes stay valid until the next call.
     */
    [[nodiscard]] std::optional<VideoPacket> encode(const MppSource& source) noexcept;

private:
    explicit VideoEncoder(std::unique_ptr<VideoState> initial) noexcept;

    std::unique_ptr<VideoState> state;
};

#endif // VIDEO_ENCODER_H
