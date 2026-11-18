#ifndef JPEG_ENCODER_H
#define JPEG_ENCODER_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

#include "dmabuf.h"
#include "logger.h"
#include "mpp_source.h"

/** @brief Lowest and highest quality factor of the encoder. */
constexpr int minJpegQuality{1};
constexpr int maxJpegQuality{99};
/** @brief Default quality factor. */
constexpr int defaultJpegQuality{90};
/** @brief Bytes of the output buffer. A 4K frame at quality 90 is well below it. */
constexpr std::size_t defaultJpegCapacity{16'777'216};

/** @brief The NV12 image to encode and the quality. */
struct JpegConfig {
    /** Width and height of the image in pixels, both even and above 0. */
    int width{0};
    int height{0};
    /** Bytes per line of the luma plane, at least the width. */
    int horStride{0};
    /** Lines of the luma plane including padding, at least the height. The chroma plane starts at
     *  horStride x verStride. */
    int verStride{0};
    /** Whether the YUV values use the full range 0 to 255 (CameraFormat::fullRange). JPEG stores
     *  full range samples and the encoder does not convert, so create() refuses false. */
    bool fullRange{false};
    /** Quality factor, 1 to 99. */
    int quality{defaultJpegQuality};
    /** Bytes of the output buffer, above 0. An image that encodes to more is an error. */
    std::size_t capacity{defaultJpegCapacity};
};

/** @brief The MPP context and the output buffer. Defined in jpeg_encoder.cpp. */
struct JpegState;

/**
 * @brief The hardware JPEG encoder of the RK3588 (MPP, MJPEG). Encodes one NV12 image at a time
 *        and keeps the result in a buffer of its own. Move-only.
 *
 * It is made for a violation snapshot, once per violation, not for every frame: encode() makes a
 * small MPP frame object and waits for the result.
 */
class JpegEncoder {
public:
    /**
     * @brief Checks the configuration, creates the MPP encoder and the output buffer.
     * @param config Image geometry, quality and output capacity.
     * @param log Logger that the object keeps for the rest of its life.
     * @return The encoder, or empty on a bad configuration or an MPP failure, which is logged.
     */
    static std::optional<JpegEncoder> create(const JpegConfig& config, const Logger& log);

    JpegEncoder(const JpegEncoder&) = delete;
    JpegEncoder& operator=(const JpegEncoder&) = delete;
    JpegEncoder(JpegEncoder&& other) noexcept;
    JpegEncoder& operator=(JpegEncoder&& other) noexcept;
    ~JpegEncoder();

    /**
     * @brief Encodes one image.
     * @param source NV12 buffer laid out as in the configuration.
     * @return The number of bytes of the JPEG, or empty on failure (logged). The bytes are in
     *         data() until the next call.
     */
    [[nodiscard]] std::optional<std::size_t> encode(const MppSource& source) noexcept;

    /** @brief The JPEG of the last successful encode(), `size` bytes as returned by it. */
    const std::uint8_t* data() const noexcept;

private:
    explicit JpegEncoder(std::unique_ptr<JpegState> initial) noexcept;

    std::unique_ptr<JpegState> state;
};

#endif // JPEG_ENCODER_H
