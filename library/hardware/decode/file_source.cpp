#include "file_source.h"

#include <poll.h>
#include <sys/eventfd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}
#include "cpu_affinity.h"
#include "rk_mpi.h"

namespace {
using Clock = std::chrono::steady_clock;

constexpr std::chrono::milliseconds idleWait{1};
constexpr std::chrono::seconds openTimeout{10};
constexpr int waitMilliseconds{2000};
// From this height on, a video that does not name its matrix is taken as BT.709, as players do.
constexpr int firstHdHeight{720};
constexpr double fallbackFps{30.0};
// Frames decoded ahead of the one that is released next. Each one holds a decoder buffer.
constexpr std::size_t lookahead{2};
// The thread that decodes is a small core: the pipeline keeps the big ones.
constexpr int feederCpu{2};
constexpr std::size_t nv12Numerator{3};
constexpr std::size_t nv12Denominator{2};
constexpr std::size_t maxMessage{128};

struct FormatCloser {
    void operator()(AVFormatContext* container) const noexcept {
        avformat_close_input(&container);
    }
};
struct FilterFree {
    void operator()(AVBSFContext* filter) const noexcept {
        av_bsf_free(&filter);
    }
};
struct PacketFree {
    void operator()(AVPacket* packet) const noexcept {
        av_packet_free(&packet);
    }
};

std::string avError(int code) {
    std::string text(maxMessage, '\0');
    av_strerror(code, text.data(), text.size());
    text.resize(std::char_traits<char>::length(text.c_str()));
    return text;
}

// The matrix of a frame: what the decoder reports, else by the height. The RGA has no BT.709 full
// range matrix, so a full range frame is converted as BT.601.
ColorMatrix chooseMatrix(MppFrameColorSpace space, bool fullRange, int height, const Logger& log) {
    ColorMatrix matrix{height >= firstHdHeight ? ColorMatrix::Bt709 : ColorMatrix::Bt601};
    switch (space) {
    case MPP_FRAME_SPC_BT709:
        matrix = ColorMatrix::Bt709;
        break;
    case MPP_FRAME_SPC_BT470BG:
    case MPP_FRAME_SPC_SMPTE170M:
    case MPP_FRAME_SPC_FCC:
    case MPP_FRAME_SPC_SMPTE240M:
        matrix = ColorMatrix::Bt601;
        break;
    case MPP_FRAME_SPC_UNSPECIFIED:
        break;
    default:
        log.warn("colour space ", static_cast<int>(space),
                 " is not supported, converted by the height of the picture");
        break;
    }
    if (fullRange && matrix == ColorMatrix::Bt709) {
        log.warn("the RGA has no BT.709 full range matrix: the video is converted as BT.601");
        matrix = ColorMatrix::Bt601;
    }
    return matrix;
}

// A frame that the decoder gave, waiting for its time.
struct Decoded {
    std::size_t index;
    bool error;
    Clock::time_point due;
};

enum class Pump : std::uint8_t { Progress, Idle, Done, Error };
enum class Next : std::uint8_t { Packet, End, Error };
} // namespace

struct FileSource::State {
    explicit State(Logger logger) : log{logger} {}
    State(const State&) = delete;
    State& operator=(const State&) = delete;
    State(State&&) = delete;
    State& operator=(State&&) = delete;
    ~State() {
        (void)shutdown();
    }

    bool openContainer(const std::string& path);
    bool openDecoder();
    bool configure(MppFrame frame);
    void handleFrame(MppFrame frame);
    Next nextPacket();
    bool putPacket(const AVPacket& packet);
    bool putEnd();
    Pump pump();
    void run();
    void fail(const std::string& why);
    void notify() const noexcept;
    bool sleepFor(std::chrono::milliseconds period);
    bool sleepUntil(Clock::time_point moment);
    void release(const Decoded& frame);
    Clock::time_point dueOf(std::int64_t pts);
    bool shutdown() noexcept;

