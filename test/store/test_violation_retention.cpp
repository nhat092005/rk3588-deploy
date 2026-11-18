// ViolationRetention: what it counts (only directories that ViolationStore wrote), the three
// limits (age, bytes, free space), the order of removal (oldest first), and that it never removes
// anything else. Uses a temporary directory and fixed times in UTC. Exit code 0 when every check
// passes.
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
#include "violation_retention.h"

namespace {
namespace fs = std::filesystem;

constexpr int year{2026};
constexpr int yearBase{1900};
constexpr int november{10}; // tm_mon
constexpr int today{18};
constexpr int noon{12};
// 2026-11-18 12:00:00 UTC, the time the age is measured against.
constexpr std::uint64_t smallViolation{1000};
constexpr std::uint64_t mediumViolation{2000};
constexpr std::uint64_t unlimited{std::numeric_limits<std::uint64_t>::max()};
constexpr std::uint64_t limitOfTwoAndHalf{2500};
constexpr std::uint64_t limitOfOneAndHalf{1500};
constexpr std::uint64_t limitOfThree{3000};
constexpr int thirtyDays{30};
constexpr int oneDay{1};
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
    std::string pattern{(fs::temp_directory_path(error) / "violation_retention_XXXXXX").string()};
    if (error || mkdtemp(pattern.data()) == nullptr) {
        return std::nullopt;
    }
    return fs::path{pattern};
}

// <root>/<day>/<name> with one file of `bytes` bytes.
fs::path makeViolation(const fs::path& root, const char* day, const char* name,
                       std::uint64_t bytes) {
    const fs::path directory{root / day / name};
    std::error_code error;
    fs::create_directories(directory, error);
    std::ofstream file{directory / "original.jpg", std::ios::binary};
    file << std::string(bytes, 'x');
    return directory;
}

RetentionConfig configFor(const std::string& root, std::uint64_t maxBytes, int keepDays,
                          std::uint64_t minFree) {
    return RetentionConfig{root, maxBytes, keepDays, minFree};
}

// Runs `body` with a fresh root, removed afterwards.
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

void checkScan(Check& check, const fs::path& root, const Logger& log) {
    (void)makeViolation(root, "2026-11-17", "100000_id1_NoHelmet", smallViolation);
    (void)makeViolation(root, "2026-11-17", "101000_id2_NoVest_2", mediumViolation);
    (void)makeViolation(root, "2026-11-17", "102000_id3_NoHelmetNoVest", smallViolation);
    // None of these is ours.
    (void)makeViolation(root, "2026-11-17", "notes", smallViolation);
    (void)makeViolation(root, "2026-11-17", ".101500_id3_NoHelmet.tmp", smallViolation);
    (void)makeViolation(root, "2026-11-17", ".101600_id3_NoHelmet_2.tmp", smallViolation);
    (void)makeViolation(root, "2026-11-17", ".notes.tmp", smallViolation);
    (void)makeViolation(root, "2026-11-17", "101700_id3_NoHelmet.tmp", smallViolation);
    (void)makeViolation(root, "2026-11-17", ".101800_id3_Weird.tmp", smallViolation);
    (void)makeViolation(root, "2026-11-17", ".101900_id3_NoHelmet", smallViolation);
    (void)makeViolation(root, "2026-11-17", "x101700_id3_NoHelmet.tmp", smallViolation);
    (void)makeViolation(root, "2026-11-17", ".101900_id3_NoHelmetxxxx", smallViolation);
    (void)makeViolation(root, "2026-13-45", ".101500_id3_NoHelmet.tmp", smallViolation);
    const fs::path nestedTemp{
        makeViolation(root, "2026-11-17", ".102100_id3_NoVest.tmp", smallViolation)};
    fs::create_directories(nestedTemp / "inner");
    (void)makeViolation(root, "2026-11-17", "120000_id4_Weird", smallViolation);
    (void)makeViolation(root, "2026-11-17", "121000_id5_", smallViolation);
    (void)makeViolation(root, "2026-11-17", "122000_idx_NoVest", smallViolation);
    (void)makeViolation(root, "2026-13-45", "100000_id1_NoHelmet", smallViolation);
    (void)makeViolation(root, "backup", "100000_id1_NoHelmet", smallViolation);
    std::ofstream{root / "2026-11-17" / "readme.txt"} << "hello";
    const fs::path nested{makeViolation(root, "2026-11-17", "130000_id6_NoVest", smallViolation)};
    fs::create_directories(nested / "subdirectory");
    // Links, a file with the name of a violation, a violation with a link inside, odd dates.
    std::error_code error;
    (void)makeViolation(root, "linktarget", "files", smallViolation);
    (void)makeViolation(root, "linktarget", "180000_id9_NoHelmet", smallViolation);
    fs::create_directory_symlink(root / "linktarget" / "files",
                                 root / "2026-11-17" / "140000_id7_NoVest", error);
    fs::create_directory_symlink(root / "linktarget", root / "2026-11-16", error);
    fs::create_directory_symlink(root / "linktarget" / "files",
                                 root / "2026-11-17" / ".102200_id3_NoVest.tmp", error);
    std::ofstream{root / "2026-11-17" / "150000_id8_NoVest"} << "a file, not a directory";
    const fs::path withLink{makeViolation(root, "2026-11-17", "160000_id9_NoVest", smallViolation)};
    fs::create_symlink(withLink / "original.jpg", withLink / "link.jpg", error);
    (void)makeViolation(root, "2026-11-17", "100061_id1_NoVest", smallViolation);
    (void)makeViolation(root, "2026-11-17", "106000_id1_NoVest", smallViolation);
    (void)makeViolation(root, "2026x11x17", "100000_id1_NoHelmet", smallViolation);
    const std::string text{root.string()};
    const std::optional<ViolationRetention> retention{
        ViolationRetention::create(configFor(text, unlimited, thirtyDays, 0), log)};
    check.expect(retention.has_value(), "created");
    if (!retention) {
        return;
    }
    check.expect(retention->count() == three, "only the three directories of ViolationStore");
    check.expect(retention->sweptCount() == 2 &&
                     !fs::exists(root / "2026-11-17" / ".101500_id3_NoHelmet.tmp") &&
                     !fs::exists(root / "2026-11-17" / ".101600_id3_NoHelmet_2.tmp"),
                 "the leftover temporary directories are removed");
    check.expect(fs::exists(root / "2026-11-17" / ".notes.tmp") &&
                     fs::exists(root / "2026-11-17" / "101700_id3_NoHelmet.tmp") &&
                     fs::exists(root / "2026-11-17" / ".101800_id3_Weird.tmp") &&
                     fs::exists(root / "2026-11-17" / ".101900_id3_NoHelmet") &&
                     fs::exists(root / "2026-11-17" / "x101700_id3_NoHelmet.tmp") &&
                     fs::exists(root / "2026-11-17" / ".101900_id3_NoHelmetxxxx") &&
                     fs::is_symlink(root / "2026-11-17" / ".102200_id3_NoVest.tmp") &&
                     fs::exists(root / "2026-13-45" / ".101500_id3_NoHelmet.tmp") &&
                     fs::exists(nestedTemp),
                 "look-alikes and a temporary directory with a directory in it stay");
    check.expect(retention->totalBytes() == smallViolation + mediumViolation + smallViolation,
                 "their bytes");
}

