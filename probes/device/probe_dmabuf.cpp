// DMA-BUF check: which dma-heap the board's RGA, RKNN and MPP accept.
// Usage: probe_dmabuf model.rknn
//   model.rknn: a model with a 1x640x640x3 uint8 input (the project's YOLO export).
// For each heap (system, system-uncached) the probe runs four checks:
//   map  : allocate 4K NV12 bytes, mmap, write and read back, DMA_BUF_IOCTL_SYNC start and end.
//   rga  : resize a 1920x1080 NV12 frame to 640x360 between two heap buffers, compare with the
//          expected constant result.
//   mpp  : import the buffer as MPP_BUFFER_TYPE_DRM, then encode one 1280x720 H.265 frame of
//          noise and one of a flat image from it. The noise packet must be much larger than the
//          flat one, which shows that the encoder reads the heap buffer.
//          Measured on the board (librockchip-mpp 1.5.0-2, 2026-11-15): MPP_BUFFER_TYPE_EXT_DMA
//          import fails with MPP_ERR_MALLOC (-4). MPP_BUFFER_TYPE_DMA_HEAP imports, but the
//          encoder ignores the content (same 300-byte packet for noise and flat image, also for
//          a buffer that MPP allocated itself). MPP_BUFFER_TYPE_DRM works and keeps the fd.
//   rknn : run the model once with rknn_inputs_set, then once with the input in a heap buffer
//          (rknn_create_mem_from_fd + rknn_set_io_mem), outputs must be byte equal.
// The CPU side follows the DMA-BUF rules: sync end after the CPU writes, sync start before it
// reads what a device wrote.
// Exit code 0 only if every check of every heap passes, 1 otherwise, 2 on bad arguments or files.
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "arguments.h"
#include "dmabuf.h"
#include "im2d.hpp"
#include "logger.h"
#include "rk_mpi.h"
#include "rknn_api.h"

namespace {
constexpr std::array<const char*, 2> heapNames{"system", "system-uncached"};
constexpr std::size_t nv12Bytes4k{12'441'600}; // 3840 x 2160 x 3 / 2
constexpr std::uint8_t mapPattern{0xA5};

// RGA: 1920x1080 NV12 down to 640x360 NV12. A constant image stays constant after resizing.
constexpr int srcWidth{1920};
constexpr int srcHeight{1080};
constexpr int dstWidth{640};
constexpr int dstHeight{360};
constexpr std::uint8_t lumaValue{100};
constexpr std::uint8_t chromaValue{128};
constexpr std::uint8_t clearValue{0};

// MPP: one H.265 frame, 1280x720 NV12. A flat image encodes to a few hundred bytes, noise to many
// times more, so the packet size shows what the encoder read from the heap buffer.
constexpr int encWidth{1280};
constexpr int encHeight{720};
constexpr std::int32_t encFps{30};
constexpr std::int32_t encBitrate{8'000'000};
constexpr std::int64_t encTimeoutMs{2000};
constexpr std::size_t noiseOverFlat{4};

// RKNN input: uint8 NHWC 1x640x640x3.
constexpr std::size_t modelInputBytes{640UL * 640UL * 3UL};
constexpr unsigned int fixedSeed{42};
constexpr unsigned int byteMask{0xFF};

constexpr std::size_t modelArg{1};
constexpr std::size_t argsCount{2};
constexpr int exitUsage{2};
constexpr const char* mppTag{"probe_dmabuf"};

constexpr std::size_t nv12Size(int width, int height) noexcept {
    return static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3 / 2;
}
constexpr std::size_t lumaSize(int width, int height) noexcept {
    return static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
}

// Sets `count` bytes from `offset` to `value`. False when the range is outside the buffer.
[[nodiscard]] bool fillBytes(DmaBuf& buffer, std::size_t offset, std::size_t count,
                             std::uint8_t value) {
    if (offset + count > buffer.size()) {
        return false;
    }
    std::uint8_t* first{static_cast<std::uint8_t*>(buffer.data()) + offset};
    std::fill(first, first + count, value);
    return true;
}

// Copies `bytes` to the start of the buffer. False when they do not fit.
[[nodiscard]] bool storeBytes(DmaBuf& buffer, const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() > buffer.size()) {
        return false;
    }
    std::copy(bytes.begin(), bytes.end(), static_cast<std::uint8_t*>(buffer.data()));
    return true;
}