    Logger log;
    // The file.
    std::unique_ptr<AVFormatContext, FormatCloser> container;
    std::unique_ptr<AVBSFContext, FilterFree> filter;
    std::unique_ptr<AVPacket, PacketFree> demuxed;
    std::unique_ptr<AVPacket, PacketFree> filtered;
    int videoIndex{-1};
    double timeBase{0.0};
    double frameSeconds{1.0 / fallbackFps};
    // The decoder and its buffers.
    MppCtx decoder{nullptr};
    MppApi* api{nullptr};
    MppBufferGroup group{nullptr};
    std::vector<DmaBuf> buffers;
    CameraFormat format;
    bool configured{false};
    // Only the thread that decodes touches these (after open() returns).
    bool havePending{false};
    bool flushed{false};
    bool endSent{false};
    bool gotEnd{false};
    std::deque<Decoded> decoded;
    std::int64_t decodedCount{0};
    std::optional<Clock::time_point> origin;
    std::int64_t originPts{0};
    double lastSeconds{0.0};
    std::int64_t sequence{0};
    // Shared with the pipeline.
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::vector<MppFrame> held;
    std::deque<Frame> ready;
    bool ended{false};
    bool failed{false};
    bool closing{false};
    bool closed{false};
    FileDescriptor event;
    std::thread feeder;
};

void FileSource::State::notify() const noexcept {
    // The counter only has to be non-zero: a full counter is as good.
    const std::uint64_t one{1};
    (void)::write(event.get(), &one, sizeof one);
}

void FileSource::State::fail(const std::string& why) {
    {
        const std::lock_guard<std::mutex> lock{mutex};
        if (failed) {
            return;
        }
        failed = true;
    }
    log.error("video source failed: ", why);
    notify();
}

bool FileSource::State::openContainer(const std::string& path) {
    event = FileDescriptor{::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC), log};
    if (!event.valid()) {
        log.error("eventfd failed");
        return false;
    }
    AVFormatContext* raw{nullptr};
    int code{avformat_open_input(&raw, path.c_str(), nullptr, nullptr)};
    if (code < 0) {
        log.error("cannot open ", path, ": ", avError(code));
        return false;
    }
    container.reset(raw);
    code = avformat_find_stream_info(raw, nullptr);
    if (code < 0) {
        log.error("cannot read the streams of ", path, ": ", avError(code));
        return false;
    }
    videoIndex = av_find_best_stream(raw, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (videoIndex < 0) {
        log.error(path, " has no video stream");
        return false;
    }
    const AVStream* stream{raw->streams[videoIndex]};
    const AVCodecParameters* parameters{stream->codecpar};
    const bool hevc{parameters->codec_id == AV_CODEC_ID_HEVC};
    if (!hevc && parameters->codec_id != AV_CODEC_ID_H264) {
        log.error(path, ": the video is ", avcodec_get_name(parameters->codec_id),
                  ", only H.264 and H.265 are supported");
        return false;
    }
    timeBase = av_q2d(stream->time_base);
    AVRational rate{stream->avg_frame_rate};
    if (rate.num <= 0 || rate.den <= 0) {
        rate = stream->r_frame_rate;
    }
    if (rate.num > 0 && rate.den > 0) {
        frameSeconds = 1.0 / av_q2d(rate);
    }
    const AVBitStreamFilter* annexB{
        av_bsf_get_by_name(hevc ? "hevc_mp4toannexb" : "h264_mp4toannexb")};
    AVBSFContext* rawFilter{nullptr};
    if (annexB == nullptr || av_bsf_alloc(annexB, &rawFilter) < 0) {
        log.error("no Annex-B filter for ", avcodec_get_name(parameters->codec_id));
        return false;
    }
    filter.reset(rawFilter);
    rawFilter->time_base_in = stream->time_base;
    code = avcodec_parameters_copy(rawFilter->par_in, parameters);
    if (code >= 0) {
        code = av_bsf_init(rawFilter);
    }
    if (code < 0) {
        log.error("cannot set up the Annex-B filter: ", avError(code));
        return false;
    }
    demuxed.reset(av_packet_alloc());
    filtered.reset(av_packet_alloc());
    if (!demuxed || !filtered) {
        log.error("av_packet_alloc failed");
        return false;
    }
    log.info("video ", path, ": ", avcodec_get_name(parameters->codec_id), " ", parameters->width,
             "x", parameters->height, ", ", 1.0 / frameSeconds, " fps");
    return true;
}

