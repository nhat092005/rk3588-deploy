// Mp4Writer on the board: the packets of a real H.265 stream (the MPP encoder) go into a fragmented
// MP4 that libavformat reads back with the picture size, one packet for each frame, key frames and
// times as given; a file that is cut short still plays up to the last complete fragment; the
// writer refuses a first packet that is not a key frame and a descriptor that cannot be written.
// Needs /dev/dma_heap/system and the MPP H.265 encoder. Exit code 0 when every check passes.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

extern "C" {
#include <libavformat/avformat.h>
}

#include "dmabuf.h"
#include "handles.h"
#include "logger.h"
#include "mp4_writer.h"
#include "mpp_source.h"
#include "video_encoder.h"

namespace {
namespace fs = std::filesystem;

constexpr int imageWidth{1280};
constexpr int imageHeight{720};
constexpr std::size_t nv12Bytes{static_cast<std::size_t>(imageWidth) * imageHeight * 3 / 2};
constexpr int gopFrames{10};
constexpr int streamFrames{65};
constexpr int streamFps{30};
constexpr int kbps{4'000};
constexpr double frameStep{1.0 / streamFps};
constexpr double firstSeconds{1000.5};
constexpr double timeTolerance{0.002};
constexpr std::uint8_t gradientNoiseMask{7};
constexpr std::size_t gradientColumn{2};
constexpr std::size_t gradientRow{3};
constexpr std::size_t gradientStep{5};
constexpr unsigned byteMask{0xFFU};
constexpr std::size_t keptPercent{60};
constexpr std::size_t percent{100};

class Check {
public:
    explicit Check(const Logger& logger) : log{logger} {}

    void expect(bool condition, const char* what) {
        if (!condition) {
            log.error("FAIL: ", what);
            passed = false;
        }
    }

    bool ok() const {
        return passed;
    }

private:
    Logger log;
    bool passed{true};
};

struct Frame {
    std::vector<std::uint8_t> bytes;
    bool keyFrame{false};
};

// Encodes frames of a gradient that moves: a key frame every gopFrames frames.
std::vector<Frame> encodeStream(const Logger& log) {
    std::vector<Frame> frames;
    VideoConfig config;
    config.width = imageWidth;
    config.height = imageHeight;
    config.horStride = imageWidth;
    config.verStride = imageHeight;
    config.fullRange = true;
    config.kbps = kbps;
    config.framesPerSecond = streamFps;
    config.gopFrames = gopFrames;
    std::optional<VideoEncoder> encoder{VideoEncoder::create(config, log)};
    std::optional<DmaBuf> buffer{DmaBuf::allocate(DmaBufConfig{defaultHeap, nv12Bytes}, log)};
    if (!encoder || !buffer) {
        return frames;
    }
    const std::optional<MppSource> source{MppSource::import(*buffer, log)};
    if (!source) {
        return frames;
    }
    auto* bytes{static_cast<std::uint8_t*>(buffer->data())};
    for (int number{0}; number < streamFrames; ++number) {
        for (std::size_t index{0}; index < nv12Bytes; ++index) {
            const std::size_t column{index % imageWidth};
            const std::size_t row{index / imageWidth};
            bytes[index] = static_cast<std::uint8_t>(
                (column * gradientColumn + row * gradientRow +
                 static_cast<std::size_t>(number) * gradientStep + (index & gradientNoiseMask)) &
                byteMask);
        }
        if (!buffer->endCpuAccess()) {
            return {};
        }
        const std::optional<VideoPacket> packet{encoder->encode(*source)};
        if (!packet) {
            return {};
        }
        frames.push_back(Frame{std::vector<std::uint8_t>(packet->data, packet->data + packet->size),
                               packet->keyFrame});
    }
    return frames;
}

struct Readback {
    bool opened{false};
    int width{0};
    int height{0};
    bool isHevc{false};
    bool hasHvcc{false};
    std::vector<double> seconds;
    std::vector<bool> keys;
    std::vector<std::size_t> sizes;
};

// What libavformat finds in a file.
Readback readBack(const fs::path& path) {
    Readback result;
    AVFormatContext* format{nullptr};
    if (avformat_open_input(&format, path.c_str(), nullptr, nullptr) < 0) {
        return result;
    }
    result.opened = true;
    if (format->nb_streams == 1) {
        const AVCodecParameters* parameters{format->streams[0]->codecpar};
        result.width = parameters->width;
        result.height = parameters->height;
        result.isHevc = parameters->codec_id == AV_CODEC_ID_HEVC;
        result.hasHvcc = parameters->extradata_size > 0 && parameters->extradata[0] == 1;
    }
    AVPacket* packet{av_packet_alloc()};
    while (av_read_frame(format, packet) >= 0) {
        const AVRational base{format->streams[packet->stream_index]->time_base};
        result.seconds.push_back(static_cast<double>(packet->pts) * base.num / base.den);
        result.keys.push_back((packet->flags & AV_PKT_FLAG_KEY) != 0);
        result.sizes.push_back(static_cast<std::size_t>(packet->size));
        av_packet_unref(packet);
    }
    av_packet_free(&packet);
    avformat_close_input(&format);
    return result;
}

std::optional<fs::path> makeDirectory() {
    std::error_code error;
    std::string pattern{(fs::temp_directory_path(error) / "mp4_writer_XXXXXX").string()};
    if (error || mkdtemp(pattern.data()) == nullptr) {
        return std::nullopt;
    }
    return fs::path{pattern};
}

// Writes the frames through a writer. Frame n is at firstSeconds + n / fps.
bool writeFile(const fs::path& path, const std::vector<Frame>& frames, std::size_t count,
               bool finish, const Logger& log) {
    FileDescriptor file{::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644), log};
    if (!file.valid()) {
        return false;
    }
    const std::unique_ptr<VideoFileWriter> writer{makeMp4Writer(file.get(), log)};
    if (!writer) {
        return false;
    }
    for (std::size_t index{0}; index < count; ++index) {
        const Frame& frame{frames.at(index)};
        if (!writer->write(frame.bytes.data(), frame.bytes.size(), frame.keyFrame,
                           firstSeconds + static_cast<double>(index) * frameStep)) {
            return false;
        }
    }
    return !finish || writer->finish();
}

void checkWholeFile(Check& check, const fs::path& directory, const std::vector<Frame>& frames,
                    const Logger& log) {
    const fs::path path{directory / "whole.mp4"};
    check.expect(writeFile(path, frames, frames.size(), true, log), "the file is written");
    const Readback read{readBack(path)};
    check.expect(read.opened && read.isHevc, "libavformat opens it as H.265");
    check.expect(read.width == imageWidth && read.height == imageHeight,
                 "the picture size comes from the SPS");
    check.expect(read.hasHvcc, "it has the hvcC box");
    check.expect(read.seconds.size() == frames.size(), "one packet for each frame");
    bool timesRight{read.seconds.size() == frames.size()};
    bool keysRight{read.keys.size() == frames.size()};
    bool sizesRight{read.sizes.size() == frames.size()};
    for (std::size_t index{0}; index < std::min(read.seconds.size(), frames.size()); ++index) {
        timesRight = timesRight && std::abs(read.seconds.at(index) -
                                            static_cast<double>(index) * frameStep) < timeTolerance;
        keysRight = keysRight && read.keys.at(index) == frames.at(index).keyFrame;
        // The MP4 packet is the same picture in the length-prefixed form: as large as the Annex B
        // one to within the start codes and parameter sets.
        sizesRight = sizesRight && read.sizes.at(index) > 0 &&
                     read.sizes.at(index) <= frames.at(index).bytes.size();
    }
    check.expect(timesRight, "the times are the seconds that were given, from 0");
    check.expect(keysRight, "the key frames are the ones that were given");
    check.expect(sizesRight, "no packet is lost or made larger");
}

void checkCutShort(Check& check, const fs::path& directory, const std::vector<Frame>& frames,
                   const Logger& log) {
    // A file that was never finished (the process was killed): it has no trailer.
    const fs::path path{directory / "open.mp4"};
    check.expect(writeFile(path, frames, frames.size(), false, log), "a file that is not finished");
    const Readback open{readBack(path)};
    check.expect(open.opened && open.width == imageWidth &&
                     open.seconds.size() >= frames.size() - gopFrames,
                 "it plays up to the last complete group of frames");
    // The same, cut in the middle of a fragment.
    const fs::path whole{directory / "whole.mp4"};
    std::ifstream in{whole, std::ios::binary};
    const std::string content{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    const fs::path cut{directory / "cut.mp4"};
    std::ofstream out{cut, std::ios::binary};
    out.write(content.data(), static_cast<std::streamsize>(content.size() * keptPercent / percent));
    out.close();
    const Readback part{readBack(cut)};
    check.expect(part.opened && part.width == imageWidth && !part.seconds.empty() &&
                     part.seconds.size() < frames.size(),
                 "a file cut in the middle still opens and holds the first frames");
}

void checkRefusals(Check& check, const fs::path& directory, const std::vector<Frame>& frames,
                   const Logger& log) {
    // The first packet must be a key frame.
    {
        FileDescriptor file{
            ::open((directory / "delta.mp4").c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644), log};
        const std::unique_ptr<VideoFileWriter> writer{makeMp4Writer(file.get(), log)};
        const Frame* delta{nullptr};
        for (const Frame& frame : frames) {
            if (!frame.keyFrame) {
                delta = &frame;
                break;
            }
        }
        check.expect(writer && delta != nullptr &&
                         !writer->write(delta->bytes.data(), delta->bytes.size(), false, 0.0),
                     "a first packet that is not a key frame is refused");
        check.expect(writer && !writer->write(frames.front().bytes.data(),
                                              frames.front().bytes.size(), true, 0.0),
                     "and the writer stays failed");
    }
    // Packets without parameter sets cannot start a file.
    {
        FileDescriptor file{
            ::open((directory / "noheader.mp4").c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644), log};
        const std::unique_ptr<VideoFileWriter> writer{makeMp4Writer(file.get(), log)};
        const std::vector<std::uint8_t> junk(64, 0x55);
        check.expect(writer && !writer->write(junk.data(), junk.size(), true, 0.0),
                     "a key frame without VPS, SPS and PPS is refused");
    }
    // A descriptor that refuses the bytes.
    {
        FileDescriptor full{::open("/dev/full", O_WRONLY | O_CLOEXEC), log};
        const std::unique_ptr<VideoFileWriter> writer{makeMp4Writer(full.get(), log)};
        bool refused{!writer};
        for (std::size_t index{0}; writer && index < frames.size() && !refused; ++index) {
            const Frame& frame{frames.at(index)};
            refused = !writer->write(frame.bytes.data(), frame.bytes.size(), frame.keyFrame,
                                     static_cast<double>(index) * frameStep) ||
                      !writer->flush();
        }
        check.expect(refused && !(writer && writer->finish()), "a full disk is reported");
    }
}

void checkEqualTimes(Check& check, const fs::path& directory, const std::vector<Frame>& frames,
                     const Logger& log) {
    // Two packets at the same time, or going back: the muxer would refuse them, the writer moves
    // time forward by a tick.
    const fs::path path{directory / "equal.mp4"};
    FileDescriptor file{::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644), log};
    const std::unique_ptr<VideoFileWriter> writer{makeMp4Writer(file.get(), log)};
    bool ok{writer != nullptr};
    for (std::size_t index{0}; ok && index < gopFrames + 2; ++index) {
        const Frame& frame{frames.at(index)};
        ok = writer->write(frame.bytes.data(), frame.bytes.size(), frame.keyFrame, firstSeconds);
    }
    check.expect(ok && writer->finish(), "packets at the same time are accepted");
    check.expect(readBack(path).seconds.size() == gopFrames + 2, "and none is lost");
}
} // namespace

int main() {
    const Logger log{"TEST_MP4", LogLevel::None};
    const Logger report{"TEST_MP4", LogLevel::Debug};
    Check check{report};
    const std::vector<Frame> frames{encodeStream(log)};
    check.expect(frames.size() == streamFrames && frames.front().keyFrame,
                 "a real stream is encoded");
    const std::optional<fs::path> directory{makeDirectory()};
    check.expect(directory.has_value(), "temporary directory made");
    if (frames.size() == streamFrames && directory) {
        checkWholeFile(check, *directory, frames, log);
        checkCutShort(check, *directory, frames, log);
        checkRefusals(check, *directory, frames, log);
        checkEqualTimes(check, *directory, frames, log);
    }
    if (directory) {
        std::error_code error;
        fs::remove_all(*directory, error);
    }
    report.info(check.ok() ? "PASS" : "FAIL");
    return check.ok() ? 0 : 1;
}
