// readTextFile and executableBaseDirectory: a file read whole, a missing file, a folder, a file
// over the size limit, and the folder two levels above the running test. Exit code 0 when every
// check passes.
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>

#include "file_text.h"
#include "logger.h"

namespace fs = std::filesystem;

namespace {
// Enough lines to need several reads of one chunk.
constexpr int manyLines{3000};

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

std::optional<fs::path> makeRoot() {
    std::error_code error;
    std::string pattern{(fs::temp_directory_path(error) / "file_text_XXXXXX").string()};
    if (error || mkdtemp(pattern.data()) == nullptr) {
        return std::nullopt;
    }
    return fs::path{pattern};
}

void write(const fs::path& path, const std::string& text) {
    std::ofstream file{path, std::ios::binary};
    file << text;
}

void checkRead(Check& check, const fs::path& root, const Logger& quiet) {
    const fs::path small{root / "small.json"};
    write(small, "{\"a\": 1}\n");
    const FileText whole{readTextFile(small.string(), quiet)};
    check.expect(whole.status == ReadStatus::Ok && whole.text == "{\"a\": 1}\n",
                 "a file is read whole");

    write(root / "empty", "");
    const FileText empty{readTextFile((root / "empty").string(), quiet)};
    check.expect(empty.status == ReadStatus::Ok && empty.text.empty(), "an empty file");

    // Longer than one read: the chunks are put together in order.
    std::string longText;
    for (int line{0}; line < manyLines; ++line) {
        longText += std::to_string(line) + "\n";
    }
    write(root / "long", longText);
    const FileText longer{readTextFile((root / "long").string(), quiet)};
    check.expect(longer.status == ReadStatus::Ok && longer.text == longText,
                 "a file of several chunks");

    const FileText missing{readTextFile((root / "nothing").string(), quiet)};
    check.expect(missing.status == ReadStatus::Missing, "a missing file");

    const FileText folder{readTextFile(root.string(), quiet)};
    check.expect(folder.status == ReadStatus::Failed, "a folder is not a file");

    write(root / "exact", std::string(maxTextFileBytes, 'x'));
    const FileText exact{readTextFile((root / "exact").string(), quiet)};
    check.expect(exact.status == ReadStatus::Ok && exact.text.size() == maxTextFileBytes,
                 "a file of exactly the limit");

    write(root / "over", std::string(maxTextFileBytes + 1, 'x'));
    const FileText over{readTextFile((root / "over").string(), quiet)};
    check.expect(over.status == ReadStatus::Failed, "one byte over the limit");
}

void checkBase(Check& check) {
    const std::optional<std::string> base{executableBaseDirectory()};
    check.expect(base.has_value(), "the folder of the executable is found");
    if (!base) {
        return;
    }
    // This test is <base>/config/test_file_text.
    std::error_code error;
    check.expect(fs::exists(fs::path{*base} / "config" / "test_file_text", error),
                 "the base folder is two levels above the executable");
    check.expect(!base->empty() && base->back() != '/', "no trailing slash");
}
} // namespace

int main() {
    const Logger log{"TEST_FILE_TEXT", LogLevel::Info};
    const Logger quiet{"TEST_FILE_TEXT", LogLevel::None};
    Check check{log};
    const std::optional<fs::path> root{makeRoot()};
    if (!root) {
        log.error("no temporary folder");
        return EXIT_FAILURE;
    }
    checkRead(check, *root, quiet);
    checkBase(check);
    std::error_code error;
    fs::remove_all(*root, error);
    if (!check.ok()) {
        return EXIT_FAILURE;
    }
    log.info("PASS");
    return EXIT_SUCCESS;
}
