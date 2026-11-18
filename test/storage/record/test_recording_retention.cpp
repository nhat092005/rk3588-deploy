// RecordingRetention: what it counts (only the files of SegmentWriter), the three limits (age,
// bytes, free space), the order of removal (oldest first), the sweep of the temporary files that an
// interrupted run left, and that it never removes anything else. Uses a temporary directory and
// fixed times in UTC. Exit code 0 when every check passes.
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <system_error>

#include <unistd.h>

#include "logger.h"
#include "recording_retention.h"

namespace {
namespace fs = std::filesystem;

constexpr int year{2026};
constexpr int yearBase{1900};
constexpr int november{10}; // tm_mon
constexpr int today{18};
constexpr int noon{12};
constexpr std::uint64_t segmentBytes{1000};
constexpr std::uint64_t unlimited{std::numeric_limits<std::uint64_t>::max()};
constexpr std::uint64_t limitOfTwoAndHalf{2500};
constexpr std::uint64_t limitOfThree{3000};
constexpr int sevenDays{7};
constexpr std::size_t three{3};
constexpr std::size_t two{2};

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

// 2026-11-18 12:00:00 UTC.
std::time_t nowTime() {
    std::tm parts{};
    parts.tm_year = year - yearBase;
    parts.tm_mon = november;
    parts.tm_mday = today;
    parts.tm_hour = noon;
    return timegm(&parts);
}

std::optional<fs::path> makeRoot() {
    std::error_code error;
    std::string pattern{(fs::temp_directory_path(error) / "recording_retention_XXXXXX").string()};
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
    fs::remove_all(*root, error);
}

// <root>/<day>/<name> as a file of `bytes` bytes.
fs::path makeFile(const fs::path& root, const char* day, const char* name, std::uint64_t bytes) {
    std::error_code error;
    fs::create_directories(root / day, error);
    const fs::path path{root / day / name};
    std::ofstream file{path, std::ios::binary};
    file << std::string(bytes, 'x');
    return path;
}

RecordingRetentionConfig configFor(const std::string& root, std::uint64_t maxBytes, int keepDays,
                                   std::uint64_t minFree) {
    return RecordingRetentionConfig{root, maxBytes, keepDays, minFree};
}

void checkScanAndSweep(Check& check, const fs::path& root, const Logger& log) {
    (void)makeFile(root, "2026-11-17", "100000.h265", segmentBytes);
    (void)makeFile(root, "2026-11-17", "100100_2.h265", segmentBytes);
    (void)makeFile(root, "2026-11-18", "090000.h265", segmentBytes);
    // Not ours: wrong names, other suffixes, a directory, links, odd dates.
    (void)makeFile(root, "2026-11-17", "notes.txt", segmentBytes);
    (void)makeFile(root, "2026-11-17", "100200.mp4", segmentBytes);
    (void)makeFile(root, "2026-11-17", "10020.h265", segmentBytes);
    (void)makeFile(root, "2026-11-17", "100200_.h265", segmentBytes);
    (void)makeFile(root, "2026-11-17", "100200_x.h265", segmentBytes);
    (void)makeFile(root, "2026-11-17", "100200.h265.bak", segmentBytes);
    (void)makeFile(root, "2026-11-17", "100261.h265", segmentBytes);
    (void)makeFile(root, "2026-11-17", "100200", segmentBytes);
    (void)makeFile(root, "2026-11-17", "100200xxxxx", segmentBytes);
    (void)makeFile(root, "2026-13-40", "100000.h265", segmentBytes);
    (void)makeFile(root, "backup", "100000.h265", segmentBytes);
    std::error_code error;
    fs::create_directories(root / "2026-11-17" / "110000.h265", error);
    fs::create_symlink(root / "2026-11-17" / "100000.h265", root / "2026-11-17" / "120000.h265",
                       error);
    fs::create_directory_symlink(root / "backup", root / "2026-11-16", error);
    // Leftover temporary files of an interrupted run, and look-alikes that must stay.
    const fs::path leftover{makeFile(root, "2026-11-17", ".100300.h265.tmp", segmentBytes)};
    const fs::path leftoverTwo{makeFile(root, "2026-11-18", ".090100_2.h265.tmp", segmentBytes)};
    const fs::path foreignHidden{makeFile(root, "2026-11-17", ".hidden.tmp", segmentBytes)};
    const fs::path hiddenTmp{makeFile(root, "2026-11-17", ".100400.mp4.tmp", segmentBytes)};
    const fs::path outsideDay{makeFile(root, "backup", ".100300.h265.tmp", segmentBytes)};
    const fs::path notHidden{makeFile(root, "2026-11-17", "100300.h265.tmp", segmentBytes)};
    const fs::path shifted{makeFile(root, "2026-11-17", "x100300.h265.tmp", segmentBytes)};
    const fs::path badStem{makeFile(root, "2026-11-17", ".foo.h265.tmp", segmentBytes)};
    const fs::path letters{makeFile(root, "2026-11-17", ".abcdef.h265.tmp", segmentBytes)};
    fs::create_directories(root / "2026-11-17" / ".100500.h265.tmp", error);
    const std::string text{root.string()};
    const std::optional<RecordingRetention> retention{
        RecordingRetention::create(configFor(text, unlimited, sevenDays, 0), log)};
    check.expect(retention.has_value(), "created");
    if (!retention) {
        return;
    }
    check.expect(retention->count() == three && retention->totalBytes() == three * segmentBytes,
                 "only the three segments of SegmentWriter");
    check.expect(retention->sweptCount() == two && !fs::exists(leftover) &&
                     !fs::exists(leftoverTwo),
                 "the leftover temporary files are removed");
    check.expect(fs::exists(foreignHidden) && fs::exists(hiddenTmp) && fs::exists(outsideDay),
                 "hidden files with other names, and a day that is not a day, stay");
    check.expect(fs::exists(notHidden) && fs::exists(shifted) && fs::exists(badStem) &&
                     fs::exists(letters),
                 "a name that is not exactly .<HHMMSS>[_k].h265.tmp stays");
    check.expect(fs::is_directory(root / "2026-11-17" / ".100500.h265.tmp"),
                 "a directory with that name stays");
}

void checkAge(Check& check, const fs::path& root, const Logger& log) {
    const fs::path old{makeFile(root, "2026-11-01", "100000.h265", segmentBytes)};
    const fs::path justOld{makeFile(root, "2026-11-11", "110000.h265", segmentBytes)};
    const fs::path exactly{makeFile(root, "2026-11-11", "120000.h265", segmentBytes)};
    const fs::path young{makeFile(root, "2026-11-11", "130000.h265", segmentBytes)};
    const fs::path other{makeFile(root, "2026-11-11", "notes.txt", segmentBytes)};
    const std::string text{root.string()};
    std::optional<RecordingRetention> retention{
        RecordingRetention::create(configFor(text, unlimited, sevenDays, 0), log)};
    if (!retention) {
        check.expect(false, "created for the age check");
        return;
    }
    check.expect(retention->enforce(nowTime()), "there is room");
    check.expect(!fs::exists(old) && !fs::exists(root / "2026-11-01"),
                 "an old segment and its empty day directory are removed");
    check.expect(!fs::exists(justOld), "a segment a little too old goes");
    check.expect(fs::exists(exactly), "one exactly at the limit stays");
    check.expect(fs::exists(young), "one just inside the limit stays");
    check.expect(fs::exists(other) && fs::exists(root / "2026-11-11"),
                 "foreign content stays and keeps its day directory");
    check.expect(retention->count() == two && retention->removedCount() == two, "the counts");
}

void checkBytes(Check& check, const fs::path& root, const Logger& log) {
    const fs::path first{makeFile(root, "2026-11-15", "100000.h265", segmentBytes)};
    const fs::path second{makeFile(root, "2026-11-16", "100000.h265", segmentBytes)};
    const fs::path third{makeFile(root, "2026-11-17", "100000.h265", segmentBytes)};
    const std::string text{root.string()};
    {
        std::optional<RecordingRetention> exact{
            RecordingRetention::create(configFor(text, limitOfThree, sevenDays, 0), log)};
        check.expect(exact && exact->enforce(nowTime()) && exact->count() == three,
                     "bytes equal to the limit remove nothing");
    }
    std::optional<RecordingRetention> retention{
        RecordingRetention::create(configFor(text, limitOfTwoAndHalf, sevenDays, 0), log)};
    check.expect(retention && retention->enforce(nowTime()), "room after removing");
    check.expect(!fs::exists(first) && fs::exists(second) && fs::exists(third),
                 "the oldest goes first");
    check.expect(retention && retention->totalBytes() == two * segmentBytes, "the total follows");
}

void checkAdd(Check& check, const fs::path& root, const Logger& log) {
    const std::string text{root.string()};
    std::optional<RecordingRetention> retention{
        RecordingRetention::create(configFor(text, limitOfTwoAndHalf, sevenDays, 0), log)};
    if (!retention) {
        check.expect(false, "created for add");
        return;
    }
    // Added newest first: the index still removes the oldest.
    const fs::path newer{makeFile(root, "2026-11-17", "100000.h265", segmentBytes)};
    retention->add(newer.string());
    const fs::path older{makeFile(root, "2026-11-16", "100000.h265", segmentBytes)};
    retention->add(older.string());
    const fs::path newest{makeFile(root, "2026-11-18", "090000.h265", segmentBytes)};
    retention->add(newest.string());
    check.expect(retention->count() == three && retention->totalBytes() == three * segmentBytes,
                 "three added segments, sized from the disk");
    retention->add((root / "2026-11-18" / "notes.txt").string());
    retention->add((root / "2026-11-18" / "100000.h265").string());
    const fs::path badDay{makeFile(root, "2026-13-40", "100000.h265", segmentBytes)};
    retention->add(badDay.string());
    check.expect(retention->count() == three,
                 "a name that is not a segment, or a missing file, is not counted");
    check.expect(retention->enforce(nowTime()), "room after removing");
    check.expect(!fs::exists(older) && fs::exists(newer) && fs::exists(newest),
                 "the oldest segment was removed");
}

void checkFreeSpace(Check& check, const fs::path& root, const Logger& log) {
    const fs::path first{makeFile(root, "2026-11-16", "100000.h265", segmentBytes)};
    const fs::path second{makeFile(root, "2026-11-17", "100000.h265", segmentBytes)};
    const fs::path foreign{makeFile(root, "2026-11-17", "notes.txt", segmentBytes)};
    const std::string text{root.string()};
    {
        std::optional<RecordingRetention> plenty{
            RecordingRetention::create(configFor(text, unlimited, sevenDays, 1), log)};
        check.expect(plenty && plenty->enforce(nowTime()) && plenty->count() == two,
                     "enough free space removes nothing");
    }
    std::optional<RecordingRetention> never{
        RecordingRetention::create(configFor(text, unlimited, sevenDays, unlimited), log)};
    check.expect(never && !never->enforce(nowTime()),
                 "no room when the volume cannot give the free space");
    check.expect(never && never->count() == 0 && !fs::exists(first) && !fs::exists(second),
                 "every segment was removed trying");
    check.expect(fs::exists(foreign), "the foreign file stays");
}

void checkConfig(Check& check, const fs::path& root, const Logger& log) {
    const std::string text{root.string()};
    check.expect(!RecordingRetention::create(configFor(text, 0, sevenDays, 0), log),
                 "no byte limit is refused");
    check.expect(!RecordingRetention::create(configFor(text, unlimited, 0, 0), log),
                 "zero days is refused");
    const std::string missing{(root / "missing").string()};
    check.expect(!RecordingRetention::create(configFor(missing, unlimited, 1, 0), log),
                 "a missing root is refused");
    std::optional<RecordingRetention> retention{
        RecordingRetention::create(configFor(text, unlimited, sevenDays, 0), log)};
    (void)makeFile(root, "2026-11-17", "100000.h265", segmentBytes);
    check.expect(retention && retention->count() == 0, "nothing before the rescan");
    if (retention) {
        retention->rescan();
        check.expect(retention->count() == 1, "the rescan finds what was added");
    }
}
} // namespace

int main() {
    const Logger log{"TEST_RECORD_RETENTION", LogLevel::None};
    const Logger report{"TEST_RECORD_RETENTION", LogLevel::Debug};
    setenv("TZ", "UTC", 1);
    tzset();
    Check check{report};
    withRoot(check, log, checkScanAndSweep);
    withRoot(check, log, checkAge);
    withRoot(check, log, checkBytes);
    withRoot(check, log, checkAdd);
    withRoot(check, log, checkFreeSpace);
    withRoot(check, log, checkConfig);
    if (check.ok()) {
        report.info("PASS");
        return EXIT_SUCCESS;
    }
    report.error("FAIL");
    return EXIT_FAILURE;
}
