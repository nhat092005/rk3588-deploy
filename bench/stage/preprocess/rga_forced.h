#ifndef BENCH_RGA_FORCED_H
#define BENCH_RGA_FORCED_H

#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

#include "logger.h"
#include "rga.h"

/** @brief The RGA core that a job is limited to. */
enum class RgaCore : std::uint8_t { Rga2, Rga30, Rga31 };

/**
 * @brief The letterbox job of RgaLetterbox::run on one chosen RGA core. The job, the geometry and
 *        the padding are those of RgaLetterbox: only the core differs.
 *
 * RgaLetterbox leaves the core to the kernel, which is what the application does. The bench needs
 * each core alone, so this class passes the core to the same improcess call.
 */
class RgaForcedLetterbox {
public:
    /**
     * @brief Checks the configuration and computes the layout.
     * @param config As for RgaLetterbox::create(). `fullRange` must be false and the rotation
     *        180 degrees.
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

private:
    RgaForcedLetterbox(RgaLetterbox&& converter, const LetterboxConfig& config, RgaCore chosen,
                       Logger logger) noexcept
        : base{std::move(converter)}, settings{config}, core{chosen}, log{logger} {}

    RgaLetterbox base;
    LetterboxConfig settings;
    RgaCore core;
    Logger log;
};

/**
 * @brief The name of a core.
 * @param core A core.
 * @return "RGA2", "RGA3_0" or "RGA3_1".
 */
std::string_view rgaCoreName(RgaCore core) noexcept;

#endif // BENCH_RGA_FORCED_H