void checkAge(Check& check, const fs::path& root, const Logger& log) {
    const fs::path old{makeViolation(root, "2026-10-01", "100000_id1_NoHelmet", smallViolation)};
    const fs::path justOld{
        makeViolation(root, "2026-10-19", "110000_id2_NoHelmet", smallViolation)};
    const fs::path exactly{makeViolation(root, "2026-10-19", "120000_id4_NoVest", smallViolation)};
    const fs::path young{makeViolation(root, "2026-10-19", "130000_id3_NoVest", smallViolation)};
    const fs::path other{makeViolation(root, "2026-10-19", "notes", smallViolation)};
    const std::string text{root.string()};
    std::optional<ViolationRetention> retention{
        ViolationRetention::create(configFor(text, unlimited, thirtyDays, 0), log)};
    if (!retention) {
        check.expect(false, "created for the age check");
        return;
    }
    check.expect(retention->enforce(nowTime()), "there is room");
    check.expect(!fs::exists(old), "a violation older than the days is removed");
    check.expect(!fs::exists(root / "2026-10-01"), "and its empty day directory");
    check.expect(!fs::exists(justOld), "one that is a little too old goes");
    check.expect(fs::exists(exactly), "one exactly at the limit stays");
    check.expect(fs::exists(young), "one just inside the limit stays");
    check.expect(fs::exists(other) && fs::exists(root / "2026-10-19"),
                 "foreign content stays and keeps its day directory");
    check.expect(retention->count() == two && retention->removedCount() == two, "the counts");
}

void checkBytes(Check& check, const fs::path& root, const Logger& log) {
    const fs::path first{makeViolation(root, "2026-11-15", "100000_id1_NoHelmet", smallViolation)};
    const fs::path second{makeViolation(root, "2026-11-16", "100000_id2_NoHelmet", smallViolation)};
    const fs::path third{makeViolation(root, "2026-11-17", "100000_id3_NoHelmet", smallViolation)};
    const std::string text{root.string()};
    {
        std::optional<ViolationRetention> exact{
            ViolationRetention::create(configFor(text, limitOfThree, thirtyDays, 0), log)};
        check.expect(exact && exact->enforce(nowTime()) && exact->count() == three,
                     "bytes equal to the limit remove nothing");
    }
    std::optional<ViolationRetention> retention{
        ViolationRetention::create(configFor(text, limitOfTwoAndHalf, thirtyDays, 0), log)};
    check.expect(retention && retention->enforce(nowTime()), "room after removing");
    check.expect(!fs::exists(first), "the oldest goes first");
    check.expect(fs::exists(second) && fs::exists(third), "the newer ones stay");
    check.expect(retention && retention->totalBytes() == two * smallViolation, "the total follows");
}