bool FileSource::State::openDecoder() {
    const bool hevc{container->streams[videoIndex]->codecpar->codec_id == AV_CODEC_ID_HEVC};
    if (mpp_create(&decoder, &api) != MPP_OK ||
        mpp_init(decoder, MPP_CTX_DEC, hevc ? MPP_VIDEO_CodingHEVC : MPP_VIDEO_CodingAVC) !=
            MPP_OK) {
        log.error("the MPP decoder cannot be created");
        return false;
    }
    // Do not block: the thread that decodes polls.
    RK_S64 timeout{0};
    if (api->control(decoder, MPP_SET_OUTPUT_TIMEOUT, &timeout) != MPP_OK) {
        log.error("MPP_SET_OUTPUT_TIMEOUT failed");
        return false;
    }
    return true;
}

// The first frame of the decoder only reports the picture: allocate the buffers for it, give them
// to the decoder as its buffer group and tell it that it can go on.
bool FileSource::State::configure(MppFrame frame) {
    const int width{static_cast<int>(mpp_frame_get_width(frame))};
    const int height{static_cast<int>(mpp_frame_get_height(frame))};
    const int horStride{static_cast<int>(mpp_frame_get_hor_stride(frame))};
    const int verStride{static_cast<int>(mpp_frame_get_ver_stride(frame))};
    const std::size_t bufferBytes{mpp_frame_get_buf_size(frame)};
    const MppFrameFormat pixels{mpp_frame_get_fmt(frame)};
    if (pixels != MPP_FMT_YUV420SP) {
        log.error("the decoder gives MPP pixel format ", static_cast<unsigned>(pixels),
                  ": only 8-bit NV12 is supported (no 10-bit, no compressed output)");
        return false;
    }
    if (width <= 0 || height <= 0 || horStride < width || verStride < height ||
        bufferBytes < static_cast<std::size_t>(horStride) * static_cast<std::size_t>(verStride) *
                          nv12Numerator / nv12Denominator) {
        log.error("bad picture from the decoder: ", width, "x", height, " stride ", horStride, "x",
                  verStride, " buffer ", bufferBytes);
        return false;
    }
    if (bufferBytes > fileSourceMaxBytes / fileSourceBuffers) {
        log.error("a decoder buffer of ", bufferBytes, " bytes, times ", fileSourceBuffers,
                  ", is above the limit of ", fileSourceMaxBytes, " bytes");
        return false;
    }
    if (mpp_buffer_group_get_external(&group, MPP_BUFFER_TYPE_DRM) != MPP_OK) {
        log.error("the external buffer group cannot be created");
        return false;
    }
    buffers.reserve(fileSourceBuffers);
    for (std::size_t index{0}; index < fileSourceBuffers; ++index) {
        std::optional<DmaBuf> buffer{DmaBuf::allocate(DmaBufConfig{defaultHeap, bufferBytes}, log)};
        if (!buffer) {
            return false;
        }
        MppBufferInfo info{};
        info.type = MPP_BUFFER_TYPE_DRM;
        info.size = bufferBytes;
        info.ptr = buffer->data();
        info.fd = buffer->fd();
        info.index = static_cast<int>(index);
        if (mpp_buffer_commit(group, &info) != MPP_OK) {
            log.error("mpp_buffer_commit failed for buffer ", index);
            return false;
        }
        buffers.push_back(std::move(*buffer));
    }
    if (api->control(decoder, MPP_DEC_SET_EXT_BUF_GROUP, group) != MPP_OK ||
        api->control(decoder, MPP_DEC_SET_INFO_CHANGE_READY, nullptr) != MPP_OK) {
        log.error("the decoder refused the buffer group");
        return false;
    }
    const bool fullRange{mpp_frame_get_color_range(frame) == MPP_FRAME_RANGE_JPEG};
    format.width = width;
    format.height = height;
    format.bytesPerLine = horStride;
    format.verticalStride = verStride;
    format.fullRange = fullRange;
    format.matrix = chooseMatrix(mpp_frame_get_colorspace(frame), fullRange, height, log);
    held.assign(fileSourceBuffers, nullptr);
    configured = true;
    log.info("decoder: ", width, "x", height, " stride ", horStride, "x", verStride, ", ",
             fileSourceBuffers, " buffers of ", bufferBytes, " bytes, ",
             fullRange ? "full" : "limited", " range, ",
             format.matrix == ColorMatrix::Bt709 ? "BT.709" : "BT.601");
    return true;
}