// Number of bytes in [offset, offset + count) that differ from `value`. A range outside the
// buffer counts as all different.
std::size_t countNot(const DmaBuf& buffer, std::size_t offset, std::size_t count,
                     std::uint8_t value) {
    if (offset + count > buffer.size()) {
        return count;
    }
    const auto* first{static_cast<const std::uint8_t*>(buffer.data()) + offset};
    return static_cast<std::size_t>(
        std::count_if(first, first + count, [value](std::uint8_t byte) { return byte != value; }));
}

// A DmaBuf imported into the RGA driver.
class RgaHandle {
public:
    // importbuffer_fd. Empty when RGA rejects the descriptor.
    static std::optional<RgaHandle> import(const DmaBuf& source, const Logger& log) {
        const rga_buffer_handle_t handle{
            importbuffer_fd(source.fd(), static_cast<int>(source.size()))};
        if (handle == 0) {
            log.error("importbuffer_fd rejected fd=", source.fd());
            return std::nullopt;
        }
        return std::optional<RgaHandle>{RgaHandle{handle, log}};
    }

    RgaHandle(const RgaHandle&) = delete;
    RgaHandle& operator=(const RgaHandle&) = delete;
    RgaHandle(RgaHandle&& other) noexcept
        : handle{std::exchange(other.handle, 0)}, log{other.log} {}
    RgaHandle& operator=(RgaHandle&& other) noexcept {
        if (this != &other) {
            release();
            handle = std::exchange(other.handle, 0);
            log = other.log;
        }
        return *this;
    }
    ~RgaHandle() {
        release();
    }

    // An NV12 image of this buffer for the RGA calls.
    rga_buffer_t nv12Image(int width, int height) const {
        return wrapbuffer_handle(handle, width, height, RK_FORMAT_YCbCr_420_SP);
    }

private:
    RgaHandle(rga_buffer_handle_t imported, Logger logger) noexcept
        : handle{imported}, log{logger} {}

    void release() noexcept {
        if (handle == 0) {
            return;
        }
        const IM_STATUS status{releasebuffer_handle(std::exchange(handle, 0))};
        if (status != IM_STATUS_SUCCESS) {
            log.error("releasebuffer_handle: ", imStrError_t(status));
        }
    }

    rga_buffer_handle_t handle{0};
    Logger log;
};

// imcheck then imresize between two NV12 images. Logs the RGA message on failure.
bool resizeNv12(const RgaHandle& source, const RgaHandle& target, const Logger& log) {
    const rga_buffer_t srcImage{source.nv12Image(srcWidth, srcHeight)};
    const rga_buffer_t dstImage{target.nv12Image(dstWidth, dstHeight)};
    const rga_buffer_t noPattern{};
    const im_rect noRect{};
    const IM_STATUS checked{imcheck_t(srcImage, dstImage, noPattern, noRect, noRect, noRect, 0)};
    if (checked != IM_STATUS_NOERROR) {
        log.error("imcheck: ", imStrError_t(checked));
        return false;
    }
    const IM_STATUS resized{imresize_t(srcImage, dstImage, 0, 0, INTER_LINEAR, 1)};
    if (resized != IM_STATUS_SUCCESS) {
        log.error("imresize: ", imStrError_t(resized));
        return false;
    }
    return true;
}

// A DmaBuf imported into MPP as a DRM buffer.
class MppImport {
public:
    // mpp_buffer_import with MPP_BUFFER_TYPE_DRM. Empty when MPP rejects the buffer.
    static std::optional<MppImport> import(DmaBuf& source, const Logger& log) {
        MppBufferInfo info{};
        info.type = MPP_BUFFER_TYPE_DRM;
        info.size = source.size();
        info.ptr = source.data();
        info.fd = source.fd();
        MppBuffer imported{nullptr};
        const MPP_RET status{
            mpp_buffer_import_with_tag(nullptr, &info, &imported, mppTag, "import")};
        if (status != MPP_OK || imported == nullptr) {
            log.error("mpp_buffer_import ret=", static_cast<int>(status));
            return std::nullopt;
        }
        return std::optional<MppImport>{MppImport{imported, log}};
    }

