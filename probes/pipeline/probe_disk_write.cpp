// Write latency of the disk under the load of a video recording: one packet of a made-up H.265
// stream every frame (at the given bitrate and 30 fps, a larger one for every key frame), written
// to segment files that are flushed and renamed when they are full, as a continuous recording
// would. Times every write(), every fsync() and every rename(). Run it next to the application
// to see what the disk does while the pipeline is busy.
// Usage: probe_disk_write <directory> <seconds> <kbps> <segment_seconds>
//   directory       : an existing, writable directory on the disk to test, for example
//                     /data/violations/disktest
//   seconds         : how long to write, 1 or more
//   kbps            : bitrate of the made-up stream, for example 20000
//   segment_seconds : length of a segment file, for example 60
// Logs the number of writes, the median, the 99th percentile and the largest time of a write, how
// many were slower than 10, 50 and 100 ms, and the time of every fsync. The files are removed at
// the end. Exit code 0 when the run ended, 2 on bad arguments.
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include "arguments.h"
#include "handles.h"
#include "logger.h"

namespace {
using Clock = std::chrono::steady_clock;

constexpr int exitUsage{2};
constexpr int decimalBase{10};
constexpr std::size_t directoryArg{1};
constexpr std::size_t secondsArg{2};
constexpr std::size_t kbpsArg{3};
constexpr std::size_t segmentArg{4};
constexpr std::size_t argsCount{5};
constexpr int framesPerSecond{30};
constexpr int gopFrames{30};
// A key frame is this many times the size of an ordinary frame (the rest of the second is shared
// so that the rate stays what was asked for).
constexpr double keyFrameWeight{8.0};
constexpr double bitsPerByte{8.0};
constexpr double bitsPerKilobit{1000.0};
constexpr double millisecondsPerSecond{1000.0};
constexpr double slowMs{10.0};
constexpr double slowerMs{50.0};
constexpr double slowestMs{100.0};
constexpr double percentile99{0.99};
constexpr double median50{0.5};
constexpr int timePrecision{2};
constexpr unsigned fileMode{0644};

struct Options {
    const char* directory{nullptr};
    long seconds{0};
    long kbps{0};
    long segmentSeconds{0};
};

std::optional<long> parseNumber(const char* text, long low, long high) {
    char* end{nullptr};
    const long value{std::strtol(text, &end, decimalBase)};
    if (end == text || *end != '\0' || value < low || value > high) {
        return std::nullopt;
    }
    return value;
}

std::optional<Options> parseArgs(const std::vector<const char*>& args, const Logger& log) {
    if (args.size() != argsCount) {
        log.error("usage: ", args.empty() ? "probe_disk_write" : args.at(0),
                  " <directory> <seconds> <kbps> <segment_seconds>");
        return std::nullopt;
    }
    const std::optional<long> seconds{parseNumber(args.at(secondsArg), 1, 86'400)};
    const std::optional<long> kbps{parseNumber(args.at(kbpsArg), 1, 200'000)};
    const std::optional<long> segment{parseNumber(args.at(segmentArg), 1, 3'600)};
    if (!seconds || !kbps || !segment) {
        log.error("bad seconds, kbps or segment_seconds");
        return std::nullopt;
    }
    return Options{args.at(directoryArg), *seconds, *kbps, *segment};
}

double millisecondsBetween(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

struct Times {
    std::vector<double> writes;
    std::vector<double> syncs;
    std::vector<double> renames;
};

// Writes every byte of the buffer. false on an error.
bool writeAll(int descriptor, const std::vector<std::uint8_t>& bytes, std::size_t size) {
    std::size_t done{0};
    while (done < size) {
        const ssize_t count{::write(descriptor, bytes.data() + done, size - done)};
        if (count < 0) {
            return false;
        }
        done += static_cast<std::size_t>(count);
    }
    return true;
}

// Size of the frame number `number` of a second: a key frame first, then ordinary frames.
std::size_t frameBytes(const Options& options, int number) {
    const double perSecond{static_cast<double>(options.kbps) * bitsPerKilobit / bitsPerByte};
    const double ordinary{perSecond / (keyFrameWeight + (gopFrames - 1))};
    return static_cast<std::size_t>(number % gopFrames == 0 ? ordinary * keyFrameWeight : ordinary);
}

// Closes a segment: fsync, then rename from the hidden name to the final one.
bool closeSegment(FileDescriptor& file, const std::string& temp, const std::string& finalPath,
                  Times& times, const Logger& log) {
    const Clock::time_point syncStart{Clock::now()};
    if (::fsync(file.get()) != 0) {
        log.error("fsync failed");
        return false;
    }
    times.syncs.push_back(millisecondsBetween(syncStart, Clock::now()));
    if (file.close()) {
        return false;
    }
    const Clock::time_point renameStart{Clock::now()};
    if (::rename(temp.c_str(), finalPath.c_str()) != 0) {
        log.error("rename failed");
        return false;
    }
    times.renames.push_back(millisecondsBetween(renameStart, Clock::now()));
    return true;
}

double percentile(std::vector<double>& samples, double fraction) {
    std::sort(samples.begin(), samples.end());
    const auto index{static_cast<std::size_t>(static_cast<double>(samples.size() - 1) * fraction)};
    return samples.at(index);
}

std::size_t slowerThan(const std::vector<double>& samples, double limit) {
    return static_cast<std::size_t>(std::count_if(samples.begin(), samples.end(),
                                                  [limit](double value) { return value > limit; }));
}

void report(Times& times, std::uint64_t bytes, const Logger& log) {
    if (times.writes.empty()) {
        return;
    }
    log.info("writes ", times.writes.size(), ", ", bytes, " bytes: median ",
             Fixed{percentile(times.writes, median50), timePrecision}, " ms, p99 ",
             Fixed{percentile(times.writes, percentile99), timePrecision}, " ms, max ",
             Fixed{times.writes.back(), timePrecision}, " ms");
    log.info("writes slower than ", Fixed{slowMs, 0}, " ms: ", slowerThan(times.writes, slowMs),
             ", ", Fixed{slowerMs, 0}, " ms: ", slowerThan(times.writes, slowerMs), ", ",
             Fixed{slowestMs, 0}, " ms: ", slowerThan(times.writes, slowestMs));
    for (const double value : times.syncs) {
        log.info("fsync of a segment: ", Fixed{value, timePrecision}, " ms");
    }
    for (const double value : times.renames) {
        log.info("rename of a segment: ", Fixed{value, timePrecision}, " ms");
    }
}

bool run(const Options& options, const Logger& log) {
    const std::string directory{options.directory};
    const std::vector<std::uint8_t> bytes(frameBytes(options, 0) + 1, 0x5A);
    Times times;
    std::uint64_t written{0};
    std::vector<std::string> finals;
    FileDescriptor file;
    std::string temp;
    std::string finalPath;
    const auto frameStep{std::chrono::duration_cast<Clock::duration>(
        std::chrono::duration<double>(1.0 / framesPerSecond))};
    Clock::time_point next{Clock::now()};
    const long totalFrames{options.seconds * framesPerSecond};
    const long segmentFrames{options.segmentSeconds * framesPerSecond};
    for (long frame{0}; frame < totalFrames; ++frame) {
        if (frame % segmentFrames == 0) {
            const std::string name{std::to_string(frame / segmentFrames)};
            temp = directory;
            temp += "/.rec_";
            temp += name;
            temp += ".tmp";
            finalPath = directory;
            finalPath += "/rec_";
            finalPath += name;
            finalPath += ".h265";
            file = FileDescriptor{
                ::open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, fileMode), log};
            if (!file.valid()) {
                log.error("cannot create ", temp);
                return false;
            }
        }
        const std::size_t size{frameBytes(options, static_cast<int>(frame))};
        const Clock::time_point start{Clock::now()};
        if (!writeAll(file.get(), bytes, std::min(size, bytes.size()))) {
            log.error("write failed");
            return false;
        }
        times.writes.push_back(millisecondsBetween(start, Clock::now()));
        written += size;
        if ((frame + 1) % segmentFrames == 0) {
            if (!closeSegment(file, temp, finalPath, times, log)) {
                return false;
            }
            finals.push_back(finalPath);
        }
        next += frameStep;
        std::this_thread::sleep_until(next);
    }
    if (file.valid() && !closeSegment(file, temp, finalPath, times, log)) {
        return false;
    }
    if (file.valid() || totalFrames % segmentFrames != 0) {
        finals.push_back(finalPath);
    }
    report(times, written, log);
    for (const std::string& path : finals) {
        (void)::unlink(path.c_str());
    }
    return true;
}
} // namespace

int main(int argc, char** argv) {
    const std::vector<const char*> args{commandLine(argc, argv)};
    const Logger log{"PROBE_DISK", LogLevel::Info};
    const std::optional<Options> options{parseArgs(args, log)};
    if (!options) {
        return exitUsage;
    }
    const bool passed{run(*options, log)};
    if (passed) {
        log.info("PASS");
    } else {
        log.error("FAIL");
    }
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