// When the frame with this pts is released: the first frame sets the origin of the clock. A file
// without pts is paced by the frame rate. Time never goes back.
Clock::time_point FileSource::State::dueOf(std::int64_t pts) {
    const bool hasPts{pts != AV_NOPTS_VALUE};
    if (!origin) {
        origin = Clock::now();
        originPts = pts;
    }
    double seconds{hasPts ? static_cast<double>(pts - originPts) * timeBase
                          : static_cast<double>(decodedCount) * frameSeconds};
    seconds = std::max(seconds, lastSeconds);
    lastSeconds = seconds;
    ++decodedCount;
    return *origin +
           std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>{seconds});
}

void FileSource::State::handleFrame(MppFrame frame) {
    if (mpp_frame_get_info_change(frame)) {
        if (configured) {
            fail("the picture size changed in the middle of the video");
        } else if (!configure(frame)) {
            fail("the decoder cannot be set up");
        }
        mpp_frame_deinit(&frame);
        return;
    }
    if (mpp_frame_get_eos(frame)) {
        gotEnd = true;
        mpp_frame_deinit(&frame);
        return;
    }
    MppBuffer buffer{mpp_frame_get_buffer(frame)};
    if (buffer == nullptr || mpp_frame_get_discard(frame) != 0) {
        mpp_frame_deinit(&frame);
        return;
    }
    const int index{mpp_buffer_get_index(buffer)};
    if (index < 0 || static_cast<std::size_t>(index) >= fileSourceBuffers) {
        mpp_frame_deinit(&frame);
        fail("a frame is not in a buffer of the source");
        return;
    }
    const auto slot{static_cast<std::size_t>(index)};
    if (!configured) {
        mpp_frame_deinit(&frame);
        fail("a frame came before the picture was reported");
        return;
    }
    {
        const std::lock_guard<std::mutex> lock{mutex};
        if (held.at(slot) != nullptr) {
            mpp_frame_deinit(&frame);
            fail("the decoder gave a buffer that is still in use");
            return;
        }
        held.at(slot) = frame;
    }
    const std::int64_t pts{mpp_frame_get_pts(frame)};
    decoded.push_back(Decoded{slot, mpp_frame_get_errinfo(frame) != 0, dueOf(pts)});
}

// Gets the next packet of the video in Annex-B form into `filtered`, reading the file as needed.
Next FileSource::State::nextPacket() {
    while (true) {
        if (havePending) {
            return Next::Packet;
        }
        int code{av_bsf_receive_packet(filter.get(), filtered.get())};
        if (code == 0) {
            havePending = true;
            return Next::Packet;
        }
        if (code == AVERROR_EOF) {
            return Next::End;
        }
        if (code != AVERROR(EAGAIN)) {
            fail("the Annex-B filter: " + avError(code));
            return Next::Error;
        }
        code = av_read_frame(container.get(), demuxed.get());
        if (code == AVERROR_EOF) {
            code = av_bsf_send_packet(filter.get(), nullptr);
            flushed = true;
        } else if (code >= 0) {
            if (demuxed->stream_index != videoIndex) {
                av_packet_unref(demuxed.get());
                continue;
            }
            code = av_bsf_send_packet(filter.get(), demuxed.get());
        }
        if (code < 0) {
            fail("reading the file: " + avError(code));
            return Next::Error;
        }
    }
}

// Hands a packet to the decoder. false when the decoder is full: try again later.
bool FileSource::State::putPacket(const AVPacket& packet) {
    MppPacket handle{nullptr};
    mpp_packet_init(&handle, packet.data, static_cast<std::size_t>(packet.size));
    mpp_packet_set_pts(handle, packet.pts);
    const MPP_RET result{api->decode_put_packet(decoder, handle)};
    mpp_packet_deinit(&handle);
    return result == MPP_OK;
}

