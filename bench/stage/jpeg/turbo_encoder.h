#ifndef TURBO_ENCODER_H
#define TURBO_ENCODER_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

#include "logger.h"

/** @brief The image the CPU encoder takes and the quality. */
struct TurboConfig {
    /** Width and height of the NV12 image in pixels, both multiples of 16. */
    int width{0};
    int height{0};
    /** Quality factor, 1 to 100 (the libjpeg scale). */
    int quality{0};
};

/** @brief What one encode gave. */
struct TurboResult {
    /** Bytes of the JPEG, in data(). */
    std::size_t bytes{0};
    /** Milliseconds spent on splitting the interleaved chroma plane (inside the encode). */
    double splitMs{0.0};
};

/** @brief State of the encoder. Defined in turbo_encoder.cpp. */
struct TurboState;

/**
 * @brief The CPU JPEG encoder of the stage 4 bench: libjpeg-turbo, one thread, the raw data input
 *        (the planes go in as they are, no colour conversion), 4:2:0, the standard tables scaled by
 *        the quality, the default DCT. The chroma plane of NV12 is split into Cb and Cr first
 *        (OpenCV), because libjpeg takes planes. Move-only.
 *
 * An error inside libjpeg is logged and ends the process (the bench has no way on).
 */
class TurboEncoder {
public:
    /**
     * @brief Checks the configuration and prepares the encoder and its buffers.
     * @return The encoder, or empty on a bad configuration (logged).
     */
    static std::optional<TurboEncoder> create(const TurboConfig& config, const Logger& log);

    TurboEncoder(const TurboEncoder&) = delete;
    TurboEncoder& operator=(const TurboEncoder&) = delete;
    TurboEncoder(TurboEncoder&& other) noexcept;
    TurboEncoder& operator=(TurboEncoder&& other) noexcept;
    ~TurboEncoder();

    /**
     * @brief Encodes one image.
     * @param nv12 The Y plane of width x height bytes, then the interleaved CbCr plane of
     *        width x height / 2 bytes, no padding, full range.
     */
    [[nodiscard]] TurboResult encode(const std::uint8_t* nv12) noexcept;

    /** @brief The JPEG of the last encode(). */
    const std::uint8_t* data() const noexcept;

private:
    explicit TurboEncoder(std::unique_ptr<TurboState> initial) noexcept;

    std::unique_ptr<TurboState> state;
};

#endif // TURBO_ENCODER_H