    MppImport(const MppImport&) = delete;
    MppImport& operator=(const MppImport&) = delete;
    MppImport(MppImport&& other) noexcept
        : buffer{std::exchange(other.buffer, nullptr)}, log{other.log} {}
    MppImport& operator=(MppImport&& other) noexcept {
        if (this != &other) {
            release();
            buffer = std::exchange(other.buffer, nullptr);
            log = other.log;
        }
        return *this;
    }
    ~MppImport() {
        release();
    }

    int descriptor() const noexcept {
        return mpp_buffer_get_fd_with_caller(buffer, "descriptor");
    }
    std::size_t size() const noexcept {
        return mpp_buffer_get_size_with_caller(buffer, "size");
    }
    bool hasAddress() const noexcept {
        return mpp_buffer_get_ptr_with_caller(buffer, "address") != nullptr;
    }
    MppBuffer handle() const noexcept {
        return buffer;
    }

private:
    MppImport(MppBuffer imported, Logger logger) noexcept : buffer{imported}, log{logger} {}

    void release() noexcept {
        if (buffer == nullptr) {
            return;
        }
        const MPP_RET status{mpp_buffer_put_with_caller(std::exchange(buffer, nullptr), "release")};
        if (status != MPP_OK) {
            log.error("mpp_buffer_put ret=", static_cast<int>(status));
        }
    }

    MppBuffer buffer{nullptr};
    Logger log;
};

// Width and height of a frame in pixels.
struct FrameSize {
    int width;
    int height;
};

constexpr FrameSize encSize{encWidth, encHeight};

// An MppEncCfg.
class EncoderConfig {
public:
    static std::optional<EncoderConfig> create(const Logger& log) {
        MppEncCfg raw{nullptr};
        const MPP_RET status{mpp_enc_cfg_init(&raw)};
        if (status != MPP_OK) {
            log.error("mpp_enc_cfg_init ret=", static_cast<int>(status));
            return std::nullopt;
        }
        return std::optional<EncoderConfig>{EncoderConfig{raw, log}};
    }

    EncoderConfig(const EncoderConfig&) = delete;
    EncoderConfig& operator=(const EncoderConfig&) = delete;
    EncoderConfig(EncoderConfig&& other) noexcept
        : config{std::exchange(other.config, nullptr)}, log{other.log} {}
    EncoderConfig& operator=(EncoderConfig&& other) noexcept {
        if (this != &other) {
            release();
            config = std::exchange(other.config, nullptr);
            log = other.log;
        }
        return *this;
    }
    ~EncoderConfig() {
        release();
    }

    MppEncCfg get() const noexcept {
        return config;
    }

    // mpp_enc_cfg_set_s32 of one named setting.
    [[nodiscard]] bool set(const char* name, std::int32_t value) const noexcept {
        const MPP_RET status{mpp_enc_cfg_set_s32(config, name, value)};
        if (status != MPP_OK) {
            log.error("mpp_enc_cfg_set_s32 ", name, " ret=", static_cast<int>(status));
            return false;
        }
        return true;
    }

private:
    EncoderConfig(MppEncCfg created, Logger logger) noexcept : config{created}, log{logger} {}

    void release() noexcept {
        if (config == nullptr) {
            return;
        }
        const MPP_RET status{mpp_enc_cfg_deinit(std::exchange(config, nullptr))};
        if (status != MPP_OK) {
            log.error("mpp_enc_cfg_deinit ret=", static_cast<int>(status));
        }
    }

    MppEncCfg config{nullptr};
    Logger log;
};

