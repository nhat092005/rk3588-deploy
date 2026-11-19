// StreamWorker and ChildProcess with stand-in programs instead of ffmpeg: the packets arrive
// whole and in order, the child starts with no blocked signals, and a child that ends, does not
// start or does not read gives up the stream with the application still alive (SIGPIPE ignored),
// and stop() ends a write that waits on a child that does not read. Plain C++ and POSIX, runs on
// the PC. Exit code 0 when every check passes.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <pthread.h>

#include "logger.h"
#include "stream_worker.h"

namespace fs = std::filesystem;

namespace {
constexpr int packetCount{20};
constexpr std::size_t packetBytes{1'000};
constexpr std::size_t bigPacketBytes{300'000};
constexpr int videoKbps{20'000};
constexpr int shortStallMs{300};
constexpr int longStallMs{60'000};
constexpr std::chrono::milliseconds step{20};
constexpr std::chrono::seconds patience{10};
constexpr std::chrono::seconds stopLimit{5};
constexpr std::chrono::milliseconds waitBeforeStop{500};
constexpr std::size_t sigPipeBit{12}; // SIGPIPE is signal 13: bit 12 of the mask
constexpr int hexBase{16};

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
    std::string pattern{(fs::temp_directory_path(error) / "stream_worker_XXXXXX").string()};
    if (error || mkdtemp(pattern.data()) == nullptr) {
        return std::nullopt;
    }
    return fs::path{pattern};
}

std::string readFile(const fs::path& path) {
    std::ifstream file{path, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

// The largest packet and the stall time of a worker.
struct Sizes {
    std::size_t packetBytes;
    int stallMs;
};

StreamConfig configOf(std::vector<std::string> command, const Sizes& sizes) {
    StreamConfig config;
    config.command = std::move(command);
    config.kbps = videoKbps;
    config.maxPacketBytes = sizes.packetBytes;
    config.stallMs = sizes.stallMs;
    return config;
}

StreamWorker makeWorker(std::vector<std::string> command, const Sizes& sizes, const Logger& log) {
    std::optional<StreamWorker> worker{
        StreamWorker::create(configOf(std::move(command), sizes), log)};
    if (!worker) {
        std::abort();
    }
    return std::move(*worker);
}

std::vector<std::string> shell(const std::string& script) {
    return {"sh", "-c", script};
}

template <typename Condition> bool waitFor(Condition done, std::chrono::seconds limit) {
    const auto deadline{std::chrono::steady_clock::now() + limit};
    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(step);
    }
    return true;
}

std::string packetText(int index) {
    return std::string(packetBytes, static_cast<char>('a' + index));
}

void checkCommand(Check& check) {
    const std::vector<std::string> command{ffmpegPushCommand("rtsp://host:1/x")};
    const auto has = [&command](const std::string& word) {
        return std::find(command.begin(), command.end(), word) != command.end();
    };
    check.expect(command.front() == "ffmpeg" && command.back() == "rtsp://host:1/x",
                 "program first, address last");
    check.expect(has("-use_wallclock_as_timestamps"), "packets stamped on arrival");
    check.expect(has("copy") && has("tcp") && has("hevc"), "copy, TCP, H.265 input");
}

void checkCreate(Check& check, const Logger& log) {
    check.expect(!StreamWorker::create(configOf({}, Sizes{packetBytes, shortStallMs}), log),
                 "an empty command is refused");
    check.expect(!StreamWorker::create(configOf(shell("true"), Sizes{0, shortStallMs}), log),
                 "a zero packet size is refused");
    check.expect(!StreamWorker::create(configOf(shell("true"), Sizes{packetBytes, 0}), log),
                 "a zero stall time is refused");
}

// Pushes packets until the stream is given up or the time is out.
bool pushUntilFailed(StreamWorker& worker, std::size_t size) {
    const std::vector<std::uint8_t> bytes(size, 1);
    return waitFor(
        [&] {
            (void)worker.queue().push(bytes.data(), bytes.size(), true, 0.0);
            return worker.counts().failed.load();
        },
        patience);
}

void checkDelivery(Check& check, const fs::path& root, const Logger& log) {
    const fs::path out{root / "out.bin"};
    StreamWorker worker{
        makeWorker(shell("cat > " + out.string()), Sizes{packetBytes, longStallMs}, log)};
    std::thread thread{[&worker] { worker.run(); }};
    std::string expected;
    for (int index = 0; index < packetCount; ++index) {
        const std::string text{packetText(index)};
        const bool pushed{worker.queue().push(reinterpret_cast<const std::uint8_t*>(text.data()),
                                              text.size(), index == 0, index * 0.1)};
        check.expect(pushed, "push accepted");
        expected += text;
        std::this_thread::sleep_for(step);
    }
    check.expect(
        waitFor([&] { return worker.counts().packetsSent.load() == packetCount; }, patience),
        "every packet was sent");
    worker.stop();
    thread.join();
    check.expect(readFile(out) == expected, "the child received the bytes whole and in order");
    check.expect(worker.counts().bytesSent.load() ==
                     static_cast<std::int64_t>(packetCount * packetBytes),
                 "bytes counted");
    check.expect(!worker.counts().failed.load(), "no failure");
}

// SigBlk and SigIgn of the child, from /proc, with SIGINT and SIGTERM blocked here as in the
// application.
void checkChildSignals(Check& check, const fs::path& root, const Logger& log) {
    const fs::path out{root / "signals.txt"};
    sigset_t blocked;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGINT);
    sigaddset(&blocked, SIGTERM);
    sigset_t before;
    pthread_sigmask(SIG_BLOCK, &blocked, &before);
    StreamWorker worker{
        makeWorker(shell("grep -E '^Sig(Blk|Ign)' /proc/self/status > " + out.string()),
                   Sizes{packetBytes, longStallMs}, log)};
    std::thread thread{[&worker] { worker.run(); }};
    thread.join();
    pthread_sigmask(SIG_SETMASK, &before, nullptr);
    std::ifstream file{out};
    std::string name;
    std::string value;
    bool blockedClear{false};
    bool pipeDefault{false};
    while (file >> name >> value) {
        const std::uint64_t mask{std::stoull(value, nullptr, hexBase)};
        if (name == "SigBlk:") {
            blockedClear = mask == 0;
        } else if (name == "SigIgn:") {
            pipeDefault = ((mask >> sigPipeBit) & 1U) == 0;
        }
    }
    check.expect(blockedClear, "the child has no blocked signals");
    check.expect(pipeDefault, "the child has the default SIGPIPE");
}

void checkGivesUp(Check& check, const Logger& log) {
    {
        StreamWorker worker{makeWorker(shell("exit 3"), Sizes{packetBytes, longStallMs}, log)};
        std::thread thread{[&worker] { worker.run(); }};
        check.expect(pushUntilFailed(worker, packetBytes),
                     "a child that ended gives up the stream (and SIGPIPE did not kill us)");
        thread.join();
        const std::vector<std::uint8_t> bytes(packetBytes, 1);
        check.expect(!worker.queue().push(bytes.data(), bytes.size(), true, 0.0),
                     "pushes are refused after the stream was given up");
    }
    {
        StreamWorker worker{makeWorker({"no_such_program_for_the_stream_test"},
                                       Sizes{packetBytes, longStallMs}, log)};
        std::thread thread{[&worker] { worker.run(); }};
        thread.join();
        check.expect(worker.counts().failed.load(), "a program that does not start gives up");
    }
    {
        StreamWorker worker{
            makeWorker(shell("exec sleep 30"), Sizes{bigPacketBytes, shortStallMs}, log)};
        std::thread thread{[&worker] { worker.run(); }};
        check.expect(pushUntilFailed(worker, bigPacketBytes),
                     "a child that does not read gives up after the stall time");
        thread.join();
    }
}

void checkStopWhileWaiting(Check& check, const Logger& log) {
    StreamWorker worker{
        makeWorker(shell("exec sleep 30"), Sizes{bigPacketBytes, longStallMs}, log)};
    std::thread thread{[&worker] { worker.run(); }};
    const std::vector<std::uint8_t> bytes(bigPacketBytes, 1);
    (void)worker.queue().push(bytes.data(), bytes.size(), true, 0.0);
    (void)worker.queue().push(bytes.data(), bytes.size(), true, 0.0);
    std::this_thread::sleep_for(waitBeforeStop);
    const auto start{std::chrono::steady_clock::now()};
    worker.stop();
    thread.join();
    check.expect(std::chrono::steady_clock::now() - start < stopLimit,
                 "stop() ends a write that waits, and the child is ended");
    check.expect(!worker.counts().failed.load(), "stop is not a failure");
}
} // namespace

int main() {
    const Logger log{"TEST_STREAM_WORKER", LogLevel::Debug};
    Check check{log};
    const std::optional<fs::path> root{makeRoot()};
    if (!root) {
        log.error("FAIL: temporary directory");
        return EXIT_FAILURE;
    }
    checkCommand(check);
    checkCreate(check, log);
    checkDelivery(check, *root, log);
    checkChildSignals(check, *root, log);
    checkGivesUp(check, log);
    checkStopWhileWaiting(check, log);
    std::error_code error;
    fs::remove_all(*root, error);
    if (check.ok()) {
        log.info("PASS");
        return EXIT_SUCCESS;
    }
    log.error("FAIL");
    return EXIT_FAILURE;
}
