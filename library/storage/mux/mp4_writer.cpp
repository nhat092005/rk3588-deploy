#include "mp4_writer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
}

namespace {
// Time of the file: 90 kHz, the clock of MPEG video.
constexpr int ticksPerSecond{90'000};
// Length of a frame (30 fps) until two packets have given a better one.
constexpr std::int64_t defaultFrameTicks{ticksPerSecond / 30};
constexpr int ioBufferBytes{1 << 16};
// NAL unit types below this are pictures (slices); the VPS, SPS and PPS come before them.
constexpr std::uint8_t firstNonPictureType{32};
constexpr std::size_t startCodeBytes{3};
constexpr int nalTypeShift{1};
constexpr int nalTypeMask{0x3f};
constexpr const char* fragmentFlags{"frag_keyframe+empty_moov+default_base_moof"};

std::string errorText(int code) {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> text{};
    av_strerror(code, text.data(), text.size());
    return std::string{text.data()};
}

// Bytes of the packet before its first picture: the parameter sets (VPS, SPS, PPS) and whatever
// goes with them. Zero when the packet starts with a picture or holds none.
std::size_t headerBytes(const std::uint8_t* data, std::size_t size) {
    for (std::size_t at{0}; at + startCodeBytes < size; ++at) {
        if (data[at] != 0 || data[at + 1] != 0 || data[at + 2] != 1) {
            continue;
        }
        const int type{(data[at + startCodeBytes] >> nalTypeShift) & nalTypeMask};
        if (type < firstNonPictureType) {
            // A four byte start code has one more zero in front.
            return at > 0 && data[at - 1] == 0 ? at - 1 : at;
        }
    }
    return 0;
}

class Mp4Writer final : public VideoFileWriter {
public:
    Mp4Writer(int fileDescriptor, const Logger& logger) : descriptor{fileDescriptor}, log{logger} {}

    Mp4Writer(const Mp4Writer&) = delete;
    Mp4Writer& operator=(const Mp4Writer&) = delete;
    Mp4Writer(Mp4Writer&&) = delete;
    Mp4Writer& operator=(Mp4Writer&&) = delete;
    ~Mp4Writer() override {
        release();
    }

    // Allocates what libavformat needs. false when it cannot (logged).
    bool open() {
        const int code{avformat_alloc_output_context2(&format, nullptr, "mp4", nullptr)};
        if (code < 0 || format == nullptr) {
            log.error("mp4: no muxer: ", errorText(code));
            return false;
        }
        auto* buffer{static_cast<std::uint8_t*>(av_malloc(ioBufferBytes))};
        if (buffer == nullptr) {
            log.error("mp4: out of memory");
            return false;
        }
        io = avio_alloc_context(buffer, ioBufferBytes, 1, this, nullptr, &writeThrough, nullptr);
        if (io == nullptr) {
            av_free(buffer);
            log.error("mp4: out of memory");
            return false;
        }
        format->pb = io;
        format->flags |= AVFMT_FLAG_CUSTOM_IO;
        packet = av_packet_alloc();
        return packet != nullptr && avformat_new_stream(format, nullptr) != nullptr;
    }

    bool write(const std::uint8_t* data, std::size_t size, bool keyFrame, double seconds) override {
        if (finished || failed) {
            return false;
        }
        if (!started) {
            if (!keyFrame || !start(data, size)) {
                if (!keyFrame) {
                    log.error("mp4: the first packet is not a key frame");
                }
                failed = true;
                return false;
            }
            firstSeconds = seconds;
        }
        AVStream* stream{format->streams[0]};
        const double elapsed{seconds - firstSeconds};
        std::int64_t ticks{std::llround(elapsed * stream->time_base.den / stream->time_base.num)};
        // Time must go forward by at least one tick, or the muxer refuses the packet.
        ticks = std::max(ticks, lastTicks + 1);
        if (lastTicks >= 0) {
            lastStep = ticks - lastTicks;
        }
        lastTicks = ticks;
        // The muxer copies what it keeps and never writes to the bytes (the API has no const), so
        // the packet may point at the caller's bytes.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
        packet->data = const_cast<std::uint8_t*>(data);
        packet->size = static_cast<int>(size);
        packet->stream_index = 0;
        packet->pts = ticks;
        packet->dts = ticks;
        packet->duration = lastStep;
        packet->flags = keyFrame ? AV_PKT_FLAG_KEY : 0;
        const int code{av_write_frame(format, packet)};
        if (code < 0 || ioFailed) {
            log.error("mp4: write: ", code < 0 ? errorText(code) : std::string{"disk"});
            failed = true;
            return false;
        }
        return true;
    }

    bool flush() override {
        if (io != nullptr) {
            avio_flush(io);
        }
        return !failed && !ioFailed;
    }

    bool finish() override {
        if (finished) {
            return !failed;
        }
        finished = true;
        if (started && !failed) {
            const int code{av_write_trailer(format)};
            if (code < 0) {
                log.error("mp4: trailer: ", errorText(code));
                failed = true;
            }
            avio_flush(io);
        }
        const bool succeeded{!failed && !ioFailed};
        release();
        return succeeded;
    }

private:
    // libavformat hands its buffer over here. The file is written through the caller's
    // descriptor, so that the file keeps its name, mode and fsync.
    static int writeThrough(void* opaque, std::uint8_t* buffer, int size) {
        auto* self{static_cast<Mp4Writer*>(opaque)};
        if (!writeBytes(self->descriptor, buffer, static_cast<std::size_t>(size), self->log)) {
            self->ioFailed = true;
            return AVERROR(EIO);
        }
        return size;
    }

    // Reads the picture size and the parameter sets from the first packet and writes the header.
    bool start(const std::uint8_t* data, std::size_t size) {
        const std::size_t header{headerBytes(data, size)};
        if (header == 0) {
            log.error("mp4: the first key frame has no VPS, SPS and PPS");
            return false;
        }
        int width{0};
        int height{0};
        if (!pictureSize(data, size, width, height)) {
            return false;
        }
        AVStream* stream{format->streams[0]};
        AVCodecParameters* parameters{stream->codecpar};
        parameters->codec_type = AVMEDIA_TYPE_VIDEO;
        parameters->codec_id = AV_CODEC_ID_HEVC;
        // hvc1 plays in QuickTime and Safari too, hev1 (the default) does not.
        parameters->codec_tag = MKTAG('h', 'v', 'c', '1');
        parameters->width = width;
        parameters->height = height;
        parameters->extradata =
            static_cast<std::uint8_t*>(av_mallocz(header + AV_INPUT_BUFFER_PADDING_SIZE));
        if (parameters->extradata == nullptr) {
            log.error("mp4: out of memory");
            return false;
        }
        std::copy_n(data, header, parameters->extradata);
        parameters->extradata_size = static_cast<int>(header);
        stream->time_base = AVRational{1, ticksPerSecond};
        AVDictionary* options{nullptr};
        av_dict_set(&options, "movflags", fragmentFlags, 0);
        const int code{avformat_write_header(format, &options)};
        av_dict_free(&options);
        if (code < 0 || ioFailed) {
            log.error("mp4: header: ", code < 0 ? errorText(code) : std::string{"disk"});
            return false;
        }
        started = true;
        return true;
    }

    // The size of the picture, as the SPS of the packet says it.
    bool pictureSize(const std::uint8_t* data, std::size_t size, int& width, int& height) {
        AVCodecParserContext* parser{av_parser_init(AV_CODEC_ID_HEVC)};
        AVCodecContext* context{avcodec_alloc_context3(nullptr)};
        if (parser == nullptr || context == nullptr) {
            log.error("mp4: no H.265 parser");
            av_parser_close(parser);
            avcodec_free_context(&context);
            return false;
        }
        context->codec_id = AV_CODEC_ID_HEVC;
        std::uint8_t* out{nullptr};
        int outSize{0};
        av_parser_parse2(parser, context, &out, &outSize, data, static_cast<int>(size),
                         AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
        // The parser reads the SPS of a frame when the frame is complete: an empty call ends it.
        av_parser_parse2(parser, context, &out, &outSize, nullptr, 0, AV_NOPTS_VALUE,
                         AV_NOPTS_VALUE, 0);
        width = parser->width;
        height = parser->height;
        av_parser_close(parser);
        avcodec_free_context(&context);
        if (width <= 0 || height <= 0) {
            log.error("mp4: the picture size could not be read from the SPS");
            return false;
        }
        return true;
    }

    void release() {
        if (format != nullptr) {
            format->pb = nullptr;
            avformat_free_context(format);
            format = nullptr;
        }
        if (io != nullptr) {
            av_free(io->buffer);
            io->buffer = nullptr;
            avio_context_free(&io);
        }
        av_packet_free(&packet);
    }

    int descriptor;
    Logger log;
    AVFormatContext* format{nullptr};
    AVIOContext* io{nullptr};
    AVPacket* packet{nullptr};
    bool started{false};
    bool finished{false};
    bool failed{false};
    bool ioFailed{false};
    double firstSeconds{0.0};
    std::int64_t lastTicks{-1};
    std::int64_t lastStep{defaultFrameTicks};
};
} // namespace

std::unique_ptr<VideoFileWriter> makeMp4Writer(int descriptor, const Logger& log) {
    auto writer{std::make_unique<Mp4Writer>(descriptor, log)};
    if (!writer->open()) {
        return nullptr;
    }
    return writer;
}