// An MppFrame that points at an imported buffer, NV12 with stride equal to width.
class EncoderFrame {
public:
    static std::optional<EncoderFrame> create(FrameSize size, const MppImport& source,
                                              const Logger& log) {
        MppFrame raw{nullptr};
        const MPP_RET status{mpp_frame_init(&raw)};
        if (status != MPP_OK) {
            log.error("mpp_frame_init ret=", static_cast<int>(status));
            return std::nullopt;
        }
        EncoderFrame frame{raw, log};
        mpp_frame_set_width(raw, static_cast<RK_U32>(size.width));
        mpp_frame_set_height(raw, static_cast<RK_U32>(size.height));
        mpp_frame_set_hor_stride(raw, static_cast<RK_U32>(size.width));
        mpp_frame_set_ver_stride(raw, static_cast<RK_U32>(size.height));
        mpp_frame_set_fmt(raw, MPP_FMT_YUV420SP);
        mpp_frame_set_eos(raw, 0);
        mpp_frame_set_buffer(raw, source.handle());
        return std::optional<EncoderFrame>{std::move(frame)};
    }

    EncoderFrame(const EncoderFrame&) = delete;
    EncoderFrame& operator=(const EncoderFrame&) = delete;
    EncoderFrame(EncoderFrame&& other) noexcept
        : frame{std::exchange(other.frame, nullptr)}, log{other.log} {}
    EncoderFrame& operator=(EncoderFrame&& other) noexcept {
        if (this != &other) {
            release();
            frame = std::exchange(other.frame, nullptr);
            log = other.log;
        }
        return *this;
    }
    ~EncoderFrame() {
        release();
    }

    MppFrame get() const noexcept {
        return frame;
    }

private:
    EncoderFrame(MppFrame created, Logger logger) noexcept : frame{created}, log{logger} {}

    void release() noexcept {
        if (frame == nullptr) {
            return;
        }
        const MPP_RET status{mpp_frame_deinit(&frame)};
        frame = nullptr;
        if (status != MPP_OK) {
            log.error("mpp_frame_deinit ret=", static_cast<int>(status));
        }
    }

    MppFrame frame{nullptr};
    Logger log;
};

// An encoded packet, released when it goes out of scope.
class EncodedPacket {
public:
    EncodedPacket(MppPacket received, Logger logger) noexcept : packet{received}, log{logger} {}
    EncodedPacket(const EncodedPacket&) = delete;
    EncodedPacket& operator=(const EncodedPacket&) = delete;
    EncodedPacket(EncodedPacket&&) = delete;
    EncodedPacket& operator=(EncodedPacket&&) = delete;
    ~EncodedPacket() {
        const MPP_RET status{mpp_packet_deinit(&packet)};
        if (status != MPP_OK) {
            log.error("mpp_packet_deinit ret=", static_cast<int>(status));
        }
    }

    std::size_t length() const noexcept {
        return mpp_packet_get_length(packet);
    }

private:
    MppPacket packet;
    Logger log;
};

// One MPP H.265 encoder, CBR, one frame in flight.
class MppEncoder {
public:
    // mpp_create, output timeout, mpp_init(ENC, HEVC) and the settings for `width` x `height`.
    static std::optional<MppEncoder> create(FrameSize size, const Logger& log) {
        MppCtx raw{nullptr};
        MppApi* api{nullptr};
        const MPP_RET created{mpp_create(&raw, &api)};
        if (created != MPP_OK) {
            log.error("mpp_create ret=", static_cast<int>(created));
            return std::nullopt;
        }
        MppEncoder encoder{raw, api, log};
        std::int64_t timeout{encTimeoutMs};
        if (api->control(raw, MPP_SET_OUTPUT_TIMEOUT, &timeout) != MPP_OK) {
            log.error("MPP_SET_OUTPUT_TIMEOUT failed");
            return std::nullopt;
        }
        const MPP_RET initialised{mpp_init(raw, MPP_CTX_ENC, MPP_VIDEO_CodingHEVC)};
        if (initialised != MPP_OK) {
            log.error("mpp_init(ENC, HEVC) ret=", static_cast<int>(initialised));
            return std::nullopt;
        }
        if (!encoder.configure(size)) {
            return std::nullopt;
        }
        return std::optional<MppEncoder>{std::move(encoder)};
    }

