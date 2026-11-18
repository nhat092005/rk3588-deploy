// ViolationStore: the directory layout, the names, the contents of the files, their permissions,
// the suffix for a repeated name, and that a failure leaves nothing behind. Uses a temporary
// directory and fixed times in UTC, so it runs anywhere. Exit code 0 when every check passes.
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include "logger.h"
#include "violation_store.h"

namespace {
namespace fs = std::filesystem;

constexpr int imageWidth{3840};
constexpr int imageHeight{2160};
constexpr int firstTrack{12};
constexpr int secondTrack{13};
constexpr unsigned defaultDirectoryMode{0755};
constexpr unsigned wideMode{0666};
constexpr unsigned wideDirectoryMode{0777};
constexpr unsigned modeMask{0777};
constexpr unsigned readOnlyMode{0555};
constexpr std::uint8_t originalByte{0xA1};
constexpr std::uint8_t markedByte{0xB2};
constexpr std::size_t originalSize{1000};
constexpr std::size_t clipSize{5000};
constexpr std::uint8_t clipByte{0x77};
constexpr std::size_t markedSize{2000};
constexpr int year{2026};
constexpr int yearBase{1900};
constexpr int november{11};
constexpr int dayOfMonth{16};
constexpr int hour{10};
constexpr int minute{15};
constexpr int second{30};
constexpr double startSeconds{12.5};
constexpr double nowSeconds{13.5};
constexpr float boxLeft{100.0F};
constexpr float boxTop{200.0F};
constexpr float boxRight{300.0F};
constexpr float boxBottom{800.0F};

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

// 2026-11-16 10:15:30 UTC plus `extraDays` days.
std::time_t wallTime(int extraDays = 0) {
    std::tm parts{};
    parts.tm_year = year - yearBase;
    parts.tm_mon = november - 1;
    parts.tm_mday = dayOfMonth + extraDays;
    parts.tm_hour = hour;
    parts.tm_min = minute;
    parts.tm_sec = second;
    return timegm(&parts);
}

ViolationEvent eventOf(int trackId, bool noHelmet, bool noVest) {
    ViolationEvent event;
    event.trackId = trackId;
    event.noHelmet = noHelmet;
    event.noVest = noVest;
    event.startSeconds = startSeconds;
    event.nowSeconds = nowSeconds;
    event.left = boxLeft;
    event.top = boxTop;
    event.right = boxRight;
    event.bottom = boxBottom;
    return event;
}

struct Images {
    std::vector<std::uint8_t> original = std::vector<std::uint8_t>(originalSize, originalByte);
    std::vector<std::uint8_t> marked = std::vector<std::uint8_t>(markedSize, markedByte);
};

ViolationRecord recordOf(const ViolationEvent& event, const Images& images, bool withMarked,
                         std::time_t when) {
    ViolationRecord record;
    record.event = event;
    record.wallTime = when;
    record.imageWidth = imageWidth;
    record.imageHeight = imageHeight;
    record.original = StoredBytes{images.original.data(), images.original.size()};
    if (withMarked) {
        record.marked = StoredBytes{images.marked.data(), images.marked.size()};
    }
    return record;
}

std::string readText(const fs::path& path) {
    std::ifstream file{path, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

unsigned modeOf(const fs::path& path) {
    struct stat info {};
    return ::stat(path.c_str(), &info) == 0 ? info.st_mode & modeMask : 0U;
}

bool contains(const std::string& text, const char* part) {
    return text.find(part) != std::string::npos;
}

// Names in a directory that start with a dot: leftovers of an interrupted or failed save.
std::size_t hiddenEntries(const fs::path& directory) {
    std::size_t count{0};
    std::error_code error;
    for (const fs::directory_entry& entry : fs::directory_iterator{directory, error}) {
        count += entry.path().filename().string().front() == '.' ? 1 : 0;
    }
    return count;
}

// A fresh empty directory under the system temporary directory.
std::optional<fs::path> makeRoot() {
    std::error_code error;
    std::string pattern{(fs::temp_directory_path(error) / "violation_store_XXXXXX").string()};
    if (error || mkdtemp(pattern.data()) == nullptr) {
        return std::nullopt;
    }
    return fs::path{pattern};
}

std::optional<ViolationStore> storeAt(const fs::path& root, const Logger& log,
                                      unsigned mode = defaultFileMode) {
    const std::string text{root.string()};
    return ViolationStore::create(StoreConfig{text, mode}, log);
}

void checkCreate(Check& check, const fs::path& root, const Logger& log) {
    check.expect(!storeAt(root / "missing", log), "a missing root is refused");
    const fs::path file{root / "plain_file"};
    std::ofstream{file} << "x";
    check.expect(!storeAt(file, log), "a file as the root is refused");
    check.expect(storeAt(root, log).has_value(), "a writable root is accepted");
    if (geteuid() != 0) {
        const fs::path locked{root / "locked"};
        std::error_code error;
        fs::create_directory(locked, error);
        fs::permissions(locked, fs::perms::owner_read | fs::perms::owner_exec, error);
        check.expect(!storeAt(locked, log), "a root that is not writable is refused");
        fs::permissions(locked, fs::perms::owner_all, error);
    }
}

void checkLayout(Check& check, const fs::path& root, const Logger& log) {
    const std::optional<ViolationStore> store{storeAt(root, log)};
    if (!store) {
        check.expect(false, "store created");
        return;
    }
    const Images images;
    const std::optional<std::string> saved{
        store->save(recordOf(eventOf(firstTrack, true, false), images, true, wallTime()))};
    const fs::path expected{root / "2026-11-16" / "101530_id12_NoHelmet"};
    check.expect(saved && fs::path{*saved} == expected, "the directory is <day>/<time>_id_<kind>");
    if (!saved) {
        return;
    }
    const fs::path directory{*saved};
    const std::string original{readText(directory / "original.jpg")};
    const std::string marked{readText(directory / "marked.jpg")};
    check.expect(original.size() == originalSize &&
                     original == std::string(originalSize, static_cast<char>(originalByte)),
                 "original.jpg holds the original bytes");
    check.expect(marked.size() == markedSize &&
                     marked == std::string(markedSize, static_cast<char>(markedByte)),
                 "marked.jpg holds the marked bytes");
    const std::string json{readText(directory / "event.json")};
    check.expect(contains(json, "\"track_id\": 12") && contains(json, "\"no_helmet\": true") &&
                     contains(json, "\"no_vest\": false"),
                 "event.json: track and kinds");
    check.expect(contains(json, "\"time\": \"2026-11-16T10:15:30+0000\"") &&
                     contains(json, "\"start_seconds\": 12.500") &&
                     contains(json, "\"reported_seconds\": 13.500"),
                 "event.json: times");
    check.expect(contains(json, "\"image\": {\"width\": 3840, \"height\": 2160}") &&
                     contains(json, "\"left\": 100.0, \"top\": 200.0, \"right\": 300.0, "
                                    "\"bottom\": 800.0") &&
                     contains(json, "\"marked\": \"marked.jpg\""),
                 "event.json: image, box, files");
    check.expect(modeOf(directory / "original.jpg") == defaultFileMode &&
                     modeOf(directory / "event.json") == defaultFileMode,
                 "files have the configured mode");
    check.expect(modeOf(directory) == defaultDirectoryMode, "directories get the execute bit");
    check.expect(hiddenEntries(directory.parent_path()) == 0, "no temporary directory is left");
}

void checkClip(Check& check, const fs::path& root, const Logger& log) {
    const std::optional<ViolationStore> store{storeAt(root, log)};
    if (!store) {
        check.expect(false, "store created for the clip");
        return;
    }
    const Images images;
    const std::optional<std::string> saved{
        store->save(recordOf(eventOf(firstTrack + 1, false, true), images, false, wallTime()))};
    if (!saved) {
        check.expect(false, "violation saved for the clip");
        return;
    }
    const std::vector<std::uint8_t> clip(clipSize, clipByte);
    const StoredBytes bytes{clip.data(), clip.size()};
    check.expect(store->saveClip(*saved, bytes), "the clip is saved");
    const fs::path path{fs::path{*saved} / "clip.h265"};
    check.expect(readText(path) == std::string(clipSize, static_cast<char>(clipByte)),
                 "clip.h265 holds the clip bytes");
    check.expect(modeOf(path) == defaultFileMode, "the clip has the configured mode");
    check.expect(hiddenEntries(*saved) == 0, "no temporary clip file is left");
    check.expect(!store->saveClip(*saved, bytes),
                 "a second clip for the same violation is refused");
    const std::optional<std::string> other{
        store->save(recordOf(eventOf(firstTrack + 2, true, true), images, false, wallTime()))};
    check.expect(other && !store->saveClip(*other, StoredBytes{}), "an empty clip is refused");
    check.expect(other && !fs::exists(fs::path{*other} / "clip.h265"), "and leaves no file");
    const std::string gone{(root / "no-such-directory").string()};
    check.expect(!store->saveClip(gone, bytes), "a missing directory is refused");
}

void checkNames(Check& check, const fs::path& root, const Logger& log) {
    const std::optional<ViolationStore> store{storeAt(root, log)};
    if (!store) {
        check.expect(false, "store created");
        return;
    }
    const Images images;
    const ViolationRecord same{
        recordOf(eventOf(firstTrack, true, false), images, false, wallTime())};
    const std::optional<std::string> first{store->save(same)};
    const std::optional<std::string> second{store->save(same)};
    const std::optional<std::string> third{store->save(same)};
    check.expect(first && second && third &&
                     fs::path{*second}.filename() == "101530_id12_NoHelmet_2" &&
                     fs::path{*third}.filename() == "101530_id12_NoHelmet_3",
                 "a repeated name gets _2 and _3");
    const std::optional<std::string> vest{
        store->save(recordOf(eventOf(secondTrack, false, true), images, false, wallTime()))};
    const std::optional<std::string> both{
        store->save(recordOf(eventOf(secondTrack, true, true), images, false, wallTime()))};
    check.expect(vest && fs::path{*vest}.filename() == "101530_id13_NoVest", "kind NoVest");
    check.expect(both && fs::path{*both}.filename() == "101530_id13_NoHelmetNoVest",
                 "kind NoHelmetNoVest");
    if (first) {
        const fs::path directory{*first};
        check.expect(!fs::exists(directory / "marked.jpg"), "no marked.jpg when there is none");
        check.expect(contains(readText(directory / "event.json"), "\"marked\": null"),
                     "event.json says marked is null");
    }
}

void checkRefusals(Check& check, const fs::path& root, const Logger& log) {
    const std::optional<ViolationStore> store{storeAt(root, log)};
    if (!store) {
        check.expect(false, "store created");
        return;
    }
    const Images images;
    const std::time_t quietDay{wallTime(1)};
    const ViolationRecord record{
        recordOf(eventOf(firstTrack, true, false), images, false, quietDay)};
    const ViolationRecord noFlag{
        recordOf(eventOf(firstTrack, false, false), images, false, quietDay)};
    ViolationRecord empty{record};
    empty.original = StoredBytes{};
    ViolationRecord noSize{record};
    noSize.imageWidth = 0;
    check.expect(!store->save(noFlag) && !store->save(empty) && !store->save(noSize),
                 "a record without a kind, an original or an image size is refused");
    check.expect(!fs::exists(root / "2026-11-17"), "a refused record creates nothing");
}

void checkFailure(Check& check, const fs::path& root, const Logger& log) {
    if (geteuid() == 0) {
        return;
    }
    const std::optional<ViolationStore> store{storeAt(root, log)};
    if (!store) {
        check.expect(false, "store created");
        return;
    }
    const Images images;
    std::error_code error;
    fs::permissions(root, fs::perms::owner_read | fs::perms::owner_exec, error);
    const std::optional<std::string> saved{
        store->save(recordOf(eventOf(firstTrack, true, false), images, false, wallTime(2)))};
    fs::permissions(root, fs::perms::owner_all, error);
    check.expect(!saved, "a save into a read-only root fails");
    check.expect(!fs::exists(root / "2026-11-18"), "a failed save leaves no directory");
}

void checkMode(Check& check, const fs::path& root, const Logger& log) {
    const std::optional<ViolationStore> store{storeAt(root, log, wideMode)};
    if (!store) {
        check.expect(false, "store created");
        return;
    }
    const Images images;
    const std::optional<std::string> saved{
        store->save(recordOf(eventOf(firstTrack, true, false), images, false, wallTime(3)))};
    check.expect(saved && modeOf(fs::path{*saved} / "original.jpg") == wideMode &&
                     modeOf(fs::path{*saved}) == wideDirectoryMode,
                 "mode 0666 is applied whatever the umask, directories get 0777");
}
// A fresh root for one check, removed afterwards.
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
} // namespace

int main() {
    const Logger log{"TEST_STORE", LogLevel::Debug};
    setenv("TZ", "UTC", 1);
    tzset();
    Check check{log};
    withRoot(check, log, checkCreate);
    withRoot(check, log, checkLayout);
    withRoot(check, log, checkNames);
    withRoot(check, log, checkClip);
    withRoot(check, log, checkRefusals);
    withRoot(check, log, checkFailure);
    withRoot(check, log, checkMode);
    if (check.ok()) {
        log.info("PASS");
        return EXIT_SUCCESS;
    }
    log.error("FAIL");
    return EXIT_FAILURE;
}
