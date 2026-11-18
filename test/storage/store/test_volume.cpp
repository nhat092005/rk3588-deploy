// The volume helpers: the budget check (pure arithmetic, at its boundary), the free space of a
// path and whether two paths share a volume. Plain C++ and POSIX, runs on the PC. Exit code 0 when
// every check passes.
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "logger.h"
#include "volume.h"

namespace {
namespace fs = std::filesystem;

constexpr std::uint64_t gigabyte{1'000'000'000};
// Free space around the 12 GB that 4 + 5 + 3 need.
constexpr std::uint64_t comfortable{13 * gigabyte};
constexpr std::uint64_t exactlyEnough{12 * gigabyte};
constexpr std::uint64_t oneByteShort{exactlyEnough - 1};
constexpr std::uint64_t tooLittle{11 * gigabyte};
// With 3 GB already used: 12 GB needed, 9 GB free is exactly enough.
constexpr std::uint64_t enoughWithUsed{9 * gigabyte};

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

void checkBudgets(Check& check) {
    // 4 GB + 5 GB of budgets and a floor of 3 GB need 12 GB; 13 GB free fits, 11 GB does not.
    const std::vector<FolderBudget> both{{4 * gigabyte, 2 * gigabyte, 0},
                                         {5 * gigabyte, 3 * gigabyte, 0}};
    check.expect(!overCommitted(both, comfortable), "13 GB free fits 4 + 5 + 3");
    check.expect(!overCommitted(both, exactlyEnough), "exactly 12 GB fits");
    check.expect(overCommitted(both, oneByteShort), "one byte less does not");
    check.expect(overCommitted(both, tooLittle), "11 GB does not");
    // What the folders already use counts as room.
    const std::vector<FolderBudget> used{{4 * gigabyte, 2 * gigabyte, 2 * gigabyte},
                                         {5 * gigabyte, 3 * gigabyte, 1 * gigabyte}};
    check.expect(!overCommitted(used, enoughWithUsed), "used bytes are room: 9 + 3 used = 12");
    check.expect(overCommitted(used, enoughWithUsed - 1), "one byte less does not");
    // The highest floor counts once, not the sum of the floors.
    const std::vector<FolderBudget> floors{{1 * gigabyte, 2 * gigabyte, 0},
                                           {1 * gigabyte, 2 * gigabyte, 0}};
    check.expect(!overCommitted(floors, 4 * gigabyte) && overCommitted(floors, 4 * gigabyte - 1),
                 "two floors of 2 GB count as 2 GB");
    check.expect(!overCommitted({}, 0), "no folder, no problem");
}

void checkPaths(Check& check) {
    std::error_code error;
    const fs::path base{fs::temp_directory_path(error)};
    const std::string path{base.string()};
    check.expect(volumeFreeBytes(path).has_value(), "the free space of a directory is known");
    check.expect(!volumeFreeBytes((base / "no-such-directory-here").string()).has_value(),
                 "a missing path has none");
    check.expect(sameVolume(path, path), "a path is on its own volume");
    check.expect(!sameVolume(path, (base / "no-such-directory-here").string()),
                 "a missing path is on no volume");
    // /proc is its own file system on Linux.
    check.expect(!fs::exists("/proc") || !sameVolume("/proc", path),
                 "paths on different devices are not the same volume");
}
} // namespace

int main() {
    const Logger log{"TEST_VOLUME", LogLevel::Debug};
    Check check{log};
    checkBudgets(check);
    checkPaths(check);
    if (check.ok()) {
        log.info("PASS");
        return EXIT_SUCCESS;
    }
    log.error("FAIL");
    return EXIT_FAILURE;
}
