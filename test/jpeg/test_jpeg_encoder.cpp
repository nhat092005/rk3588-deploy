// JpegEncoder on the board: the JPEG is valid (markers, size in the header), the encoder reads the
// buffer (flat and noisy images give very different sizes), the quality changes the size, and a bad
// configuration is refused. Needs /dev/dma_heap/system and the MPP JPEG encoder. Exit code 0 when
// every check passes.
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>

#include "dmabuf.h"
#include "jpeg_encoder.h"
#include "logger.h"
#include "mpp_source.h"

namespace {
constexpr int imageWidth{1280};
constexpr int imageHeight{720};
constexpr std::size_t nv12Bytes{static_cast<std::size_t>(imageWidth) * imageHeight * 3 / 2};
constexpr std::uint8_t flatLevel{128};
constexpr std::size_t flatMaxBytes{20'000};
constexpr std::size_t noiseMinBytes{200'000};
constexpr int lowQuality{60};
constexpr int highQuality{95};
constexpr std::uint64_t noiseSeed{88172645463325252ULL};
constexpr unsigned shiftLeftFirst{13U};
constexpr unsigned shiftRight{7U};
constexpr unsigned shiftLeftSecond{17U};
constexpr std::uint8_t markerPrefix{0xFF};
constexpr std::uint8_t startMarker{0xD8};
constexpr std::uint8_t endMarker{0xD9};
constexpr std::uint8_t firstFrameMarker{0xC0};
constexpr std::uint8_t lastFrameMarker{0xC2};
constexpr std::size_t markerBytes{2};
constexpr std::size_t byteBits{8};
constexpr unsigned byteMask{0xFFU};
// The frame header after the marker: length (2), precision (1), height (2), width (2).
constexpr std::size_t frameHeaderBytes{9};
constexpr std::size_t heightOffset{5};
constexpr std::size_t widthOffset{7};

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

// NV12 content: every byte the same, or xorshift noise.
bool fill(DmaBuf& buffer, bool noise) {
    auto* bytes{static_cast<std::uint8_t*>(buffer.data())};
    std::uint64_t state{noiseSeed};
    for (std::size_t index{0}; index < nv12Bytes; ++index) {
        state ^= state << shiftLeftFirst;
        state ^= state >> shiftRight;
        state ^= state << shiftLeftSecond;
        bytes[index] = noise ? static_cast<std::uint8_t>(state & byteMask) : flatLevel;
    }
    return buffer.endCpuAccess();
}

// Width and height of the first frame header (SOF0 to SOF2) of a JPEG, or empty.
struct JpegSize {
    int width{0};
    int height{0};
};

std::optional<JpegSize> headerSize(const std::uint8_t* data, std::size_t length) {
    std::size_t position{markerBytes}; // after the start marker
    while (position + 4 < length && data[position] == markerPrefix) {
        const std::uint8_t marker{data[position + 1]};
        const std::size_t segment{(static_cast<std::size_t>(data[position + 2]) << byteBits) |
                                  data[position + 3]};
        if (marker >= firstFrameMarker && marker <= lastFrameMarker &&
            position + frameHeaderBytes < length) {
            const auto read{[&](std::size_t offset) {
                return (static_cast<int>(data[position + offset]) << byteBits) |
                       data[position + offset + 1];
            }};
            return JpegSize{read(widthOffset), read(heightOffset)};
        }
        position += markerBytes + segment;
    }
    return std::nullopt;
}

bool validJpeg(const JpegEncoder& encoder, std::size_t length) {
    const std::uint8_t* data{encoder.data()};
    return length > markerBytes && data[0] == markerPrefix && data[1] == startMarker &&
           data[length - 2] == markerPrefix && data[length - 1] == endMarker;
}

// What one encode gave: the size of the JPEG, whether it has its markers, and whether its header
// holds the image size.
struct EncodeResult {
    std::optional<std::size_t> bytes;
    bool valid{false};
    bool sizeMatches{false};
};

EncodeResult encodeOnce(DmaBuf& buffer, int quality, const Logger& log) {
    EncodeResult result;
    std::optional<JpegEncoder> encoder{JpegEncoder::create(
        JpegConfig{imageWidth, imageHeight, imageWidth, imageHeight, true, quality}, log)};
    const std::optional<MppSource> source{MppSource::import(buffer, log)};
    if (!encoder || !source) {
        return result;
    }
    result.bytes = encoder->encode(*source);
    if (!result.bytes) {
        return result;
    }
    result.valid = validJpeg(*encoder, *result.bytes);
    const std::optional<JpegSize> size{headerSize(encoder->data(), *result.bytes)};
    result.sizeMatches = size && size->width == imageWidth && size->height == imageHeight;
    return result;
}

void checkContent(Check& check, const Logger& log) {
    std::optional<DmaBuf> flat{DmaBuf::allocate(DmaBufConfig{defaultHeap, nv12Bytes}, log)};
    std::optional<DmaBuf> noisy{DmaBuf::allocate(DmaBufConfig{defaultHeap, nv12Bytes}, log)};
    if (!flat || !noisy || !fill(*flat, false) || !fill(*noisy, true)) {
        check.expect(false, "buffers prepared");
        return;
    }
    const EncodeResult flatResult{encodeOnce(*flat, defaultJpegQuality, log)};
    const EncodeResult noisyResult{encodeOnce(*noisy, defaultJpegQuality, log)};
    check.expect(flatResult.bytes && noisyResult.bytes, "both images encode");
    check.expect(flatResult.valid && noisyResult.valid, "markers: start and end");
    check.expect(flatResult.sizeMatches && noisyResult.sizeMatches,
                 "the header holds the image size");
    check.expect(flatResult.bytes && *flatResult.bytes < flatMaxBytes, "a flat image is small");
    check.expect(noisyResult.bytes && *noisyResult.bytes > noiseMinBytes, "a noisy image is large");
    const EncodeResult low{encodeOnce(*noisy, lowQuality, log)};
    const EncodeResult high{encodeOnce(*noisy, highQuality, log)};
    check.expect(low.bytes && high.bytes && *low.bytes < *high.bytes, "a higher quality is larger");
}

void checkRepeat(Check& check, const Logger& log) {
    std::optional<DmaBuf> buffer{DmaBuf::allocate(DmaBufConfig{defaultHeap, nv12Bytes}, log)};
    if (!buffer || !fill(*buffer, true)) {
        check.expect(false, "buffer prepared");
        return;
    }
    std::optional<JpegEncoder> encoder{JpegEncoder::create(
        JpegConfig{imageWidth, imageHeight, imageWidth, imageHeight, true, defaultJpegQuality},
        log)};
    const std::optional<MppSource> source{MppSource::import(*buffer, log)};
    if (!encoder || !source) {
        check.expect(false, "encoder created");
        return;
    }
    const std::optional<std::size_t> first{encoder->encode(*source)};
    const std::optional<std::size_t> second{encoder->encode(*source)};
    check.expect(first && second && *first == *second, "the same image gives the same size");
}

void checkConfig(Check& check, const Logger& log) {
    const auto create{
        [&log](const JpegConfig& config) { return JpegEncoder::create(config, log).has_value(); }};
    const JpegConfig good{imageWidth,  imageHeight, imageWidth,
                          imageHeight, true,        defaultJpegQuality};
    check.expect(create(good), "a good configuration is accepted");
    JpegConfig bad{good};
    bad.fullRange = false;
    check.expect(!create(bad), "limited range refused");
    bad = good;
    bad.width = imageWidth + 1;
    check.expect(!create(bad), "odd width refused");
    bad = good;
    bad.horStride = imageWidth - 2;
    check.expect(!create(bad), "stride below the width refused");
    bad = good;
    bad.quality = 0;
    check.expect(!create(bad), "quality 0 refused");
    bad = good;
    bad.quality = maxJpegQuality + 1;
    check.expect(!create(bad), "quality above the maximum refused");
    bad = good;
    bad.capacity = 0;
    check.expect(!create(bad), "no output capacity refused");
}
} // namespace

int main() {
    const Logger log{"TEST_JPEG", LogLevel::Debug};
    Check check{log};
    checkConfig(check, log);
    checkContent(check, log);
    checkRepeat(check, log);
    if (check.ok()) {
        log.info("PASS");
        return EXIT_SUCCESS;
    }
    log.error("FAIL");
    return EXIT_FAILURE;
}
