#include "video_encoder.h"

#include <array>
#include <cstring>
#include <utility>
#include <vector>

#include "hevc_nal.h"
#include "mpp_handles.h"
#include "rk_mpi.h"

namespace {
constexpr std::int64_t outputTimeoutMs{2000};
constexpr int evenMask{1};
constexpr std::int32_t bitsPerKilobit{1000};
constexpr std::int32_t rateSpreadDivisor{16}; // bps_max and bps_min: the target plus and minus 1/16

bool configValid(const VideoConfig& config) {
    return config.width > 0 && config.height > 0 &&
           ((config.width | config.height) & evenMask) == 0 && config.horStride >= config.width &&
           config.verStride >= config.height && config.kbps > 0 && config.kbps <= maxVideoKbps &&
           config.framesPerSecond > 0 && config.framesPerSecond <= maxVideoFps &&
           config.gopFrames > 0 && config.capacity > 0;
}
} // namespace

struct VideoState {
    VideoState(const VideoConfig& config, Logger logger)
        : settings{config}, log{logger}, output(config.capacity) {}
    VideoState(const VideoState&) = delete;
    VideoState& operator=(const VideoState&) = delete;
    VideoState(VideoState&&) = delete;
    VideoState& operator=(VideoState&&) = delete;
    ~VideoState() {
        if (context != nullptr) {
            const MPP_RET status{mpp_destroy(context)};
            if (status != MPP_OK) {
                log.error("mpp_destroy ret=", static_cast<int>(status));
            }
        }
    }

    bool configure();

    VideoConfig settings;
    Logger log;
    MppCtx context{nullptr};
    MppApi* api{nullptr};
    std::vector<std::uint8_t> output;
};

// The settings of the stream: input geometry and format, constant bitrate, key frame interval.
bool VideoState::configure() {
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
    const std::int32_t target{settings.kbps * bitsPerKilobit};
    const std::int32_t spread{target / rateSpreadDivisor};
    const std::array<std::pair<const char*, std::int32_t>, 19> values{{
        {"prep:width", settings.width},
        {"prep:height", settings.height},
        {"prep:hor_stride", settings.horStride},
        {"prep:ver_stride", settings.verStride},
        {"prep:format", MPP_FMT_YUV420SP},
        {"prep:range", settings.fullRange ? MPP_FRAME_RANGE_JPEG : MPP_FRAME_RANGE_MPEG},
        {"codec:type", MPP_VIDEO_CodingHEVC},
        {"rc:mode", MPP_ENC_RC_MODE_CBR},
        {"rc:bps_target", target},
        {"rc:bps_max", target + spread},
        {"rc:bps_min", target - spread},
        {"rc:fps_in_flex", 0},
        {"rc:fps_in_num", settings.framesPerSecond},
        {"rc:fps_in_denorm", 1},
        {"rc:fps_out_flex", 0},
        {"rc:fps_out_num", settings.framesPerSecond},
        {"rc:fps_out_denorm", 1},
        {"rc:gop", settings.gopFrames},
        {"rc:drop_mode", MPP_ENC_RC_DROP_FRM_DISABLED},
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
    // VPS, SPS and PPS with every key frame, not only the first: a clip starts on any key frame.
    MppEncHeaderMode headerMode{MPP_ENC_HEADER_MODE_EACH_IDR};
    if (api->control(context, MPP_ENC_SET_HEADER_MODE, &headerMode) != MPP_OK) {
        log.error("MPP_ENC_SET_HEADER_MODE failed");
        return false;
    }
    return true;
}

std::optional<VideoEncoder> VideoEncoder::create(const VideoConfig& config, const Logger& log) {
    if (!configValid(config)) {
        log.error("bad video config: ", config.width, "x", config.height, " stride ",
                  config.horStride, "x", config.verStride, " ", config.kbps, " kbps ",
                  config.framesPerSecond, " fps gop ", config.gopFrames, " capacity ",
                  config.capacity);
        return std::nullopt;
    }
    auto state{std::make_unique<VideoState>(config, log)};
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
    const MPP_RET initialised{mpp_init(state->context, MPP_CTX_ENC, MPP_VIDEO_CodingHEVC)};
    if (initialised != MPP_OK) {
        log.error("mpp_init(ENC, HEVC) ret=", static_cast<int>(initialised));
        return std::nullopt;
    }
    if (!state->configure()) {
        return std::nullopt;
    }
    return std::optional<VideoEncoder>{VideoEncoder{std::move(state)}};
}

VideoEncoder::VideoEncoder(std::unique_ptr<VideoState> initial) noexcept
    : state{std::move(initial)} {}
VideoEncoder::VideoEncoder(VideoEncoder&& other) noexcept = default;
VideoEncoder& VideoEncoder::operator=(VideoEncoder&& other) noexcept = default;
VideoEncoder::~VideoEncoder() = default;

std::optional<VideoPacket> VideoEncoder::encode(const MppSource& source) noexcept {
    VideoState& mpp{*state};
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
        mpp.log.error("video packet of ", length, " bytes, capacity ", mpp.output.size());
        return std::nullopt;
    }
    std::memcpy(mpp.output.data(), mpp_packet_get_pos(received), length);
    return VideoPacket{mpp.output.data(), length, hasKeyFrame(mpp.output.data(), length)};
}