    MppEncoder(const MppEncoder&) = delete;
    MppEncoder& operator=(const MppEncoder&) = delete;
    MppEncoder(MppEncoder&& other) noexcept
        : context{std::exchange(other.context, nullptr)}, api{other.api}, log{other.log} {}
    MppEncoder& operator=(MppEncoder&& other) noexcept {
        if (this != &other) {
            release();
            context = std::exchange(other.context, nullptr);
            api = other.api;
            log = other.log;
        }
        return *this;
    }
    ~MppEncoder() {
        release();
    }

    // Encodes one frame and waits for its packet. Returns the packet length in bytes.
    std::optional<std::size_t> encode(const EncoderFrame& frame) {
        const MPP_RET put{api->encode_put_frame(context, frame.get())};
        if (put != MPP_OK) {
            log.error("encode_put_frame ret=", static_cast<int>(put));
            return std::nullopt;
        }
        MppPacket raw{nullptr};
        const MPP_RET got{api->encode_get_packet(context, &raw)};
        if (got != MPP_OK || raw == nullptr) {
            log.error("encode_get_packet ret=", static_cast<int>(got),
                      raw == nullptr ? " no packet" : "");
            return std::nullopt;
        }
        const EncodedPacket packet{raw, log};
        return packet.length();
    }

private:
    MppEncoder(MppCtx created, MppApi* functions, Logger logger) noexcept
        : context{created}, api{functions}, log{logger} {}

    bool configure(FrameSize size) {
        const std::optional<EncoderConfig> config{EncoderConfig::create(log)};
        if (!config || api->control(context, MPP_ENC_GET_CFG, config->get()) != MPP_OK) {
            return false;
        }
        const std::array<std::pair<const char*, std::int32_t>, 14> settings{{
            {"prep:width", size.width},
            {"prep:height", size.height},
            {"prep:hor_stride", size.width},
            {"prep:ver_stride", size.height},
            {"prep:format", MPP_FMT_YUV420SP},
            {"rc:mode", MPP_ENC_RC_MODE_CBR},
            {"rc:bps_target", encBitrate},
            {"rc:fps_in_flex", 0},
            {"rc:fps_in_num", encFps},
            {"rc:fps_in_denorm", 1},
            {"rc:fps_out_flex", 0},
            {"rc:fps_out_num", encFps},
            {"rc:fps_out_denorm", 1},
            {"rc:gop", encFps},
        }};
        for (const auto& setting : settings) {
            if (!config->set(setting.first, setting.second)) {
                return false;
            }
        }
        if (api->control(context, MPP_ENC_SET_CFG, config->get()) != MPP_OK) {
            log.error("MPP_ENC_SET_CFG failed");
            return false;
        }
        return true;
    }

    void release() noexcept {
        if (context == nullptr) {
            return;
        }
        const MPP_RET status{mpp_destroy(std::exchange(context, nullptr))};
        if (status != MPP_OK) {
            log.error("mpp_destroy ret=", static_cast<int>(status));
        }
    }

    MppCtx context{nullptr};
    MppApi* api{nullptr};
    Logger log;
};

// One loaded RKNN model.
class RknnContext {
public:
    static std::optional<RknnContext> load(std::vector<char>& blob, const Logger& log) {
        rknn_context raw{0};
        const int status{
            rknn_init(&raw, blob.data(), static_cast<std::uint32_t>(blob.size()), 0, nullptr)};
        if (status != 0) {
            log.error("rknn_init ret=", status);
            return std::nullopt;
        }
        return std::optional<RknnContext>{RknnContext{raw, log}};
    }

    RknnContext(const RknnContext&) = delete;
    RknnContext& operator=(const RknnContext&) = delete;
    RknnContext(RknnContext&& other) noexcept
        : context{std::exchange(other.context, 0)}, loaded{std::exchange(other.loaded, false)},
          log{other.log} {}
    RknnContext& operator=(RknnContext&& other) noexcept {
        if (this != &other) {
            release();
            context = std::exchange(other.context, 0);
            loaded = std::exchange(other.loaded, false);
            log = other.log;
        }
        return *this;
    }
    ~RknnContext() {
        release();
    }

    rknn_context get() const noexcept {
        return context;
    }

private:
    RknnContext(rknn_context raw, Logger logger) noexcept
        : context{raw}, loaded{true}, log{logger} {}

