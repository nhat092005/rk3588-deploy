// SegmentWriter: the segments start on key frames and end at the first key frame after their
// length, a stream with a gap starts a new one, the names come from the wall clock (and get a
// suffix when they repeat), a file has its final name only when it is whole, and the failures are
// reported without leaving files behind. Uses a temporary directory and fixed times in UTC. Exit
// code 0 when every check passes.
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include <csignal>

#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include "logger.h"
#include "segment_writer.h"
#include "video_file.h"

namespace {
namespace fs = std::filesystem;

constexpr int year{2026};
constexpr int yearBase{1900};
constexpr int november{10}; // tm_mon
constexpr int today{18};
constexpr double segmentLength{10.0};
// Values and times of the made-up packets.
constexpr std::uint8_t packetA{1};
constexpr std::uint8_t packetB{2};
constexpr std::uint8_t packetC{3};
constexpr std::uint8_t packetD{4};
constexpr std::uint8_t packetE{5};
constexpr std::uint8_t packetF{6};
constexpr std::uint8_t packetG{7};
constexpr double timeSecondFrame{0.1};
constexpr double timeThird{0.2};
constexpr double timeFourth{0.3};
constexpr double timeGap{0.5};
constexpr double timeOneSecond{1.0};
constexpr double timeBeforeEnd{9.9};
constexpr double timeEnds{10.1};
constexpr double timeAfterEnd{11.0};
constexpr double timeRefused{20.0};
constexpr double timeRefusedNext{20.1};
constexpr double timeRestart{21.0};
constexpr std::size_t packetSize{50};
constexpr unsigned modeMask{07777};
constexpr int hourOfFirst{9};
constexpr int minuteOfFirst{30};
constexpr int secondsOfSecond{10};

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

// 2026-11-18 at hour:minute:second UTC.
struct Hms {
    int hour;
    int minute;
    int second;
};

std::time_t at(const Hms& clock) {
    std::tm parts{};
    parts.tm_year = year - yearBase;
    parts.tm_mon = november;
    parts.tm_mday = today;
    parts.tm_hour = clock.hour;
    parts.tm_min = clock.minute;
    parts.tm_sec = clock.second;
    return timegm(&parts);
}

std::optional<fs::path> makeRoot() {
    std::error_code error;
    std::string pattern{(fs::temp_directory_path(error) / "segment_writer_XXXXXX").string()};
    if (error || mkdtemp(pattern.data()) == nullptr) {
        return std::nullopt;
    }
    return fs::path{pattern};
}

template <typename Body> void withRoot(Check& check, const Logger& log, Body body) {
    const std::optional<fs::path> root{makeRoot()};
    if (!root) {
        check.expect(false, "temporary directory made");
        return;
    }
    body(check, *root, log);
    std::error_code error;
    fs::permissions(*root, fs::perms::owner_all, error);
    fs::remove_all(*root, error);
}

// A packet of packetSize bytes that all equal `value`.
QueuedPacket packet(std::uint8_t value, bool key, double seconds, bool gap = false) {
    QueuedPacket result;
    result.bytes.assign(packetSize, value);
    result.keyFrame = key;
    result.seconds = seconds;
    result.afterGap = gap;
    return result;
}

SegmentWriter makeWriter(const fs::path& root, const Logger& log, unsigned mode = defaultFileMode) {
    const std::string text{root.string()};
    std::optional<SegmentWriter> writer{
        SegmentWriter::create(SegmentConfig{text, segmentLength, mode}, log)};
    if (!writer) {
        std::abort();
    }
    return std::move(*writer);
}

std::string readText(const fs::path& path) {
    std::ifstream file{path, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

std::size_t hiddenFiles(const fs::path& directory) {
    std::size_t count{0};
    std::error_code error;
    for (const fs::directory_entry& entry : fs::directory_iterator{directory, error}) {
        count += entry.path().filename().string().front() == '.' ? 1 : 0;
    }
    return count;
}

// A format that writes two characters for a packet (its value and k or d) and E for the end.
class MarkWriter final : public VideoFileWriter {
public:
    MarkWriter(int descriptor, const Logger& logger) : file{descriptor}, log{logger} {}

    bool write(const std::uint8_t* data, std::size_t /*size*/, bool keyFrame,
               double /*seconds*/) override {
        return put(std::string{static_cast<char>('0' + data[0]), keyFrame ? 'k' : 'd'});
    }
    bool flush() override {
        return true;
    }
    bool finish() override {
        return put("E");
    }

private:
    bool put(const std::string& text) {
        return writeBytes(file, reinterpret_cast<const std::uint8_t*>(text.data()), text.size(),
                          log);
    }

    int file;
    Logger log;
};

std::unique_ptr<VideoFileWriter> makeMarkWriter(int descriptor, const Logger& log) {
    return std::make_unique<MarkWriter>(descriptor, log);
}

void checkFormat(Check& check, const fs::path& root, const Logger& log) {
    const std::string text{root.string()};
    std::optional<SegmentWriter> writer{SegmentWriter::create(
        SegmentConfig{text, segmentLength, defaultFileMode, VideoOutput{".mp4", &makeMarkWriter}},
        log)};
    check.expect(writer.has_value(), "a writer of another format is created");
    if (!writer) {
        return;
    }
    const fs::path day{root / "2026-11-18"};
    const std::time_t start{at(Hms{hourOfFirst, minuteOfFirst, 0})};
    check.expect(writer->consume(packet(packetB, true, timeSecondFrame), start, true), "key frame");
    check.expect(fs::exists(day / ".093000.mp4.tmp") && !fs::exists(day / "093000.mp4"),
                 "the file has a hidden name with the suffix of the format while it is open");
    check.expect(writer->consume(packet(packetC, false, timeOneSecond), start, true), "a frame");
    writer->finish();
    check.expect(readText(day / "093000.mp4") == "2k3dE",
                 "the writer got the packets and its end came before the file was completed");
    check.expect(hiddenFiles(day) == 0 && !fs::exists(day / "093000.h265"),
                 "no temporary file and no .h265 file");
    const std::vector<ClosedSegment> closed{writer->takeClosed()};
    check.expect(closed.size() == 1 && closed.front().path == (day / "093000.mp4").string(),
                 "the closed segment has the name of the format");
    check.expect(
        !SegmentWriter::create(
            SegmentConfig{text, segmentLength, defaultFileMode, VideoOutput{".mp4", nullptr}}, log),
        "a format without a writer is refused");
}

void checkSegments(Check& check, const fs::path& root, const Logger& log) {
    SegmentWriter writer{makeWriter(root, log)};
    const fs::path day{root / "2026-11-18"};
    const std::time_t start{at(Hms{hourOfFirst, minuteOfFirst, 0})};
    check.expect(writer.consume(packet(packetA, false, 0.0), start, true) &&
                     writer.skippedPackets() == 1,
                 "a frame before any key frame is skipped");
    check.expect(!writer.startsSegment(packet(packetA, false, 0.0)),
                 "an ordinary frame never starts a segment");
    check.expect(writer.startsSegment(packet(packetB, true, timeSecondFrame)),
                 "a key frame starts the first segment");
    check.expect(writer.consume(packet(packetB, true, timeSecondFrame), start, true),
                 "the first key frame");
    check.expect(fs::exists(day / ".093000.h265.tmp") && !fs::exists(day / "093000.h265"),
                 "the file has a hidden name while it is open");
    check.expect(writer.consume(packet(packetC, false, timeOneSecond), start, true),
                 "an ordinary frame");
    check.expect(!writer.startsSegment(packet(packetD, true, timeBeforeEnd)),
                 "a key frame before the length goes on");
    check.expect(writer.consume(packet(packetD, true, timeBeforeEnd), start, true),
                 "that key frame");
    check.expect(!writer.startsSegment(packet(packetE, false, timeAfterEnd)),
                 "an ordinary frame after the length goes on");
    check.expect(writer.consume(packet(packetE, false, timeAfterEnd), start, true), "that frame");
    check.expect(writer.takeClosed().empty(), "nothing is closed yet");
    // The key frame at 10.1 s ends the segment (it started at 0.1 s): 10.0 s long.
    const std::time_t later{at(Hms{hourOfFirst, minuteOfFirst, secondsOfSecond})};
    check.expect(writer.startsSegment(packet(packetF, true, timeEnds)),
                 "a key frame after the length ends it");
    check.expect(writer.consume(packet(packetF, true, timeEnds), later, true),
                 "and starts the next");
    const std::vector<ClosedSegment> closed{writer.takeClosed()};
    check.expect(closed.size() == 1 && closed.front().path == (day / "093000.h265").string() &&
                     closed.front().bytes == 4 * packetSize,
                 "the first segment is closed with its bytes");
    check.expect(readText(day / "093000.h265") ==
                     std::string(packetSize, '\x02') + std::string(packetSize, '\x03') +
                         std::string(packetSize, '\x04') + std::string(packetSize, '\x05'),
                 "it holds the packets in order");
    check.expect(fs::exists(day / ".093010.h265.tmp"),
                 "the next segment is open under a hidden name");
    check.expect(writer.consume(packet(packetG, false, timeAfterEnd), later, true),
                 "another frame");
    writer.finish();
    check.expect(readText(day / "093010.h265") ==
                     std::string(packetSize, '\x06') + std::string(packetSize, '\x07'),
                 "finish() completes the segment that was open");
    check.expect(writer.segmentCount() == 2 && hiddenFiles(day) == 0,
                 "two segments, no hidden file left");
    check.expect((fs::status(day / "093000.h265").permissions() & fs::perms::others_write) ==
                     fs::perms::none,
                 "the file has the configured mode");
}

void checkGapAndRefusal(Check& check, const fs::path& root, const Logger& log) {
    SegmentWriter writer{makeWriter(root, log)};
    const fs::path day{root / "2026-11-18"};
    const std::time_t start{at(Hms{hourOfFirst, minuteOfFirst, 0})};
    check.expect(writer.consume(packet(packetA, true, 0.0), start, true), "first");
    check.expect(writer.consume(packet(packetB, false, timeSecondFrame), start, true), "second");
    // A gap ends the segment although it is far from its length.
    const std::time_t soon{at(Hms{hourOfFirst, minuteOfFirst, 1})};
    check.expect(writer.startsSegment(packet(packetC, true, timeGap, true)),
                 "a key frame after a gap starts a segment");
    check.expect(writer.consume(packet(packetC, true, timeGap, true), soon, true), "and does");
    check.expect(writer.takeClosed().size() == 1, "the segment before the gap is closed");
    // No room: the open segment is completed, the frames are skipped until a key frame may start.
    check.expect(writer.consume(packet(packetD, true, timeRefused), soon, false),
                 "a refused start is not an error");
    check.expect(writer.takeClosed().size() == 1, "the open segment was completed");
    check.expect(writer.consume(packet(packetE, false, timeRefusedNext), soon, true),
                 "a frame with nothing open is skipped");
    check.expect(writer.skippedPackets() >= 2, "the skipped frames are counted");
    check.expect(!fs::exists(day / ".093001.h265.tmp"), "no file is open");
    check.expect(writer.consume(packet(packetF, true, timeRestart), soon, true),
                 "a key frame with room starts again");
    writer.finish();
    check.expect(writer.takeClosed().size() == 1, "and ends");
}

void checkNames(Check& check, const fs::path& root, const Logger& log) {
    SegmentWriter writer{makeWriter(root, log)};
    const fs::path day{root / "2026-11-18"};
    const std::time_t second{at(Hms{hourOfFirst, minuteOfFirst, 0})};
    check.expect(writer.consume(packet(packetA, true, 0.0), second, true), "first segment");
    check.expect(writer.consume(packet(packetB, true, 0.0, true), second, true),
                 "a second segment in the same second");
    check.expect(writer.consume(packet(packetC, true, 0.0, true), second, true), "a third");
    writer.finish();
    check.expect(fs::exists(day / "093000.h265") && fs::exists(day / "093000_2.h265") &&
                     fs::exists(day / "093000_3.h265"),
                 "the names repeat with _2 and _3");
}

void checkMode(Check& check, const fs::path& root, const Logger& log) {
    constexpr unsigned wideMode{0666};
    SegmentWriter writer{makeWriter(root, log, wideMode)};
    check.expect(writer.consume(packet(packetA, true, 0.0), at(Hms{hourOfFirst, 0, 0}), true),
                 "a segment");
    writer.finish();
    struct stat info {};
    const std::string path{(root / "2026-11-18" / "090000.h265").string()};
    check.expect(::stat(path.c_str(), &info) == 0 && (info.st_mode & modeMask) == wideMode,
                 "the mode is applied whatever the umask");
}

// A write that fails half way (the file size limit): the file is removed and the stream skipped.
void checkWriteFailure(Check& check, const fs::path& root, const Logger& log) {
    constexpr rlim_t sizeLimit{2 * packetSize + (packetSize / 2)};
    SegmentWriter writer{makeWriter(root, log)};
    const std::time_t start{at(Hms{hourOfFirst, minuteOfFirst, 0})};
    (void)std::signal(SIGXFSZ, SIG_IGN);
    rlimit saved{};
    (void)getrlimit(RLIMIT_FSIZE, &saved);
    const rlimit small{sizeLimit, saved.rlim_max};
    (void)setrlimit(RLIMIT_FSIZE, &small);
    const bool first{writer.consume(packet(packetA, true, 0.0), start, true)};
    const bool second{writer.consume(packet(packetB, false, timeSecondFrame), start, true)};
    const bool third{writer.consume(packet(packetC, false, timeThird), start, true)};
    (void)setrlimit(RLIMIT_FSIZE, &saved);
    check.expect(first && second && !third, "the write that goes over the limit fails");
    check.expect(hiddenFiles(root / "2026-11-18") == 0, "the half written file is removed");
    check.expect(writer.consume(packet(packetD, false, timeFourth), start, true) &&
                     writer.skippedPackets() == 1,
                 "frames after the failure are skipped");
    check.expect(writer.consume(packet(packetE, true, timeOneSecond), start, true),
                 "the next key frame starts again");
    writer.finish();
    check.expect(writer.segmentCount() == 1, "and the segment completes");
}

void checkFailures(Check& check, const fs::path& root, const Logger& log) {
    const std::string text{root.string()};
    check.expect(!SegmentWriter::create(SegmentConfig{text, 0.0, defaultFileMode}, log),
                 "a length of 0 is refused");
    const std::string missing{(root / "missing").string()};
    check.expect(
        !SegmentWriter::create(SegmentConfig{missing, segmentLength, defaultFileMode}, log),
        "a missing root is refused");
    std::error_code error;
    std::ofstream{root / "file"} << "x";
    // Executable, so that only the directory test refuses it.
    fs::permissions(root / "file", fs::perms::owner_all, error);
    const std::string notDirectory{(root / "file").string()};
    check.expect(
        !SegmentWriter::create(SegmentConfig{notDirectory, segmentLength, defaultFileMode}, log),
        "a file as the root is refused");
    // A root that is read-only is refused at creation. A root that became read-only: the open
    // fails, the error is reported, nothing is left, and the stream is skipped until the next key
    // frame.
    SegmentWriter writer{makeWriter(root, log)};
    fs::permissions(root, fs::perms::owner_read | fs::perms::owner_exec, error);
    if (::access(text.c_str(), W_OK) == 0) {
        return; // running as root: a read-only directory is still writable
    }
    check.expect(!SegmentWriter::create(SegmentConfig{text, segmentLength, defaultFileMode}, log),
                 "a root that cannot be written is refused");
    check.expect(!writer.consume(packet(packetA, true, 0.0), at(Hms{hourOfFirst, 0, 0}), true),
                 "a segment that cannot be opened is an error");
    check.expect(
        writer.consume(packet(packetB, false, timeSecondFrame), at(Hms{hourOfFirst, 0, 0}), true) &&
            writer.skippedPackets() == 1,
        "the frames after it are skipped");
    fs::permissions(root, fs::perms::owner_all, error);
    check.expect(!fs::exists(root / "2026-11-18"), "nothing was created");
}
} // namespace

int main() {
    const Logger log{"TEST_SEGMENT", LogLevel::None};
    const Logger report{"TEST_SEGMENT", LogLevel::Debug};
    setenv("TZ", "UTC", 1);
    tzset();
    Check check{report};
    withRoot(check, log, checkSegments);
    withRoot(check, log, checkFormat);
    withRoot(check, log, checkGapAndRefusal);
    withRoot(check, log, checkNames);
    withRoot(check, log, checkMode);
    withRoot(check, log, checkWriteFailure);
    withRoot(check, log, checkFailures);
    if (check.ok()) {
        report.info("PASS");
        return EXIT_SUCCESS;
    }
    report.error("FAIL");
    return EXIT_FAILURE;
}
