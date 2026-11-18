// ClipRecorder: the clip starts on the key frame at least preSeconds before the event and ends
// postSeconds after it, the ring wraps without damaging the bytes, clips are independent, buffers
// are reused after release(), and bad cases are counted. Packets are made-up bytes that name their
// number. Plain C++, runs on the PC. Exit code 0 when every check passes.
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <vector>

#include "clip_recorder.h"
#include "logger.h"

namespace {
constexpr int streamFps{10};
constexpr double frameStep{0.1};
constexpr int packetsPerGop{10};
constexpr std::size_t packetBytes{100};
constexpr std::size_t streamRate{packetBytes * streamFps}; // bytes per second
constexpr double preTime{3.0};
constexpr double postTime{2.0};
constexpr int eventPacket{70};     // seconds 7.0
constexpr int firstClipPacket{40}; // the key frame at 4.0 is the newest one at or before 7.0 - 3.0
constexpr int lastClipPacket{90};  // 9.0 is the first packet 2.0 after the event
constexpr std::size_t clipsAtOnce{2};
constexpr std::int64_t eventA{101};
constexpr std::int64_t eventB{202};
constexpr std::int64_t eventC{303};
constexpr std::size_t straddlingPacket{97}; // does not divide the ring of 7500 bytes
constexpr std::size_t tinyPacket{10};
constexpr int wrapEvent{390};
constexpr int wrapFirst{360}; // key frame at 36.0, the newest one at or before 39.0 - 3.0
constexpr int wrapLast{410};
constexpr int earlyEvent{5};
constexpr int overlapB{80};
constexpr int overlapBLast{100};
constexpr std::size_t hugePacket{1'000'000};
constexpr std::size_t burstPacket{2500};
constexpr int burstPackets{30};
constexpr double nearZero{1e-9};
constexpr int postPackets{20};                     // postTime / frameStep
constexpr std::size_t burstLimit{streamRate * 10}; // a clip of 10 s of the expected rate

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

ClipConfig baseConfig() {
    ClipConfig config;
    config.bytesPerSecond = streamRate;
    config.preSeconds = preTime;
    config.postSeconds = postTime;
    config.gopSeconds = 1.0;
    config.framesPerSecond = streamFps;
    config.clipCount = clipsAtOnce;
    return config;
}

ClipRecorder makeRecorder(const Logger& log, const ClipConfig& config) {
    std::optional<ClipRecorder> recorder{ClipRecorder::create(config, log)};
    if (!recorder) {
        std::abort();
    }
    return std::move(*recorder);
}

double secondsOf(int number) {
    return static_cast<double>(number) * frameStep;
}

// Adds packet `number`: `packetBytes` bytes that all equal number mod 256, a key frame every
// packetsPerGop packets.
void feed(ClipRecorder& recorder, int number, std::size_t size = packetBytes) {
    const std::vector<std::uint8_t> bytes(size, static_cast<std::uint8_t>(number));
    recorder.addPacket(bytes.data(), size, number % packetsPerGop == 0, secondsOf(number));
}

void feedRange(ClipRecorder& recorder, int first, int last, std::size_t size = packetBytes) {
    for (int number{first}; number <= last; ++number) {
        feed(recorder, number, size);
    }
}

// Whether the clip holds packets `first` to `last` in order, whole.
bool holdsPackets(const ClipRef& clip, int first, int last, std::size_t size = packetBytes) {
    const auto count{static_cast<std::size_t>(last - first + 1)};
    if (clip.size != count * size) {
        return false;
    }
    for (std::size_t at{0}; at < clip.size; ++at) {
        const int number{first + static_cast<int>(at / size)};
        if (clip.data[at] != static_cast<std::uint8_t>(number)) {
            return false;
        }
    }
    return true;
}

void checkClipRange(Check& check, const Logger& log) {
    ClipRecorder recorder{makeRecorder(log, baseConfig())};
    feedRange(recorder, 0, eventPacket);
    check.expect(recorder.start(eventA), "a clip starts");
    check.expect(!recorder.finished().has_value(), "the clip is not finished before the post time");
    feedRange(recorder, eventPacket + 1, lastClipPacket - 1);
    check.expect(!recorder.finished().has_value(), "still recording one packet before the end");
    feed(recorder, lastClipPacket);
    const std::optional<ClipRef> clip{recorder.finished()};
    check.expect(clip.has_value(), "the clip is finished at the post time");
    if (!clip) {
        return;
    }
    check.expect(clip->eventId == eventA, "the event id comes back");
    check.expect(std::abs(clip->startSeconds - secondsOf(firstClipPacket)) < nearZero,
                 "starts on the newest key frame at least preSeconds before the event");
    check.expect(std::abs(clip->eventSeconds - secondsOf(eventPacket)) < nearZero,
                 "the event time is the time of the last packet at start()");
    check.expect(holdsPackets(*clip, firstClipPacket, lastClipPacket),
                 "the bytes are packets 40 to 90 in order");
    check.expect(!recorder.finished().has_value(), "a clip is delivered once");
    recorder.release(clip->slot);
}

void checkEarlyEvent(Check& check, const Logger& log) {
    ClipRecorder recorder{makeRecorder(log, baseConfig())};
    feedRange(recorder, 0, earlyEvent);
    check.expect(recorder.start(eventA), "an event before preSeconds of video still gets a clip");
    feedRange(recorder, earlyEvent + 1, earlyEvent + postPackets);
    const std::optional<ClipRef> clip{recorder.finished()};
    check.expect(clip.has_value() && holdsPackets(*clip, 0, earlyEvent + postPackets),
                 "it starts at the oldest key frame");
}

void checkNoPostTime(Check& check, const Logger& log) {
    ClipConfig config{baseConfig()};
    config.postSeconds = 0.0;
    ClipRecorder recorder{makeRecorder(log, config)};
    feedRange(recorder, 0, eventPacket);
    check.expect(recorder.start(eventA), "a clip without post time starts");
    const std::optional<ClipRef> clip{recorder.finished()};
    check.expect(clip.has_value() && holdsPackets(*clip, firstClipPacket, eventPacket),
                 "it is finished at once and ends with the event packet");
}

// The ring wraps many times. With a packet size that does not divide the ring, packets straddle
// its end; with tiny packets the packet index is full before the bytes are.
void checkWrapWith(Check& check, const Logger& log, std::size_t size, const char* what) {
    ClipRecorder recorder{makeRecorder(log, baseConfig())};
    feedRange(recorder, 0, wrapEvent, size);
    check.expect(recorder.start(eventA), what);
    feedRange(recorder, wrapEvent + 1, wrapLast, size);
    const std::optional<ClipRef> clip{recorder.finished()};
    check.expect(clip.has_value() && holdsPackets(*clip, wrapFirst, wrapLast, size), what);
    check.expect(recorder.oversizedCount() == 0, "no packet was too large");
}

void checkWrap(Check& check, const Logger& log) {
    checkWrapWith(check, log, packetBytes, "the bytes survive the ring wrapping");
    checkWrapWith(check, log, straddlingPacket, "packets that straddle the end of the ring");
    checkWrapWith(check, log, tinyPacket, "the packet index fills before the bytes");
}

void checkIndependentClips(Check& check, const Logger& log) {
    ClipRecorder recorder{makeRecorder(log, baseConfig())};
    feedRange(recorder, 0, eventPacket);
    check.expect(recorder.start(eventA), "first clip");
    feedRange(recorder, eventPacket + 1, overlapB);
    check.expect(recorder.start(eventB), "second clip while the first is recording");
    feedRange(recorder, overlapB + 1, overlapBLast);
    const std::optional<ClipRef> first{recorder.finished()};
    const std::optional<ClipRef> second{recorder.finished()};
    check.expect(first && first->eventId == eventA &&
                     holdsPackets(*first, firstClipPacket, lastClipPacket),
                 "the first clip is whole");
    check.expect(second && second->eventId == eventB &&
                     holdsPackets(*second, firstClipPacket + packetsPerGop, overlapBLast),
                 "the second clip has its own range");
}

void checkFinishAll(Check& check, const Logger& log) {
    ClipRecorder recorder{makeRecorder(log, baseConfig())};
    feedRange(recorder, 0, eventPacket);
    check.expect(recorder.start(eventA), "a clip starts");
    feedRange(recorder, eventPacket + 1, eventPacket + 3);
    check.expect(!recorder.finished().has_value(), "it is still recording");
    recorder.finishAll();
    const std::optional<ClipRef> clip{recorder.finished()};
    check.expect(clip.has_value() && holdsPackets(*clip, firstClipPacket, eventPacket + 3),
                 "finishAll ends it with the packets so far");
    recorder.finishAll();
    check.expect(!recorder.finished().has_value(), "finishAll with nothing recording does nothing");
}

void checkPool(Check& check, const Logger& log) {
    ClipRecorder recorder{makeRecorder(log, baseConfig())};
    feedRange(recorder, 0, eventPacket);
    check.expect(recorder.start(eventA) && recorder.start(eventB), "both buffers in use");
    check.expect(!recorder.start(eventC), "no buffer left");
    check.expect(recorder.skippedCount() == 1, "the refused clip is counted");
    feedRange(recorder, eventPacket + 1, lastClipPacket);
    const std::optional<ClipRef> clip{recorder.finished()};
    check.expect(clip.has_value(), "a clip is finished");
    check.expect(!recorder.start(eventC), "a finished clip still holds its buffer");
    if (clip) {
        recorder.release(clip->slot);
    }
    check.expect(recorder.start(eventC), "a released buffer is reused");
    check.expect(recorder.skippedCount() == 2, "two refusals in total");
}

void checkNoKeyFrame(Check& check, const Logger& log) {
    ClipRecorder recorder{makeRecorder(log, baseConfig())};
    const std::vector<std::uint8_t> bytes(packetBytes, 1);
    recorder.addPacket(bytes.data(), bytes.size(), false, 0.0);
    check.expect(!recorder.start(eventA), "a ring without a key frame gives no clip");
    check.expect(recorder.skippedCount() == 1, "and it is counted");
}

void checkOversizeAndFull(Check& check, const Logger& log) {
    ClipRecorder recorder{makeRecorder(log, baseConfig())};
    feedRange(recorder, 0, eventPacket);
    feed(recorder, eventPacket + 1, hugePacket);
    check.expect(recorder.oversizedCount() == 1, "a packet larger than the ring is counted");
    // A burst of large packets fills the clip buffer before the post time: the clip ends early.
    check.expect(recorder.start(eventA), "a clip starts");
    for (int number{0}; number < burstPackets; ++number) {
        const std::vector<std::uint8_t> bytes(burstPacket, 1);
        recorder.addPacket(bytes.data(), bytes.size(), false,
                           secondsOf(eventPacket + 2) + (frameStep * number / burstPackets));
    }
    const std::optional<ClipRef> clip{recorder.finished()};
    check.expect(clip.has_value() && clip->size <= burstLimit,
                 "a full clip buffer finishes the clip and never overflows");
}

void checkConfig(Check& check, const Logger& log) {
    ClipConfig config{baseConfig()};
    config.bytesPerSecond = 0;
    check.expect(!ClipRecorder::create(config, log).has_value(), "no rate");
    config = baseConfig();
    config.preSeconds = -1.0;
    check.expect(!ClipRecorder::create(config, log).has_value(), "negative pre");
    config = baseConfig();
    config.postSeconds = std::numeric_limits<double>::quiet_NaN();
    check.expect(!ClipRecorder::create(config, log).has_value(), "nan post");
    config = baseConfig();
    config.gopSeconds = 0.0;
    check.expect(!ClipRecorder::create(config, log).has_value(), "gop 0");
    config = baseConfig();
    config.framesPerSecond = 0;
    check.expect(!ClipRecorder::create(config, log).has_value(), "fps 0");
    config = baseConfig();
    config.clipCount = 0;
    check.expect(!ClipRecorder::create(config, log).has_value(), "no clips");
}
} // namespace

int main() {
    const Logger log{"TEST_CLIP", LogLevel::None};
    const Logger report{"TEST_CLIP", LogLevel::Debug};
    Check check{report};
    checkClipRange(check, log);
    checkEarlyEvent(check, log);
    checkNoPostTime(check, log);
    checkWrap(check, log);
    checkIndependentClips(check, log);
    checkFinishAll(check, log);
    checkPool(check, log);
    checkNoKeyFrame(check, log);
    checkOversizeAndFull(check, log);
    checkConfig(check, log);
    if (check.ok()) {
        report.info("PASS");
        return EXIT_SUCCESS;
    }
    report.error("FAIL");
    return EXIT_FAILURE;
}