    void release() noexcept {
        if (!loaded) {
            return;
        }
        loaded = false;
        const int status{rknn_destroy(context)};
        if (status != 0) {
            log.error("rknn_destroy ret=", status);
        }
    }

    rknn_context context{0};
    bool loaded{false};
    Logger log;
};

// A DmaBuf attached to an RKNN context as the input tensor.
class RknnInputMem {
public:
    // rknn_create_mem_from_fd then rknn_set_io_mem with `attr`. Empty when either call fails.
    static std::optional<RknnInputMem> attach(const RknnContext& model, DmaBuf& source,
                                              rknn_tensor_attr& attr, const Logger& log) {
        rknn_tensor_mem* memory{rknn_create_mem_from_fd(
            model.get(), source.fd(), source.data(), static_cast<std::uint32_t>(source.size()), 0)};
        if (memory == nullptr) {
            log.error("rknn_create_mem_from_fd returned null for fd=", source.fd());
            return std::nullopt;
        }
        RknnInputMem owner{model.get(), memory, log};
        const int status{rknn_set_io_mem(model.get(), memory, &attr)};
        if (status != 0) {
            log.error("rknn_set_io_mem ret=", status);
            return std::nullopt;
        }
        return std::optional<RknnInputMem>{std::move(owner)};
    }

    RknnInputMem(const RknnInputMem&) = delete;
    RknnInputMem& operator=(const RknnInputMem&) = delete;
    RknnInputMem(RknnInputMem&& other) noexcept
        : context{other.context}, memory{std::exchange(other.memory, nullptr)}, log{other.log} {}
    RknnInputMem& operator=(RknnInputMem&& other) noexcept {
        if (this != &other) {
            release();
            context = other.context;
            memory = std::exchange(other.memory, nullptr);
            log = other.log;
        }
        return *this;
    }
    ~RknnInputMem() {
        release();
    }

private:
    RknnInputMem(rknn_context owner, rknn_tensor_mem* created, Logger logger) noexcept
        : context{owner}, memory{created}, log{logger} {}

    void release() noexcept {
        if (memory == nullptr) {
            return;
        }
        const int status{rknn_destroy_mem(context, std::exchange(memory, nullptr))};
        if (status != 0) {
            log.error("rknn_destroy_mem ret=", status);
        }
    }

    rknn_context context{0};
    rknn_tensor_mem* memory{nullptr};
    Logger log;
};

// rknn_run, then output 0 as float bytes. Empty on failure.
std::optional<std::vector<char>> runAndReadOutput(const RknnContext& model, const Logger& log) {
    int status{rknn_run(model.get(), nullptr)};
    if (status != 0) {
        log.error("rknn_run ret=", status);
        return std::nullopt;
    }
    rknn_output output{};
    output.want_float = 1;
    status = rknn_outputs_get(model.get(), 1, &output, nullptr);
    if (status != 0) {
        log.error("rknn_outputs_get ret=", status);
        return std::nullopt;
    }
    const char* first{static_cast<const char*>(output.buf)};
    std::vector<char> bytes(first, first + output.size);
    status = rknn_outputs_release(model.get(), 1, &output);
    if (status != 0) {
        log.error("rknn_outputs_release ret=", status);
        return std::nullopt;
    }
    return bytes;
}

// Reference run: the image goes in through rknn_inputs_set.
std::optional<std::vector<char>> runWithCopy(const RknnContext& model,
                                             std::vector<std::uint8_t>& image, const Logger& log) {
    rknn_input input{};
    input.index = 0;
    input.type = RKNN_TENSOR_UINT8;
    input.fmt = RKNN_TENSOR_NHWC;
    input.size = static_cast<std::uint32_t>(image.size());
    input.buf = image.data();
    const int status{rknn_inputs_set(model.get(), 1, &input)};
    if (status != 0) {
        log.error("rknn_inputs_set ret=", status);
        return std::nullopt;
    }
    return runAndReadOutput(model, log);
}

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

// Fixed pseudo-random bytes, the same on every run.
std::vector<std::uint8_t> makeImage(std::size_t count) {
    std::vector<std::uint8_t> image(count);
    std::minstd_rand engine{fixedSeed};
    for (std::uint8_t& pixel : image) {
        pixel = static_cast<std::uint8_t>(engine() & byteMask);
    }
    return image;
}

