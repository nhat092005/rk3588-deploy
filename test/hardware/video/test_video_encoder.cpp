// VideoEncoder on the board: the stream starts with VPS, SPS, PPS and an IDR picture, a key frame
// comes every gopFrames frames and only then, a flat stream is far smaller than a noisy one, the
// constant bitrate mode keeps a stream of moderate detail near its target, and a bad configuration
// or a too small output buffer is refused. Needs /dev/dma_heap/system and the MPP H.265 encoder.
// Exit code 0 when every check passes.
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <vector>

#include "dmabuf.h"
#include "hevc_nal.h"
#include "logger.h"
#include "mpp_source.h"
#include "video_encoder.h"

namespace {
constexpr int imageWidth{1280};
constexpr int imageHeight{720};
constexpr std::size_t nv12Bytes{static_cast<std::size_t>(imageWidth) * imageHeight * 3 / 2};
constexpr std::uint8_t flatLevel{128};
constexpr int gopFrames{10};
constexpr int streamFrames{35};
constexpr int lowKbps{4'000};
constexpr int streamFps{30};
constexpr std::size_t flatPFrameMaxBytes{20'000};
constexpr std::size_t tinyCapacity{100};
constexpr std::size_t noiseOverFlat{10};
constexpr double rateTolerance{1.5}; // the gradient stream is within 1/1.5 and 1.5 of its target
constexpr std::uint64_t noiseSeed{88172645463325252ULL};
constexpr unsigned shiftLeftFirst{13U};
constexpr unsigned shiftRight{7U};
constexpr unsigned shiftLeftSecond{17U};
constexpr unsigned byteMask{0xFFU};
constexpr std::size_t gradientColumn{2};
constexpr std::size_t gradientRow{3};
constexpr std::size_t gradientStep{5};
constexpr std::uint64_t gradientNoiseMask{7};
constexpr std::size_t bitsPerByte{8};
constexpr double bitsPerKilobit{1000.0};
constexpr std::array<std::uint8_t, 4> startCode{0x00, 0x00, 0x00, 0x01};

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

VideoConfig baseConfig() {
    VideoConfig config;
    config.width = imageWidth;
    config.height = imageHeight;
    config.horStride = imageWidth;
    config.verStride = imageHeight;
    config.fullRange = true;
    config.kbps = lowKbps;
    config.framesPerSecond = streamFps;
    config.gopFrames = gopFrames;
    return config;
}

enum class Content : std::uint8_t { Flat, Noise, Pattern };

// NV12 content: every byte the same, xorshift noise that goes on from `state`, or a diagonal
// gradient that moves by `number` with a little noise on it.
bool fill(DmaBuf& buffer, Content content, int number, std::uint64_t& state) {
    auto* bytes{static_cast<std::uint8_t*>(buffer.data())};
    for (std::size_t index{0}; index < nv12Bytes; ++index) {
        state ^= state << shiftLeftFirst;
        state ^= state >> shiftRight;
        state ^= state << shiftLeftSecond;
        std::uint8_t value{flatLevel};
        if (content == Content::Noise) {
            value = static_cast<std::uint8_t>(state & byteMask);
        } else if (content == Content::Pattern) {
            const std::size_t column{index % imageWidth};
            const std::size_t row{index / imageWidth};
            value = static_cast<std::uint8_t>((column * gradientColumn + row * gradientRow +
                                               static_cast<std::size_t>(number) * gradientStep +
                                               (state & gradientNoiseMask)) &
                                              byteMask);
        }
        bytes[index] = value;
    }
    return buffer.endCpuAccess();
}

struct Stream {
    bool ok{false};
    std::vector<std::size_t> sizes;
    std::vector<bool> keys;
    bool firstHasHeaders{false};
    // Whether every key frame holds the VPS, so that a clip can start on any of them.
    bool everyKeyHasHeaders{true};
    bool firstStartsWithStartCode{false};
    std::size_t total{0};
};

// Encodes `streamFrames` frames of a new image each (noise differs from frame to frame).
Stream encodeStream(const VideoConfig& config, Content content, const Logger& log) {
    Stream stream;
    std::optional<VideoEncoder> encoder{VideoEncoder::create(config, log)};
    std::optional<DmaBuf> buffer{DmaBuf::allocate(DmaBufConfig{defaultHeap, nv12Bytes}, log)};
    if (!encoder || !buffer) {
        return stream;
    }
    const std::optional<MppSource> source{MppSource::import(*buffer, log)};
    if (!source) {
        return stream;
    }
    std::uint64_t state{noiseSeed};
    for (int number{0}; number < streamFrames; ++number) {
        if (!fill(*buffer, content, number, state)) {
            return stream;
        }
        const std::optional<VideoPacket> packet{encoder->encode(*source)};
        if (!packet) {
            return stream;
        }
        if (number == 0) {
            stream.firstHasHeaders = hasNalType(packet->data, packet->size, NalType::Vps);
            stream.firstStartsWithStartCode =
                packet->size >= startCode.size() &&
                std::equal(startCode.begin(), startCode.end(), packet->data);
        }
        if (packet->keyFrame && !hasNalType(packet->data, packet->size, NalType::Vps)) {
            stream.everyKeyHasHeaders = false;
        }
        stream.sizes.push_back(packet->size);
        stream.keys.push_back(packet->keyFrame);
        stream.total += packet->size;
    }
    stream.ok = true;
    return stream;
}

void checkFlatStream(const Stream& flat, Check& check) {
    check.expect(flat.ok, "a flat stream is encoded");
    if (!flat.ok) {
        return;
    }
    check.expect(flat.firstHasHeaders, "the first packet holds the VPS");
    check.expect(flat.firstStartsWithStartCode, "the first packet starts with a start code");
    check.expect(flat.everyKeyHasHeaders, "every key frame carries the VPS, not only the first");
    bool keysAtGop{true};
    bool pFramesSmall{true};
    for (std::size_t number{0}; number < flat.keys.size(); ++number) {
        const bool expectedKey{number % gopFrames == 0};
        keysAtGop = keysAtGop && flat.keys.at(number) == expectedKey;
        pFramesSmall = pFramesSmall && (expectedKey || flat.sizes.at(number) < flatPFrameMaxBytes);
    }
    check.expect(keysAtGop, "a key frame every gopFrames frames and only then");
    check.expect(pFramesSmall, "the P frames of a flat image are small");
}

void checkRate(const Stream& flat, const Stream& noisy, const Stream& pattern, Check& check) {
    check.expect(noisy.ok && pattern.ok, "the noise and the gradient streams are encoded");
    if (!noisy.ok || !pattern.ok || !flat.ok) {
        return;
    }
    check.expect(noisy.total > noiseOverFlat * flat.total,
                 "a noisy stream is much larger than a flat one");
    const double seconds{static_cast<double>(streamFrames) / streamFps};
    const double kbps{static_cast<double>(pattern.total * bitsPerByte) / seconds / bitsPerKilobit};
    check.expect(kbps < lowKbps * rateTolerance && kbps > lowKbps / rateTolerance,
                 "the gradient stream is near the target bitrate");
}

struct BadCase {
    VideoConfig config;
    const char* what{nullptr};
};

std::vector<BadCase> badCases() {
    std::vector<BadCase> cases;
    VideoConfig config{baseConfig()};
    config.width = 0;
    cases.push_back({config, "width 0"});
    config = baseConfig();
    config.height = imageHeight + 1;
    cases.push_back({config, "odd height"});
    config = baseConfig();
    config.horStride = imageWidth - 2;
    cases.push_back({config, "stride below the width"});
    config = baseConfig();
    config.verStride = imageHeight - 2;
    cases.push_back({config, "lines below the height"});
    config = baseConfig();
    config.kbps = 0;
    cases.push_back({config, "bitrate 0"});
    config = baseConfig();
    config.kbps = maxVideoKbps + 1;
    cases.push_back({config, "bitrate above the limit"});
    config = baseConfig();
    config.framesPerSecond = 0;
    cases.push_back({config, "frame rate 0"});
    config = baseConfig();
    config.gopFrames = 0;
    cases.push_back({config, "gop 0"});
    config = baseConfig();
    config.capacity = 0;
    cases.push_back({config, "capacity 0"});
    return cases;
}

void checkRefusals(const Logger& log, Check& check) {
    for (const BadCase& item : badCases()) {
        check.expect(!VideoEncoder::create(item.config, log).has_value(), item.what);
    }
    // A packet larger than the output buffer is an error, not a cut stream.
    VideoConfig tiny{baseConfig()};
    tiny.capacity = tinyCapacity;
    check.expect(!encodeStream(tiny, Content::Flat, log).ok, "a packet above the capacity fails");
}
} // namespace

int main() {
    const Logger log{"TEST_VIDEO", LogLevel::Debug};
    Check check{log};
    const Stream flat{encodeStream(baseConfig(), Content::Flat, log)};
    const Stream noisy{encodeStream(baseConfig(), Content::Noise, log)};
    const Stream pattern{encodeStream(baseConfig(), Content::Pattern, log)};
    log.info("flat ", flat.total, " bytes, noise ", noisy.total, " bytes, pattern ", pattern.total,
             " bytes in ", streamFrames, " frames");
    checkFlatStream(flat, check);
    checkRate(flat, noisy, pattern, check);
    checkRefusals(log, check);
    if (check.ok()) {
        log.info("PASS");
        return EXIT_SUCCESS;
    }
    log.error("FAIL");
    return EXIT_FAILURE;
}