// Tells the decoder that the video is over, so that it gives the frames that it keeps.
bool FileSource::State::putEnd() {
    MppPacket handle{nullptr};
    mpp_packet_init(&handle, nullptr, 0);
    mpp_packet_set_eos(handle);
    const MPP_RET result{api->decode_put_packet(decoder, handle)};
    mpp_packet_deinit(&handle);
    return result == MPP_OK;
}

// One step: take a frame of the decoder, or else give it a packet.
Pump FileSource::State::pump() {
    MppFrame frame{nullptr};
    if (api->decode_get_frame(decoder, &frame) == MPP_OK && frame != nullptr) {
        handleFrame(frame);
        const std::lock_guard<std::mutex> lock{mutex};
        return failed ? Pump::Error : Pump::Progress;
    }
    if (gotEnd) {
        return Pump::Done;
    }
    if (endSent) {
        return Pump::Idle;
    }
    const Next next{nextPacket()};
    if (next == Next::Error) {
        return Pump::Error;
    }
    if (next == Next::Packet) {
        if (!putPacket(*filtered)) {
            return Pump::Idle;
        }
        havePending = false;
        av_packet_unref(filtered.get());
        return Pump::Progress;
    }
    if (!putEnd()) {
        return Pump::Idle;
    }
    endSent = true;
    return Pump::Progress;
}

// Waits, or until the source is closed. false when it was closed.
bool FileSource::State::sleepFor(std::chrono::milliseconds period) {
    std::unique_lock<std::mutex> lock{mutex};
    wake.wait_for(lock, period, [this] { return closing; });
    return !closing;
}

bool FileSource::State::sleepUntil(Clock::time_point moment) {
    std::unique_lock<std::mutex> lock{mutex};
    wake.wait_until(lock, moment, [this] { return closing; });
    return !closing;
}

// Makes a frame available to the pipeline, stamped with the clock at this moment.
void FileSource::State::release(const Decoded& frame) {
    Frame out;
    out.index = frame.index;
    out.flaggedError = frame.error;
    out.captureSeconds = std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
    out.bytesUsed.at(0) = static_cast<std::size_t>(format.bytesPerLine) *
                          static_cast<std::size_t>(format.verticalStride) * nv12Numerator /
                          nv12Denominator;
    {
        const std::lock_guard<std::mutex> lock{mutex};
        out.sequence = sequence++;
        ready.push_back(out);
    }
    notify();
}

// The thread that decodes: keeps a few frames decoded ahead and releases the oldest when its time
// has come.
void FileSource::State::run() {
    if (!pinThreadToCpu(feederCpu)) {
        log.warn("could not pin the video thread to CPU ", feederCpu);
    }
    bool inputDone{false};
    while (true) {
        {
            const std::lock_guard<std::mutex> lock{mutex};
            if (closing || failed) {
                return;
            }
        }
        while (!inputDone && decoded.size() < lookahead) {
            const Pump step{pump()};
            if (step == Pump::Error) {
                return;
            }
            if (step == Pump::Done) {
                inputDone = true;
            }
            if (step == Pump::Idle || step == Pump::Done) {
                break;
            }
        }
        if (!decoded.empty()) {
            if (!sleepUntil(decoded.front().due)) {
                return;
            }
            release(decoded.front());
            decoded.pop_front();
            continue;
        }
        if (inputDone) {
            {
                const std::lock_guard<std::mutex> lock{mutex};
                ended = true;
            }
            notify();
            return;
        }
        if (!sleepFor(idleWait)) {
            return;
        }
    }
}

bool FileSource::State::shutdown() noexcept {
    if (closed) {
        return true;
    }
    closed = true;
    {
        const std::lock_guard<std::mutex> lock{mutex};
        closing = true;
    }
    wake.notify_all();
    if (feeder.joinable()) {
        feeder.join();
    }
    bool ok{true};
    {
        const std::lock_guard<std::mutex> lock{mutex};
        for (MppFrame& frame : held) {
            if (frame != nullptr) {
                mpp_frame_deinit(&frame);
                frame = nullptr;
            }
        }
    }
    if (decoder != nullptr) {
        // The reset first: destroying an HEVC decoder that has not been reset crashes (measured).
        if (api->reset(decoder) != MPP_OK) {
            log.error("the decoder reset failed");
            ok = false;
        }
        mpp_destroy(decoder);
        decoder = nullptr;
    }
    if (group != nullptr) {
        mpp_buffer_group_put(group);
        group = nullptr;
    }
    buffers.clear();
    return ok;
}