// Logs one line per check, the result that the DmaBuf design is read from.
bool report(const char* heap, const char* check, bool passed, const Logger& log) {
    if (passed) {
        log.info("heap=", heap, " check=", check, " ok");
    } else {
        log.error("heap=", heap, " check=", check, " FAIL");
    }
    return passed;
}

// Allocate 4K NV12 bytes, write a pattern, read it back, sync both ways.
bool checkMap(const char* heap, const Logger& log) {
    std::optional<DmaBuf> buffer{DmaBuf::allocate(DmaBufConfig{heap, nv12Bytes4k}, log)};
    if (!buffer) {
        return false;
    }
    const bool written{fillBytes(*buffer, 0, buffer->size(), mapPattern)};
    const bool flushed{buffer->endCpuAccess()};
    const bool refreshed{buffer->beginCpuAccess()};
    const std::size_t wrong{countNot(*buffer, 0, buffer->size(), mapPattern)};
    if (wrong != 0) {
        log.error("read back ", wrong, " wrong bytes");
    }
    return written && flushed && refreshed && wrong == 0;
}

// Fill the source, clear the target, resize on the RGA, check the target on the CPU.
bool checkRga(const char* heap, const Logger& log) {
    const std::size_t srcLuma{lumaSize(srcWidth, srcHeight)};
    const std::size_t dstLuma{lumaSize(dstWidth, dstHeight)};
    std::optional<DmaBuf> source{
        DmaBuf::allocate(DmaBufConfig{heap, nv12Size(srcWidth, srcHeight)}, log)};
    std::optional<DmaBuf> target{
        DmaBuf::allocate(DmaBufConfig{heap, nv12Size(dstWidth, dstHeight)}, log)};
    if (!source || !target) {
        return false;
    }
    const bool prepared{fillBytes(*source, 0, srcLuma, lumaValue) &&
                        fillBytes(*source, srcLuma, source->size() - srcLuma, chromaValue) &&
                        fillBytes(*target, 0, target->size(), clearValue) &&
                        source->endCpuAccess() && target->endCpuAccess()};
    if (!prepared) {
        return false;
    }
    const std::optional<RgaHandle> sourceHandle{RgaHandle::import(*source, log)};
    const std::optional<RgaHandle> targetHandle{RgaHandle::import(*target, log)};
    if (!sourceHandle || !targetHandle || !resizeNv12(*sourceHandle, *targetHandle, log) ||
        !target->beginCpuAccess()) {
        return false;
    }
    const std::size_t wrongLuma{countNot(*target, 0, dstLuma, lumaValue)};
    const std::size_t wrongChroma{
        countNot(*target, dstLuma, target->size() - dstLuma, chromaValue)};
    log.info("RGA result: wrong luma=", wrongLuma, " wrong chroma=", wrongChroma);
    return wrongLuma == 0 && wrongChroma == 0;
}

// A flat NV12 image: constant luma, constant chroma.
std::vector<std::uint8_t> makeFlatImage(int width, int height) {
    const std::size_t luma{lumaSize(width, height)};
    std::vector<std::uint8_t> image(nv12Size(width, height), chromaValue);
    std::fill(image.begin(), image.begin() + static_cast<std::ptrdiff_t>(luma), lumaValue);
    return image;
}

// Writes `content` into the heap buffer, encodes it with a new encoder, returns the packet length.
std::optional<std::size_t> encodeContent(DmaBuf& buffer, const MppImport& imported,
                                         const std::vector<std::uint8_t>& content,
                                         const Logger& log) {
    if (!storeBytes(buffer, content) || !buffer.endCpuAccess()) {
        return std::nullopt;
    }
    std::optional<MppEncoder> encoder{MppEncoder::create(encSize, log)};
    const std::optional<EncoderFrame> frame{EncoderFrame::create(encSize, imported, log)};
    if (!encoder || !frame) {
        return std::nullopt;
    }
    return encoder->encode(*frame);
}

