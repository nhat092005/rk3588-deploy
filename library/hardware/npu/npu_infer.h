#ifndef NPU_INFER_H
#define NPU_INFER_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "dmabuf.h"
#include "logger.h"

/** @brief The NPU cores a run may use. */
enum class NpuCores : std::uint8_t { Auto, Core0, Core1, Core2, Core01, Core012 };

/** @brief What to load. */
struct NpuConfig {
    /** Path of the .rknn model file. */
    const char* modelPath{nullptr};
    /** NPU cores for the model: Auto lets the driver choose one, Core012 splits one frame over
     *  all three (rknn_set_core_mask). */
    NpuCores cores{NpuCores::Auto};
};

/** @brief Identifies an input buffer attached to an NpuInfer. Only attachInput() creates it. */
enum class InputId : std::uint32_t {};

/** @brief The RKNN context and its buffers. Defined in npu_infer.cpp, so no RKNN type is visible
 * here. */
struct NpuState;

/**
 * @brief One RKNN model on the NPU. The input is read straight from a DmaBuf (zero copy, no
 *        rknn_inputs_set), the outputs go to buffers that are allocated once. Move-only. Nothing
 *        is allocated in run().
 *
 * The model has one input, uint8 NHWC, and any number of outputs, returned as float. The
 * destructor releases the attached inputs first and then the model.
 */
class NpuInfer {
public:
    /**
     * @brief Reads the model file, initialises the context and sizes the buffers.
     * @param config Model path.
     * @param log Logger that the object keeps for the rest of its life.
     * @return The loaded model, or empty on failure (unreadable file, rknn_init error, a model
     *         that does not have exactly one input), which is logged once with the cause.
     */
    static std::optional<NpuInfer> create(const NpuConfig& config, const Logger& log);

    NpuInfer(const NpuInfer&) = delete;
    NpuInfer& operator=(const NpuInfer&) = delete;
    NpuInfer(NpuInfer&& other) noexcept;
    NpuInfer& operator=(NpuInfer&& other) noexcept;
    ~NpuInfer();

    /** @brief Bytes the input tensor needs. A DmaBuf for attachInput() must be at least this. */
    std::size_t inputBytes() const noexcept;

    /** @brief Number of output tensors of the model, at least 1. */
    std::size_t outputCount() const noexcept;

    /**
     * @brief The float values of one output tensor after the last successful run().
     * @param index Output index, below outputCount().
     * @return The values, valid until the next run(). Empty content before the first run().
     */
    const std::vector<float>& output(std::size_t index) const;

    /**
     * @brief Makes a buffer usable as the model input (rknn_create_mem_from_fd).
     * @param buffer Buffer of at least inputBytes() bytes, filled with the uint8 NHWC image. It
     *        must outlive this object. The CPU must have called endCpuAccess() after writing it.
     * @return The id to pass to run(), or empty on failure, which is logged.
     *
     * Attach each buffer once, at start: run() does no setup.
     */
    [[nodiscard]] std::optional<InputId> attachInput(DmaBuf& buffer);

    /**
     * @brief Runs the model on an attached buffer and waits for the result.
     * @param input An id from attachInput() of this object.
     * @return false when the id is unknown or the NPU call fails, which is logged. The outputs
     *         are then not updated.
     */
    [[nodiscard]] bool run(InputId input) noexcept;

private:
    explicit NpuInfer(std::unique_ptr<NpuState> initial) noexcept;

    std::unique_ptr<NpuState> state;
};

#endif // NPU_INFER_H