std::unique_ptr<FileSource> FileSource::open(const std::string& path, const Logger& log) {
    auto state{std::make_unique<State>(log)};
    if (!state->openContainer(path) || !state->openDecoder()) {
        return nullptr;
    }
    // Read ahead until the decoder reports the picture, which configures the buffers.
    const Clock::time_point deadline{Clock::now() + openTimeout};
    while (!state->configured) {
        const Pump step{state->pump()};
        if (step == Pump::Error) {
            return nullptr;
        }
        if (step == Pump::Done) {
            log.error(path, ": the decoder gave no picture");
            return nullptr;
        }
        if (step == Pump::Idle) {
            std::this_thread::sleep_for(idleWait);
        }
        if (Clock::now() > deadline) {
            log.error(path, ": the decoder did not report the picture in ", openTimeout.count(),
                      " s");
            return nullptr;
        }
    }
    State* const raw{state.get()};
    raw->feeder = std::thread{[raw] { raw->run(); }};
    return std::unique_ptr<FileSource>{new FileSource{std::move(state)}};
}

FileSource::FileSource(std::unique_ptr<State> initial) noexcept : state{std::move(initial)} {}

FileSource::~FileSource() {
    (void)close();
}

const CameraFormat& FileSource::format() const noexcept {
    return state->format;
}

std::size_t FileSource::bufferCount() const noexcept {
    return fileSourceBuffers;
}

const DmaBuf& FileSource::planeBuffer(std::size_t index, std::size_t /*plane*/) const {
    return state->buffers.at(index);
}

WaitStatus FileSource::waitFrame(const StopSignal& stop) const noexcept {
    State& source{*state};
    while (true) {
        std::array<pollfd, 2> descriptors{
            {{source.event.get(), POLLIN, 0}, {stop.descriptor(), POLLIN, 0}}};
        // A look without waiting first: the stop signal wins over a frame.
        int count{::poll(descriptors.data(), descriptors.size(), 0)};
        if (count > 0 && (descriptors.at(1).revents & POLLIN) != 0) {
            return WaitStatus::Stopped;
        }
        {
            const std::lock_guard<std::mutex> lock{source.mutex};
            if (source.failed) {
                return WaitStatus::Failed;
            }
            if (!source.ready.empty()) {
                return WaitStatus::Ready;
            }
            if (source.ended) {
                return WaitStatus::End;
            }
        }
        count = ::poll(descriptors.data(), descriptors.size(), waitMilliseconds);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            source.log.error("poll failed: ", lastError());
            return WaitStatus::Failed;
        }
        if (count == 0) {
            return WaitStatus::Timeout;
        }
        if ((descriptors.at(1).revents & POLLIN) != 0) {
            return WaitStatus::Stopped;
        }
        std::uint64_t counter{0};
        (void)::read(source.event.get(), &counter, sizeof counter);
    }
}

Dequeued FileSource::dequeue() const noexcept {
    State& source{*state};
    const std::lock_guard<std::mutex> lock{source.mutex};
    if (source.ready.empty()) {
        return Dequeued{DequeueStatus::NotReady, Frame{}};
    }
    const Frame frame{source.ready.front()};
    source.ready.pop_front();
    return Dequeued{DequeueStatus::Frame, frame};
}

bool FileSource::queue(std::size_t index) const noexcept {
    State& source{*state};
    MppFrame frame{nullptr};
    {
        const std::lock_guard<std::mutex> lock{source.mutex};
        if (index >= source.held.size() || source.held.at(index) == nullptr) {
            source.log.error("queue: buffer ", index, " is not held");
            return false;
        }
        frame = std::exchange(source.held.at(index), nullptr);
    }
    mpp_frame_deinit(&frame);
    return true;
}

bool FileSource::close() noexcept {
    return state->shutdown();
}
