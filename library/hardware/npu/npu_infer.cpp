#include "npu_infer.h"

#include <fstream>
#include <utility>

#include "rknn_api.h"

struct NpuState {
    explicit NpuState(Logger logger) : log{logger} {}
    NpuState(const NpuState&) = delete;
    NpuState& operator=(const NpuState&) = delete;
    NpuState(NpuState&&) = delete;
    NpuState& operator=(NpuState&&) = delete;

    // Memory objects first, then the context: a memory object needs a live context.
    ~NpuState() {
        for (rknn_tensor_mem* memory : inputs) {
            const int status{rknn_destroy_mem(context, memory)};
            if (status != 0) {
                log.error("rknn_destroy_mem ret=", status);
            }
        }
        if (loaded) {
            const int status{rknn_destroy(context)};
            if (status != 0) {
                log.error("rknn_destroy ret=", status);
            }
        }
    }

    Logger log;
    rknn_context context{0};
    bool loaded{false};
    rknn_tensor_attr inputAttr{};
    // Side of the square input in pixels, read from the model in create().
    int inputSide{0};
    // The shape of every output, as the model declares it.
    std::vector<std::vector<std::uint32_t>> outputDims;
    // Owned: released by the destructor.
    std::vector<rknn_tensor_mem*> inputs;
    std::vector<std::vector<float>> outputs;
    // Set once in create(), reused by every run().
    std::vector<rknn_output> outputRequests;
};

namespace {
// The RKNN core mask of a core choice.
rknn_core_mask coreMaskOf(NpuCores cores) noexcept {
    switch (cores) {
    case NpuCores::Core0:
        return RKNN_NPU_CORE_0;
    case NpuCores::Core1:
        return RKNN_NPU_CORE_1;
    case NpuCores::Core2:
        return RKNN_NPU_CORE_2;
    case NpuCores::Core01:
        return RKNN_NPU_CORE_0_1;
    case NpuCores::Core012:
        return RKNN_NPU_CORE_0_1_2;
    case NpuCores::Auto:
        break;
    }
    return RKNN_NPU_CORE_AUTO;
}

// The whole file, or nothing when it cannot be read or is empty.
std::optional<std::vector<char>> readFile(const char* path) {
    std::ifstream file{path, std::ios::binary | std::ios::ate};
    if (!file) {
        return std::nullopt;
    }
    const std::streamoff size{file.tellg()};
    if (size <= 0) {
        return std::nullopt;
    }
    std::vector<char> content(static_cast<std::size_t>(size));
    file.seekg(0);
    file.read(content.data(), size);
    if (!file) {
        return std::nullopt;
    }
    return content;
}

// The side of a square 3 channel image input, or 0 for any other input shape.
int squareInputSide(const rknn_tensor_attr& attr) noexcept {
    constexpr std::uint32_t imageDims{4};
    constexpr std::uint32_t rgbChannels{3};
    if (attr.n_dims != imageDims) {
        return 0;
    }
    const bool nhwc{attr.fmt == RKNN_TENSOR_NHWC};
    const std::uint32_t channels{nhwc ? attr.dims[3] : attr.dims[1]};
    const std::uint32_t height{nhwc ? attr.dims[1] : attr.dims[2]};
    const std::uint32_t width{nhwc ? attr.dims[2] : attr.dims[3]};
    if (channels != rgbChannels || height != width || height == 0) {
        return 0;
    }
    return static_cast<int>(height);
}

// Queries the input attribute and every output attribute, then sizes the output buffers and
// fills the output requests.
bool describeModel(NpuState& state) {
    rknn_input_output_num counts{};
    int status{rknn_query(state.context, RKNN_QUERY_IN_OUT_NUM, &counts, sizeof(counts))};
    if (status != 0) {
        state.log.error("rknn_query(IN_OUT_NUM) ret=", status);
        return false;
    }
    if (counts.n_input != 1 || counts.n_output == 0) {
        state.log.error("model needs one input and at least one output, has n_input=",
                        counts.n_input, " n_output=", counts.n_output);
        return false;
    }
    state.inputAttr.index = 0;
    status =
        rknn_query(state.context, RKNN_QUERY_INPUT_ATTR, &state.inputAttr, sizeof(state.inputAttr));
    if (status != 0) {
        state.log.error("rknn_query(INPUT_ATTR) ret=", status);
        return false;
    }
    state.inputSide = squareInputSide(state.inputAttr);
    if (state.inputSide == 0) {
        state.log.error("the model input is not a square 3 channel image");
        return false;
    }
    state.log.info("NPU input: side=", state.inputSide, " size=", state.inputAttr.size,
                   " size_with_stride=", state.inputAttr.size_with_stride);
    // The data is handed over as uint8 NHWC, the layout of the RGB output of RgaLetterbox.
    state.inputAttr.type = RKNN_TENSOR_UINT8;
    state.inputAttr.fmt = RKNN_TENSOR_NHWC;

    state.outputs.resize(counts.n_output);
    state.outputDims.resize(counts.n_output);
    state.outputRequests.resize(counts.n_output);
    for (std::uint32_t index{0}; index < counts.n_output; ++index) {
        rknn_tensor_attr attr{};
        attr.index = index;
        status = rknn_query(state.context, RKNN_QUERY_OUTPUT_ATTR, &attr, sizeof(attr));
        if (status != 0) {
            state.log.error("rknn_query(OUTPUT_ATTR ", index, ") ret=", status);
            return false;
        }
        state.outputDims.at(index).assign(attr.dims, attr.dims + attr.n_dims);
        std::vector<float>& values{state.outputs.at(index)};
        values.assign(attr.n_elems, 0.0F);
        rknn_output& request{state.outputRequests.at(index)};
        request = rknn_output{};
        request.want_float = 1;
        request.is_prealloc = 1;
        request.index = index;
        request.buf = values.data();
        request.size = static_cast<std::uint32_t>(values.size() * sizeof(float));
        state.log.info("NPU output ", index, ": ", attr.n_elems, " values");
    }
    return true;
}
} // namespace

