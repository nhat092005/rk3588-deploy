// The PPE pipeline: camera, RGA, NPU, postprocess, tracker and violation rule on one thread.
// Usage: rk3588_deploy [--model <path>] [flags]   (--help lists the flags)
// The defaults come from resource/config/app_config.json next to the bin/ folder, if it exists.
// With input.source "file" (the demo) the videos of input.path run one after the other, each
// through its own pipeline, and each gives a session file named after the video.
// Exit code 0 after SIGINT, SIGTERM or --frames frames, 1 on a failure, 2 on a bad command line.
#include <poll.h>

#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "app_options.h"
#include "arguments.h"
#include "file_text.h"
#include "input_videos.h"
#include "logger.h"
#include "pipeline.h"
#include "stream_check.h"

namespace {
constexpr int exitUsage{2};

// SIGINT or SIGTERM is pending. The signal is not read, so this stays true.
bool stopRequested(const StopSignal& stop) {
    pollfd watched{stop.descriptor(), POLLIN, 0};
    return ::poll(&watched, 1, 0) > 0 && (watched.revents & POLLIN) != 0;
}

// The demo: every video of input.path, in order. A video that fails is logged and the next one
// runs. 0 when all of them were done or a signal stopped the run, 1 when one failed.
int runVideos(const AppOptions& options, const StopSignal& stop, const Logger& log) {
    const std::optional<std::vector<std::string>> videos{listInputVideos(options.inputPath, log)};
    if (!videos) {
        return EXIT_FAILURE;
    }
    std::set<std::string> names;
    std::size_t done{0};
    std::size_t failed{0};
    for (std::size_t at{0}; at < videos->size(); ++at) {
        const std::string& video{videos->at(at)};
        const std::string name{std::filesystem::path{video}.stem().string()};
        log.info("video ", at + 1, " of ", videos->size(), ": ", video);
        // Two files with the same name would write the same session file.
        if (!names.insert(name).second) {
            log.error("skipped: another video is named ", name, ", it would replace its output");
            ++failed;
            continue;
        }
        AppOptions each{options};
        each.inputPath = video;
        std::optional<Pipeline> pipeline{Pipeline::create(each, log)};
        if (!pipeline || !pipeline->run(stop)) {
            log.error("video ", video, " failed");
            ++failed;
        } else if (!stopRequested(stop)) {
            ++done;
        }
        if (stopRequested(stop)) {
            log.warn("stopped by signal, ", videos->size() - at - 1, " video(s) not run");
            break;
        }
    }
    log.info("videos: ", done, " done, ", failed, " failed, of ", videos->size());
    return failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
} // namespace

int main(int argc, char** argv) {
    const Logger log{"APP", LogLevel::Info};
    const std::vector<const char*> args{commandLine(argc, argv)};
    // Without the folder of the executable there is no default config file: the defaults of the
    // code and the command line still work.
    const std::string baseDir{executableBaseDirectory().value_or("")};
    const ParseResult parsed{parseOptions(args, baseDir, log)};
    if (parsed.status != ParseStatus::Run) {
        return parsed.status == ParseStatus::Help ? EXIT_SUCCESS : exitUsage;
    }
    // A stream that was asked for must work: better to stop now than to run without it.
    if (parsed.options.stream && !checkStreamReady(parsed.options.streamUrl, log)) {
        return EXIT_FAILURE;
    }
    const std::optional<StopSignal> stop{StopSignal::create(log)};
    if (!stop) {
        return EXIT_FAILURE;
    }
    if (parsed.options.inputSource == InputSource::File) {
        return runVideos(parsed.options, *stop, log);
    }
    std::optional<Pipeline> pipeline{Pipeline::create(parsed.options, log)};
    if (!pipeline) {
        return EXIT_FAILURE;
    }
    return pipeline->run(*stop) ? EXIT_SUCCESS : EXIT_FAILURE;
}