// Import the buffer, then encode a noise image and a flat image through it. The packet of the
// noise must be much larger, which shows that the encoder reads the heap buffer.
bool checkMpp(const char* heap, const Logger& log) {
    std::optional<DmaBuf> buffer{
        DmaBuf::allocate(DmaBufConfig{heap, nv12Size(encWidth, encHeight)}, log)};
    if (!buffer) {
        return false;
    }
    const std::optional<MppImport> imported{MppImport::import(*buffer, log)};
    if (!imported) {
        return false;
    }
    log.info("MPP import: fd=", imported->descriptor(), " (ours ", buffer->fd(),
             ") size=", imported->size(), " address=", imported->hasAddress() ? "yes" : "no");
    const std::optional<std::size_t> noisy{
        encodeContent(*buffer, *imported, makeImage(buffer->size()), log)};
    const std::optional<std::size_t> flat{
        encodeContent(*buffer, *imported, makeFlatImage(encWidth, encHeight), log)};
    if (!noisy || !flat) {
        return false;
    }
    log.info("MPP encode: packet of noise=", *noisy, " bytes, of flat image=", *flat, " bytes");
    return *flat > 0 && *noisy > *flat * noiseOverFlat;
}

// Reference run with rknn_inputs_set, then the same image in a heap buffer. Outputs must match.
bool checkRknn(const char* heap, const RknnContext& model, const Logger& log) {
    std::vector<std::uint8_t> image{makeImage(modelInputBytes)};
    const std::optional<std::vector<char>> reference{runWithCopy(model, image, log)};
    if (!reference) {
        return false;
    }
    std::optional<DmaBuf> buffer{DmaBuf::allocate(DmaBufConfig{heap, modelInputBytes}, log)};
    if (!buffer || !storeBytes(*buffer, image) || !buffer->endCpuAccess()) {
        return false;
    }
    rknn_tensor_attr attr{};
    attr.index = 0;
    const int queried{rknn_query(model.get(), RKNN_QUERY_INPUT_ATTR, &attr, sizeof(attr))};
    if (queried != 0) {
        log.error("rknn_query(INPUT_ATTR) ret=", queried);
        return false;
    }
    log.info("RKNN input: size=", attr.size, " size_with_stride=", attr.size_with_stride);
    attr.type = RKNN_TENSOR_UINT8;
    attr.fmt = RKNN_TENSOR_NHWC;
    const std::optional<RknnInputMem> attached{RknnInputMem::attach(model, *buffer, attr, log)};
    if (!attached) {
        return false;
    }
    const std::optional<std::vector<char>> result{runAndReadOutput(model, log)};
    if (!result) {
        return false;
    }
    const bool same{*result == *reference};
    log.info("RKNN output: ", result->size(), " bytes, ", same ? "equal to" : "DIFFERENT from",
             " the rknn_inputs_set run");
    return same;
}

// The four checks of one heap. The model is loaded once per heap: set_io_mem changes the context.
bool checkHeap(const char* heap, std::vector<char>& blob, const Logger& log) {
    log.info("heap ", heap);
    const bool mapped{report(heap, "map", checkMap(heap, log), log)};
    const bool resized{report(heap, "rga", checkRga(heap, log), log)};
    const bool imported{report(heap, "mpp", checkMpp(heap, log), log)};
    std::optional<RknnContext> model{RknnContext::load(blob, log)};
    const bool inferred{report(heap, "rknn", model && checkRknn(heap, *model, log), log)};
    return mapped && resized && imported && inferred;
}
} // namespace

int main(int argc, char** argv) {
    const std::vector<const char*> args{commandLine(argc, argv)};
    const Logger log{"PROBE_DMABUF", LogLevel::Debug};
    if (args.size() != argsCount) {
        log.error("usage: ", args.empty() ? "probe_dmabuf" : args.at(0), " model.rknn");
        return exitUsage;
    }
    std::optional<std::vector<char>> blob{readFile(args.at(modelArg))};
    if (!blob) {
        log.error("cannot read model file: ", args.at(modelArg));
        return exitUsage;
    }

    bool passed{true};
    for (const char* heap : heapNames) {
        passed = checkHeap(heap, *blob, log) && passed;
    }
    if (passed) {
        log.info("PASS");
    } else {
        log.error("FAIL");
    }
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
