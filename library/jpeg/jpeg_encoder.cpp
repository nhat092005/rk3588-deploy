#include "jpeg_encoder.h"

#include <array>
#include <cstring>
#include <utility>
#include <vector>

#include "mpp_handles.h"
#include "rk_mpi.h"

namespace {
constexpr std::int64_t outputTimeoutMs{2000};
constexpr int evenMask{1};

bool configValid(const JpegConfig& config) {
    return config.fullRange && config.width > 0 && config.height > 0 &&
           ((config.width | config.height) & evenMask) == 0 && config.horStride >= config.width &&
           config.verStride >= config.height && config.quality >= minJpegQuality &&
           config.quality <= maxJpegQuality && config.capacity > 0;
}
} // namespace

struct JpegState {
    JpegState(const JpegConfig& config, Logger logger)
        : settings{config}, log{logger}, output(config.capacity) {}
    JpegState(const JpegState&) = delete;
    JpegState& operator=(const JpegState&) = delete;
    JpegState(JpegState&&) = delete;
    JpegState& operator=(JpegState&&) = delete;
    ~JpegState() {
        if (context != nullptr) {
            const MPP_RET status{mpp_destroy(context)};
            if (status != MPP_OK) {
                log.error("mpp_destroy ret=", static_cast<int>(status));
            }
        }
    }

    bool configure();

    JpegConfig settings;
    Logger log;
    MppCtx context{nullptr};
    MppApi* api{nullptr};
    std::vector<std::uint8_t> output;
};

// The settings of a JPEG encoder: input geometry and format, fixed quality.
bool JpegState::configure() {
    MppEncCfg raw{nullptr};
    if (mpp_enc_cfg_init(&raw) != MPP_OK) {
        log.error("mpp_enc_cfg_init failed");
        return false;
    }
    const MppConfigPtr owner{raw};
    if (api->control(context, MPP_ENC_GET_CFG, raw) != MPP_OK) {
        log.error("MPP_ENC_GET_CFG failed");
        return false;
    }
    const std::array<std::pair<const char*, std::int32_t>, 10> values{{
        {"prep:width", settings.width},
        {"prep:height", settings.height},
        {"prep:hor_stride", settings.horStride},
        {"prep:ver_stride", settings.verStride},
        {"prep:format", MPP_FMT_YUV420SP},
        {"prep:range", MPP_FRAME_RANGE_JPEG},
        {"rc:mode", MPP_ENC_RC_MODE_FIXQP},
        {"jpeg:q_factor", settings.quality},
        {"jpeg:qf_min", minJpegQuality},
        {"jpeg:qf_max", maxJpegQuality},
    }};
    for (const auto& value : values) {
        if (mpp_enc_cfg_set_s32(raw, value.first, value.second) != MPP_OK) {
            log.error("mpp_enc_cfg_set_s32 ", value.first, " failed");
            return false;
        }
    }
    if (api->control(context, MPP_ENC_SET_CFG, raw) != MPP_OK) {
        log.error("MPP_ENC_SET_CFG failed");
        return false;
    }
    return true;
}

std::optional<JpegEncoder> JpegEncoder::create(const JpegConfig& config, const Logger& log) {
    if (!configValid(config)) {
        log.error("bad jpeg config: ", config.width, "x", config.height, " stride ",
                  config.horStride, "x", config.verStride, " full range ", config.fullRange,
                  " quality ", config.quality, " capacity ", config.capacity);
        return std::nullopt;
    }
    auto state{std::make_unique<JpegState>(config, log)};
    if (mpp_create(&state->context, &state->api) != MPP_OK) {
        state->context = nullptr;
        log.error("mpp_create failed");
        return std::nullopt;
    }
    std::int64_t timeout{outputTimeoutMs};
    if (state->api->control(state->context, MPP_SET_OUTPUT_TIMEOUT, &timeout) != MPP_OK) {
        log.error("MPP_SET_OUTPUT_TIMEOUT failed");
        return std::nullopt;
    }
    const MPP_RET initialised{mpp_init(state->context, MPP_CTX_ENC, MPP_VIDEO_CodingMJPEG)};
    if (initialised != MPP_OK) {
        log.error("mpp_init(ENC, MJPEG) ret=", static_cast<int>(initialised));
        return std::nullopt;
    }
    if (!state->configure()) {
        return std::nullopt;
    }
    return std::optional<JpegEncoder>{JpegEncoder{std::move(state)}};
}

JpegEncoder::JpegEncoder(std::unique_ptr<JpegState> initial) noexcept : state{std::move(initial)} {}
JpegEncoder::JpegEncoder(JpegEncoder&& other) noexcept = default;
JpegEncoder& JpegEncoder::operator=(JpegEncoder&& other) noexcept = default;
JpegEncoder::~JpegEncoder() = default;

std::optional<std::size_t> JpegEncoder::encode(const MppSource& source) noexcept {
    JpegState& mpp{*state};
    MppFrame raw{nullptr};
    if (mpp_frame_init(&raw) != MPP_OK) {
        mpp.log.error("mpp_frame_init failed");
        return std::nullopt;
    }
    const MppFramePtr frame{raw};
    mpp_frame_set_width(raw, static_cast<RK_U32>(mpp.settings.width));
    mpp_frame_set_height(raw, static_cast<RK_U32>(mpp.settings.height));
    mpp_frame_set_hor_stride(raw, static_cast<RK_U32>(mpp.settings.horStride));
    mpp_frame_set_ver_stride(raw, static_cast<RK_U32>(mpp.settings.verStride));
    mpp_frame_set_fmt(raw, MPP_FMT_YUV420SP);
    mpp_frame_set_eos(raw, 0);
    mpp_frame_set_buffer(raw, static_cast<MppBuffer>(source.handle()));
    const MPP_RET put{mpp.api->encode_put_frame(mpp.context, raw)};
    if (put != MPP_OK) {
        mpp.log.error("encode_put_frame ret=", static_cast<int>(put));
        return std::nullopt;
    }
    MppPacket received{nullptr};
    const MPP_RET got{mpp.api->encode_get_packet(mpp.context, &received)};
    if (got != MPP_OK || received == nullptr) {
        mpp.log.error("encode_get_packet ret=", static_cast<int>(got),
                      received == nullptr ? " no packet" : "");
        return std::nullopt;
    }
    const MppPacketPtr packet{received};
    const std::size_t length{mpp_packet_get_length(received)};
    if (length == 0 || length > mpp.output.size()) {
        mpp.log.error("jpeg of ", length, " bytes, capacity ", mpp.output.size());
        return std::nullopt;
    }
    std::memcpy(mpp.output.data(), mpp_packet_get_pos(received), length);
    return length;
}

const std::uint8_t* JpegEncoder::data() const noexcept {
    return state->output.data();
}
