#ifndef BENCH_RGA_FORCED_H
#define BENCH_RGA_FORCED_H

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "dmabuf.h"
#include "im2d.hpp"
#include "logger.h"
#include "rga.h"

/** @brief The RGA core that a job is limited to. */
enum class RgaCore : std::uint8_t { Rga2, Rga30, Rga31 };

/**
 * @brief The letterbox job of RgaLetterbox::run on one chosen RGA core. The job, the geometry and
 *        the padding are those of RgaLetterbox: only the core differs.
 *
 * RgaLetterbox leaves the core to the kernel, which is what the application does. The bench needs
 * each core alone, so this class passes the core to the same improcess calls. A full range source
 * takes two calls, as in RgaLetterbox: NV12 to RGB into a staging buffer, then scale, rotate and
 * place. The staging buffer is in the lowHeap for RGA2, which maps memory below 4 GB only, and in
 * the defaultHeap for the RGA3 cores, as in RgaLetterbox. Both calls use the core.
 */
class RgaForcedLetterbox {
public:
    /**
     * @brief Checks the configuration and computes the layout.
     * @param config As for RgaLetterbox::create(). The rotation must be 180 degrees.
     * @param core The only core the job may use.
     * @param log Logger that the object keeps.
     * @return The converter, or empty when the configuration is rejected, which is logged.
     */
    static std::optional<RgaForcedLetterbox> create(const LetterboxConfig& config, RgaCore core,
                                                    const Logger& log);

    /**
     * @brief Fills the target with the padding value, once per target buffer.
     * @param target RGB output buffer of targetSize x targetSize x 3 bytes.
     * @return false when RGA fails, which is logged.
     */
    [[nodiscard]] bool fillPadding(const RgaImage& target) const noexcept {
        return base.fillPadding(target);
    }

    /**
     * @brief Converts one frame on the chosen core.
     * @param source NV12 frame buffer, of the size in the configuration.
     * @param target Output buffer, already padded by fillPadding().
     * @return false when the core rejects the job or fails, which is logged with the RGA message.
     */
    [[nodiscard]] bool run(const RgaImage& source, const RgaImage& target) const noexcept;

    /**
     * @brief The status of the last call that failed.
     * @return The RGA status, IM_STATUS_SUCCESS when no call has failed.
     */
    IM_STATUS lastStatus() const noexcept {
        return failedStatus;
    }

    /**
     * @brief The message of the last call that failed. The RGA library gives a message once, so it
     *        is kept here.
     * @return The message, empty when no call has failed.
     */
    const std::string& lastMessage() const noexcept {
        return failedMessage;
    }

private:
    RgaForcedLetterbox(RgaLetterbox&& converter, const LetterboxConfig& config, RgaCore chosen,
                       Logger logger, std::optional<DmaBuf> stagingBuffer,
                       std::optional<RgaImage> stagingHandle) noexcept
        : base{std::move(converter)}, settings{config}, core{chosen}, log{logger},
          staging{std::move(stagingBuffer)}, stagingImage{std::move(stagingHandle)} {}

    // NV12 full range to RGB888 of the source size, into the staging buffer, on the core.
    [[nodiscard]] bool convertFullRange(const RgaImage& source, const RgaImage& staged,
                                        int mask) const noexcept;

    // Layout and padding only: it is made for a limited range source, so it owns no staging buffer.
    RgaLetterbox base;
    LetterboxConfig settings;
    RgaCore core;
    Logger log;
    // The buffer is declared before its RGA handle, which is released first.
    std::optional<DmaBuf> staging;
    std::optional<RgaImage> stagingImage;
    mutable IM_STATUS failedStatus{IM_STATUS_SUCCESS};
    mutable std::string failedMessage;
};

/**
 * @brief The name of a core.
 * @param core A core.
 * @return "RGA2", "RGA3_0" or "RGA3_1".
 */
std::string_view rgaCoreName(RgaCore core) noexcept;

#endif // BENCH_RGA_FORCED_H
