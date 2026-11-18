#ifndef EVIDENCE_WORKER_H
#define EVIDENCE_WORKER_H

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "app_options.h"
#include "evidence_hub.h"
#include "jpeg_encoder.h"
#include "logger.h"
#include "rga.h"
#include "violation_store.h"

/** @brief What the evidence thread has done, readable from any thread. */
struct EvidenceCounts {
    std::atomic<std::int64_t> violationsSaved{0};
    std::atomic<std::int64_t> violationsFailed{0};
    std::atomic<std::int64_t> clipsSaved{0};
    std::atomic<std::int64_t> clipsFailed{0};
    /** Violations not stored because the volume had no room even after removing the old ones. */
    std::atomic<std::int64_t> violationsRefused{0};
    /** Old violations removed to keep the limits. */
    std::atomic<std::int64_t> violationsRemoved{0};
};

/**
 * @brief The work of the evidence thread: for each request of the hub's queue, encode the two
 *        images of the violations as JPEGs and store them, or add a finished clip to the directory
 *        of its violation. It also keeps the folder within its limits (ViolationRetention): at
 *        the start, after every stored violation or clip, and once an hour. It runs in the thread
 *        that calls run(); the disk and the JPEG encoder are slow, so the frame threads never wait
 *        for it. Move-only through its members.
 */
class EvidenceWorker {
public:
    /**
     * @brief Creates the two JPEG encoders and the store.
     * @param options The command line: output folder, JPEG quality, storage limit and days.
     * @param size Size of the images in pixels.
     * @param stride Bytes per line of the images.
     * @param fullRange Whether the images use the full YUV range.
     * @param log Logger that the object keeps for the rest of its life.
     * @return The worker, or empty on failure (the folder is missing or not writable, an encoder
     *         fails, a retention limit is out of range), which is logged. The folder is read and
     *         the old violations that break a limit are removed before it returns.
     */
    static std::optional<EvidenceWorker> create(const AppOptions& options, ImageSize size,
                                                int stride, bool fullRange, const Logger& log);

    EvidenceWorker(const EvidenceWorker&) = delete;
    EvidenceWorker& operator=(const EvidenceWorker&) = delete;
    EvidenceWorker(EvidenceWorker&& other) noexcept;
    EvidenceWorker& operator=(EvidenceWorker&& other) noexcept;
    ~EvidenceWorker();

    /**
     * @brief Serves the hub's queue until it is closed and empty.
     * @param hub The hub that the writer thread fills.
     */
    void run(EvidenceHub& hub) noexcept;

    /** @brief The counts, valid while the worker lives. */
    const EvidenceCounts& counts() const noexcept;

    /** @brief Bytes of the violations in the folder now (the retention's index). */
    std::uint64_t storedBytes() const noexcept;

private:
    struct State;
    explicit EvidenceWorker(std::unique_ptr<State> initial) noexcept;

    std::unique_ptr<State> state;
};

#endif // EVIDENCE_WORKER_H
