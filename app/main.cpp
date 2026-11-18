// The PPE pipeline: camera, RGA, NPU, postprocess, tracker and violation rule on one thread.
// Usage: rk3588_deploy --model <path> [flags]   (--help lists the flags)
// Exit code 0 after SIGINT, SIGTERM or --frames frames, 1 on a failure, 2 on a bad command line.
#include <cstdlib>
#include <optional>
#include <vector>

#include "app_options.h"
#include "arguments.h"
#include "logger.h"
#include "pipeline.h"

namespace {
constexpr int exitUsage{2};
} // namespace

int main(int argc, char** argv) {
    const Logger log{"APP", LogLevel::Info};
    const std::vector<const char*> args{commandLine(argc, argv)};
    const ParseResult parsed{parseOptions(args, log)};
    if (parsed.status != ParseStatus::Run) {
        return parsed.status == ParseStatus::Help ? EXIT_SUCCESS : exitUsage;
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