std::optional<NpuInfer> NpuInfer::create(const NpuConfig& config, const Logger& log) {
    std::optional<std::vector<char>> blob{readFile(config.modelPath)};
    if (!blob) {
        log.error("cannot read model file: ", config.modelPath);
        return std::nullopt;
    }
    auto state{std::make_unique<NpuState>(log)};
    const int status{rknn_init(&state->context, blob->data(),
                               static_cast<std::uint32_t>(blob->size()), 0, nullptr)};
    if (status != 0) {
        log.error("rknn_init ret=", status);
        return std::nullopt;
    }
    state->loaded = true;
    const int masked{rknn_set_core_mask(state->context, coreMaskOf(config.cores))};
    if (masked != 0) {
        log.error("rknn_set_core_mask ret=", masked);
        return std::nullopt;
    }
    if (!describeModel(*state)) {
        return std::nullopt;
    }
    return std::optional<NpuInfer>{NpuInfer{std::move(state)}};
}

NpuInfer::NpuInfer(std::unique_ptr<NpuState> initial) noexcept : state{std::move(initial)} {}

NpuInfer::NpuInfer(NpuInfer&& other) noexcept = default;
NpuInfer& NpuInfer::operator=(NpuInfer&& other) noexcept = default;
NpuInfer::~NpuInfer() = default;

std::size_t NpuInfer::inputBytes() const noexcept {
    return state->inputAttr.size_with_stride;
}

int NpuInfer::inputSide() const noexcept {
    return state->inputSide;
}

std::size_t NpuInfer::outputCount() const noexcept {
    return state->outputs.size();
}

const std::vector<float>& NpuInfer::output(std::size_t index) const {
    return state->outputs.at(index);
}

const std::vector<std::uint32_t>& NpuInfer::outputDims(std::size_t index) const {
    return state->outputDims.at(index);
}

std::optional<InputId> NpuInfer::attachInput(DmaBuf& buffer) {
    const std::size_t needed{inputBytes()};
    if (buffer.size() < needed) {
        state->log.error("input buffer has ", buffer.size(), " bytes, the model needs ", needed);
        return std::nullopt;
    }
    rknn_tensor_mem* memory{rknn_create_mem_from_fd(state->context, buffer.fd(), buffer.data(),
                                                    static_cast<std::uint32_t>(needed), 0)};
    if (memory == nullptr) {
        state->log.error("rknn_create_mem_from_fd returned null for fd=", buffer.fd());
        return std::nullopt;
    }
    state->inputs.push_back(memory);
    return static_cast<InputId>(state->inputs.size() - 1);
}

bool NpuInfer::run(InputId input) noexcept {
    const std::size_t slot{static_cast<std::size_t>(input)};
    if (slot >= state->inputs.size()) {
        state->log.error("unknown input id ", slot);
        return false;
    }
    int status{rknn_set_io_mem(state->context, state->inputs.at(slot), &state->inputAttr)};
    if (status != 0) {
        state->log.error("rknn_set_io_mem ret=", status);
        return false;
    }
    status = rknn_run(state->context, nullptr);
    if (status != 0) {
        state->log.error("rknn_run ret=", status);
        return false;
    }
    const auto outputCountValue{static_cast<std::uint32_t>(state->outputRequests.size())};
    status =
        rknn_outputs_get(state->context, outputCountValue, state->outputRequests.data(), nullptr);
    if (status != 0) {
        state->log.error("rknn_outputs_get ret=", status);
        return false;
    }
    status = rknn_outputs_release(state->context, outputCountValue, state->outputRequests.data());
    if (status != 0) {
        state->log.error("rknn_outputs_release ret=", status);
        return false;
    }
    return true;
}
