// listInputVideos: one file as it is, a folder of .mp4 files in name order (any case of the suffix,
// no other file, no folder), and the failures (missing path, empty folder). Uses a temporary
// folder. Exit code 0 when every check passes.
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include "input_videos.h"
#include "logger.h"

namespace {
namespace fs = std::filesystem;

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

void touch(const fs::path& file) {
    std::ofstream{file} << "x";
}

void checkVideos(Check& check, const fs::path& root, const Logger& quiet) {
    const fs::path folder{root / "demo"};
    fs::create_directories(folder / "nested.mp4");
    touch(folder / "b.mp4");
    touch(folder / "a.mp4");
    touch(folder / "C.MP4");
    touch(folder / "notes.txt");
    touch(folder / "d.mov");
    touch(folder / "e.mp4.part");
    const std::optional<std::vector<std::string>> videos{listInputVideos(folder.string(), quiet)};
    const std::vector<std::string> expected{
        (folder / "C.MP4").string(), (folder / "a.mp4").string(), (folder / "b.mp4").string()};
    check.expect(videos && *videos == expected,
                 "a folder gives its .mp4 files in name order, in any case of the suffix, and "
                 "nothing else");
    const std::optional<std::vector<std::string>> one{
        listInputVideos((folder / "d.mov").string(), quiet)};
    check.expect(one && one->size() == 1 && one->front() == (folder / "d.mov").string(),
                 "a file is taken as it is, whatever its suffix");
    check.expect(!listInputVideos((root / "missing").string(), quiet),
                 "a path that does not exist is refused");
    fs::create_directories(root / "empty");
    touch(root / "empty" / "readme.txt");
    check.expect(!listInputVideos((root / "empty").string(), quiet),
                 "a folder without .mp4 files is refused");
}
} // namespace

int main() {
    const Logger quiet{"VIDEOS", LogLevel::None};
    const Logger report{"TEST_VIDEOS", LogLevel::Debug};
    Check check{report};
    std::error_code error;
    std::string pattern{(fs::temp_directory_path(error) / "input_videos_XXXXXX").string()};
    if (error || mkdtemp(pattern.data()) == nullptr) {
        report.error("FAIL: temporary folder");
        return EXIT_FAILURE;
    }
    const fs::path root{pattern};
    checkVideos(check, root, quiet);
    fs::remove_all(root, error);
    if (check.ok()) {
        report.info("PASS");
        return EXIT_SUCCESS;
    }
    return EXIT_FAILURE;
}
