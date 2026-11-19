// The PPE pipeline: camera, RGA, NPU, postprocess, tracker and violation rule on one thread.
// Usage: rk3588_deploy [--model <path>] [flags]   (--help lists the flags)
// The defaults come from resource/config/app_config.json next to the bin/ folder, if it exists.
// Exit code 0 after SIGINT, SIGTERM or --frames frames, 1 on a failure, 2 on a bad command line.
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

#include "app_options.h"
#include "arguments.h"
#include "file_text.h"
#include "logger.h"
#include "pipeline.h"
#include "stream_check.h"

namespace {
constexpr int exitUsage{2};
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
    std::optional<Pipeline> pipeline{Pipeline::create(parsed.options, log)};
    if (!pipeline) {
        return EXIT_FAILURE;
    }
    return pipeline->run(*stop) ? EXIT_SUCCESS : EXIT_FAILURE;
}