void checkAddedViolations(Check& check, const fs::path& root, const Logger& log) {
    const std::string text{root.string()};
    std::optional<ViolationRetention> retention{
        ViolationRetention::create(configFor(text, limitOfTwoAndHalf, thirtyDays, 0), log)};
    if (!retention) {
        check.expect(false, "created for the added violations");
        return;
    }
    // Added newest first: the index still removes the oldest.
    const fs::path newer{makeViolation(root, "2026-11-17", "100000_id2_NoHelmet", smallViolation)};
    retention->add(newer.string());
    const fs::path older{makeViolation(root, "2026-11-16", "100000_id1_NoHelmet", smallViolation)};
    retention->add(older.string());
    check.expect(retention->count() == two && retention->totalBytes() == two * smallViolation,
                 "two added violations");
    // The clip of the newer one arrives: 1000 + 1000 + 1000 is over the limit of 2500.
    retention->addBytes(newer.string(), smallViolation);
    check.expect(retention->totalBytes() == three * smallViolation, "addBytes raises the total");
    check.expect(retention->enforce(nowTime()), "room after removing");
    check.expect(!fs::exists(older) && fs::exists(newer), "the older violation was removed");
    retention->add((root / "not-a-violation").string());
    check.expect(retention->count() == 1, "a path that is not a violation is not counted");
    // The bytes added later belong to the entry: removing it takes them away too.
    std::optional<ViolationRetention> tight{
        ViolationRetention::create(configFor(text, limitOfOneAndHalf, thirtyDays, 0), log)};
    const fs::path clipped{makeViolation(root, "2026-11-18", "090000_id5_NoVest", smallViolation)};
    if (tight) {
        tight->add(clipped.string());
        tight->addBytes(clipped.string(), smallViolation);
        check.expect(tight->enforce(nowTime()) && tight->totalBytes() == 0 && tight->count() == 0,
                     "the bytes of a clip leave with its violation");
    }
}

void checkFreeSpace(Check& check, const fs::path& root, const Logger& log) {
    const fs::path first{makeViolation(root, "2026-11-16", "100000_id1_NoHelmet", smallViolation)};
    const fs::path second{makeViolation(root, "2026-11-17", "100000_id2_NoVest", smallViolation)};
    const fs::path foreign{makeViolation(root, "2026-11-17", "notes", smallViolation)};
    const std::string text{root.string()};
    {
        std::optional<ViolationRetention> plenty{
            ViolationRetention::create(configFor(text, unlimited, thirtyDays, 1), log)};
        check.expect(plenty && plenty->enforce(nowTime()) && plenty->count() == two,
                     "enough free space removes nothing");
    }
    std::optional<ViolationRetention> never{
        ViolationRetention::create(configFor(text, unlimited, thirtyDays, unlimited), log)};
    check.expect(never && !never->enforce(nowTime()),
                 "no room when the volume cannot give the free space");
    check.expect(never && never->count() == 0, "every violation was removed trying");
    check.expect(!fs::exists(first) && !fs::exists(second), "their directories are gone");
    check.expect(fs::exists(foreign), "the foreign directory stays");
}

void checkRescanAndConfig(Check& check, const fs::path& root, const Logger& log) {
    const std::string text{root.string()};
    std::optional<ViolationRetention> retention{
        ViolationRetention::create(configFor(text, unlimited, thirtyDays, 0), log)};
    (void)makeViolation(root, "2026-11-17", "100000_id1_NoHelmet", smallViolation);
    if (!retention) {
        check.expect(false, "created for the rescan");
        return;
    }
    check.expect(retention->count() == 0, "nothing before the rescan");
    retention->rescan();
    check.expect(retention->count() == 1, "the rescan finds what was added");
    check.expect(!ViolationRetention::create(configFor(text, 0, thirtyDays, 0), log),
                 "no byte limit is refused");
    check.expect(!ViolationRetention::create(configFor(text, unlimited, 0, 0), log),
                 "zero days is refused");
    const std::string missing{(root / "missing").string()};
    check.expect(!ViolationRetention::create(configFor(missing, unlimited, oneDay, 0), log),
                 "a missing root is refused");
}
} // namespace

int main() {
    const Logger log{"TEST_RETENTION", LogLevel::None};
    const Logger report{"TEST_RETENTION", LogLevel::Debug};
    setenv("TZ", "UTC", 1);
    tzset();
    Check check{report};
    withRoot(check, log, checkScan);
    withRoot(check, log, checkAge);
    withRoot(check, log, checkBytes);
    withRoot(check, log, checkAddedViolations);
    withRoot(check, log, checkFreeSpace);
    withRoot(check, log, checkRescanAndConfig);
    if (check.ok()) {
        report.info("PASS");
        return EXIT_SUCCESS;
    }
    report.error("FAIL");
    return EXIT_FAILURE;
}
