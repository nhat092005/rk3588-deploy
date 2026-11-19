// SessionWriter: the file opens at the first key frame under its final name, holds the packets in
// order, is named by the wall clock (with a suffix when the name repeats), a gap in the stream does
// not start a new file, and it stops for good, keeping the file, when the volume falls below the
// floor or the disk refuses. The root and the floor are checked at creation. Uses a temporary
// directory and fixed times in UTC. Exit code 0 when every check passes.
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
#include "session_writer.h"
#include "video_file.h"

namespace {
namespace fs = std::filesystem;

constexpr int year{2026};
constexpr int yearBase{1900};
constexpr int november{10}; // tm_mon
constexpr int today{18};
constexpr int hourOfFirst{9};
constexpr int minuteOfFirst{30};
constexpr int secondsLater{5};
// Values and times of the made-up packets.
constexpr std::uint8_t packetA{1};
constexpr std::uint8_t packetB{2};
constexpr std::uint8_t packetC{3};
constexpr std::uint8_t packetD{4};
constexpr std::uint8_t packetE{5};
constexpr double timeFirst{0.1};
constexpr double timeSecond{0.2};
constexpr double timeThird{1.0};
constexpr double timeFourth{2.0};
constexpr std::size_t packetSize{50};
constexpr unsigned modeMask{07777};
constexpr std::uint64_t floorBytes{1000};
constexpr std::uint64_t roomBytes{5000};
constexpr std::uint64_t lowBytes{10};

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

// What the made-up volume has free: a test sets it, the writer reads it.
std::optional<std::uint64_t>& madeUpFree() {
    static std::optional<std::uint64_t> value{roomBytes};
    return value;
}

std::optional<std::uint64_t> freeNow(const std::string& /*path*/) {
    return madeUpFree();
}

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
    std::string pattern{(fs::temp_directory_path(error) / "session_writer_XXXXXX").string()};
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
    madeUpFree() = roomBytes;
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

SessionWriter makeWriter(const fs::path& root, const Logger& log, unsigned mode = defaultFileMode) {
    const std::string text{root.string()};
    std::optional<SessionWriter> writer{
        SessionWriter::create(SessionConfig{text, floorBytes, mode, &freeNow}, log)};
    if (!writer) {
        std::abort();
    }
    return std::move(*writer);
}

std::string readText(const fs::path& path) {
    std::ifstream file{path, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

std::string filled(std::uint8_t value) {
    return std::string(packetSize, static_cast<char>(value));
}

// A format that writes two characters for a packet (its value and k or d), F for a flush and E for
// the end: it shows what SessionWriter hands to the writer of a format, and in which order.
class MarkWriter final : public VideoFileWriter {
public:
    MarkWriter(int descriptor, const Logger& logger) : file{descriptor}, log{logger} {}

    bool write(const std::uint8_t* data, std::size_t /*size*/, bool keyFrame,
               double /*seconds*/) override {
        const std::string marks{static_cast<char>('0' + data[0]), keyFrame ? 'k' : 'd'};
        return put(marks);
    }
    bool flush() override {
        return put("F");
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
    std::optional<SessionWriter> writer{
        SessionWriter::create(SessionConfig{text, floorBytes, defaultFileMode, &freeNow,
                                            VideoOutput{".mp4", &makeMarkWriter}},
                              log)};
    check.expect(writer.has_value(), "a writer of another format is created");
    if (!writer) {
        return;
    }
    const std::time_t start{at(Hms{hourOfFirst, minuteOfFirst, 0})};
    check.expect(writer->consume(packet(packetB, true, timeFirst), start) &&
                     writer->consume(packet(packetC, false, timeSecond), start) &&
                     writer->consume(packet(packetD, true, timeThird), start + 1) &&
                     writer->consume(packet(packetE, false, timeFourth), start + 2),
                 "four packets");
    writer->finish();
    const fs::path file{root / "2026-11-18" / "093000.mp4"};
    check.expect(writer->path() == file.string(), "the name has the suffix of the format");
    // The flush comes after the key frame that closes a group, not before it, and not at the first.
    check.expect(readText(file) == "2k3d4kF5dE", "the writer gets the packets, a flush after each "
                                                 "later key frame, and the end");
    check.expect(!fs::exists(root / "2026-11-18" / "093000.h265"), "no .h265 file");
    check.expect(!SessionWriter::create(SessionConfig{text, floorBytes, defaultFileMode, &freeNow,
                                                      VideoOutput{".mp4", nullptr}},
                                        log),
                 "a format without a writer is refused");
}

void checkFile(Check& check, const fs::path& root, const Logger& log) {
    SessionWriter writer{makeWriter(root, log)};
    const fs::path day{root / "2026-11-18"};
    const std::time_t start{at(Hms{hourOfFirst, minuteOfFirst, 0})};
    check.expect(writer.consume(packet(packetA, false, 0.0), start) && writer.skippedPackets() == 1,
                 "a frame before any key frame is skipped");
    check.expect(writer.path().empty() && !fs::exists(day), "nothing is created until a key frame");
    check.expect(writer.consume(packet(packetB, true, timeFirst), start), "the first key frame");
    check.expect(writer.path() == (day / "093000.h265").string(),
                 "the file is named by the clock of the first key frame");
    check.expect(fs::exists(day / "093000.h265"), "it has its final name while it is open");
    check.expect(writer.consume(packet(packetC, false, timeSecond), start), "an ordinary frame");
    check.expect(writer.consume(packet(packetD, true, timeThird), start + 1),
                 "the next key frame goes to the same file");
    check.expect(writer.consume(packet(packetE, false, timeFourth), start + 2), "another frame");
    writer.finish();
    check.expect(readText(day / "093000.h265") ==
                     filled(packetB) + filled(packetC) + filled(packetD) + filled(packetE),
                 "the file holds the packets in order");
    check.expect(writer.bytesWritten() == 4 * packetSize && !writer.stopped(),
                 "four packets written, not stopped");
    std::size_t hidden{0};
    std::error_code error;
    for (const fs::directory_entry& entry : fs::directory_iterator{day, error}) {
        hidden += entry.path().filename().string().front() == '.' ? 1 : 0;
    }
    check.expect(hidden == 0, "no temporary file");
    struct stat info {};
    check.expect(::stat((day / "093000.h265").c_str(), &info) == 0 &&
                     (info.st_mode & modeMask) == defaultFileMode,
                 "the permissions of the file");
}

void checkGap(Check& check, const fs::path& root, const Logger& log) {
    SessionWriter writer{makeWriter(root, log)};
    const std::time_t start{at(Hms{hourOfFirst, minuteOfFirst, 0})};
    check.expect(writer.consume(packet(packetA, true, timeFirst), start), "first key frame");
    check.expect(writer.consume(packet(packetB, true, timeSecond, true), start + 1),
                 "a key frame after a gap");
    writer.finish();
    check.expect(writer.gapCount() == 1, "the gap is counted");
    std::size_t files{0};
    std::error_code error;
    for (const fs::directory_entry& entry : fs::directory_iterator{root / "2026-11-18", error}) {
        files += entry.is_regular_file() ? 1 : 0;
    }
    check.expect(files == 1, "a gap does not start a new file");
    check.expect(readText(root / "2026-11-18" / "093000.h265") == filled(packetA) + filled(packetB),
                 "the stream goes on after the gap");
}

void checkNames(Check& check, const fs::path& root, const Logger& log) {
    const std::time_t start{at(Hms{hourOfFirst, minuteOfFirst, 0})};
    SessionWriter first{makeWriter(root, log)};
    check.expect(first.consume(packet(packetA, true, timeFirst), start), "first run");
    first.finish();
    SessionWriter second{makeWriter(root, log)};
    check.expect(second.consume(packet(packetB, true, timeFirst), start),
                 "second run, same second");
    second.finish();
    const fs::path day{root / "2026-11-18"};
    check.expect(second.path() == (day / "093000_2.h265").string(),
                 "the name that repeats gets a suffix");
    check.expect(readText(day / "093000.h265") == filled(packetA),
                 "the first file is left as it was");
    SessionWriter third{makeWriter(root, log)};
    check.expect(third.consume(packet(packetC, true, timeFirst),
                               at(Hms{hourOfFirst, minuteOfFirst, secondsLater})),
                 "a run later in the day");
    third.finish();
    check.expect(third.path() == (day / "093005.h265").string(),
                 "a later run shares the folder of the day");
}

void checkFloor(Check& check, const fs::path& root, const Logger& log) {
    SessionWriter writer{makeWriter(root, log)};
    const std::time_t start{at(Hms{hourOfFirst, minuteOfFirst, 0})};
    check.expect(writer.consume(packet(packetA, true, timeFirst), start), "first key frame");
    check.expect(writer.consume(packet(packetB, false, timeSecond), start), "an ordinary frame");
    madeUpFree() = lowBytes;
    check.expect(writer.consume(packet(packetC, false, timeThird), start),
                 "an ordinary frame does not check the room");
    check.expect(!writer.stopped(), "still writing between key frames");
    check.expect(writer.consume(packet(packetD, true, timeFourth), start + 1),
                 "a key frame below the floor");
    check.expect(writer.stopped() && !writer.stopReason().empty(), "it stops and says why");
    check.expect(writer.consume(packet(packetE, true, timeFourth), start + 2),
                 "a packet after the stop is ignored");
    madeUpFree() = roomBytes;
    check.expect(writer.consume(packet(packetE, true, timeFourth), start + 3) && writer.stopped(),
                 "it does not start again when the room comes back");
    writer.finish();
    check.expect(writer.bytesWritten() == 3 * packetSize,
                 "nothing is written after the stop, even when the room comes back");
    std::size_t files{0};
    std::error_code error;
    for (const fs::directory_entry& entry : fs::directory_iterator{root / "2026-11-18", error}) {
        files += entry.is_regular_file() ? 1 : 0;
    }
    check.expect(files == 1, "no second file after the stop");
    check.expect(readText(root / "2026-11-18" / "093000.h265") ==
                     filled(packetA) + filled(packetB) + filled(packetC),
                 "the file is kept with what was written before the stop");
}

void checkFloorOff(Check& check, const fs::path& root, const Logger& log) {
    const std::string text{root.string()};
    std::optional<SessionWriter> writer{
        SessionWriter::create(SessionConfig{text, 0, defaultFileMode, &freeNow}, log)};
    madeUpFree() = lowBytes;
    check.expect(writer.has_value(), "a floor of 0 is off: created with little room");
    if (!writer) {
        return;
    }
    const std::time_t start{at(Hms{hourOfFirst, minuteOfFirst, 0})};
    check.expect(writer->consume(packet(packetA, true, timeFirst), start) &&
                     writer->consume(packet(packetB, true, timeSecond), start + 1) &&
                     !writer->stopped(),
                 "it keeps writing with little room");
    writer->finish();
}

void checkCreate(Check& check, const fs::path& root, const Logger& log) {
    const std::string text{root.string()};
    madeUpFree() = lowBytes;
    check.expect(
        !SessionWriter::create(SessionConfig{text, floorBytes, defaultFileMode, &freeNow}, log)
             .has_value(),
        "creation is refused when the volume is already below the floor");
    madeUpFree() = roomBytes;
    const std::string missing{(root / "missing").string()};
    check.expect(
        !SessionWriter::create(SessionConfig{missing, floorBytes, defaultFileMode, &freeNow}, log)
             .has_value(),
        "a missing root is refused");
    const fs::path plain{root / "plain"};
    std::ofstream{plain} << "x";
    const std::string notDirectory{plain.string()};
    check.expect(!SessionWriter::create(
                      SessionConfig{notDirectory, floorBytes, defaultFileMode, &freeNow}, log)
                      .has_value(),
                 "a file is refused as a root");
    check.expect(
        SessionWriter::create(SessionConfig{text, floorBytes, defaultFileMode, &freeNow}, log)
            .has_value(),
        "a good root is accepted");
    // Not writable: only when not running as root (root can write anywhere).
    if (::geteuid() != 0) {
        const fs::path locked{root / "locked"};
        std::error_code error;
        fs::create_directory(locked, error);
        fs::permissions(locked, fs::perms::owner_read | fs::perms::owner_exec, error);
        const std::string lockedText{locked.string()};
        check.expect(!SessionWriter::create(
                          SessionConfig{lockedText, floorBytes, defaultFileMode, &freeNow}, log)
                          .has_value(),
                     "a root that cannot be written is refused");
        fs::permissions(locked, fs::perms::owner_all, error);
    }
}

// A write that the disk refuses: the file size limit makes write() fail with EFBIG. The file is
// kept with what was written, the writer stops and reports false.
void checkWriteError(Check& check, const fs::path& root, const Logger& log) {
    struct rlimit before {};
    if (::getrlimit(RLIMIT_FSIZE, &before) != 0) {
        check.expect(false, "getrlimit");
        return;
    }
    (void)std::signal(SIGXFSZ, SIG_IGN);
    SessionWriter writer{makeWriter(root, log)};
    const std::time_t start{at(Hms{hourOfFirst, minuteOfFirst, 0})};
    check.expect(writer.consume(packet(packetA, true, timeFirst), start), "first key frame");
    struct rlimit tight {};
    tight.rlim_cur = packetSize + packetSize / 2;
    tight.rlim_max = before.rlim_max;
    (void)::setrlimit(RLIMIT_FSIZE, &tight);
    const bool refused{writer.consume(packet(packetB, false, timeSecond), start)};
    (void)::setrlimit(RLIMIT_FSIZE, &before);
    check.expect(!refused && writer.stopped(), "a refused write stops the writer");
    check.expect(writer.consume(packet(packetC, true, timeThird), start + 1),
                 "later packets are ignored");
    writer.finish();
    check.expect(fs::exists(root / "2026-11-18" / "093000.h265") &&
                     readText(root / "2026-11-18" / "093000.h265").size() >= packetSize,
                 "the file is kept after a failed write");
}
} // namespace

int main() {
    const Logger log{"TEST_SESSION", LogLevel::None};
    const Logger report{"TEST_SESSION", LogLevel::Debug};
    setenv("TZ", "UTC", 1);
    tzset();
    Check check{report};
    withRoot(check, log, checkFile);
    withRoot(check, log, checkFormat);
    withRoot(check, log, checkGap);
    withRoot(check, log, checkNames);
    withRoot(check, log, checkFloor);
    withRoot(check, log, checkFloorOff);
    withRoot(check, log, checkCreate);
    withRoot(check, log, checkWriteError);
    if (check.ok()) {
        report.info("PASS");
        return EXIT_SUCCESS;
    }
    return EXIT_FAILURE;
}
