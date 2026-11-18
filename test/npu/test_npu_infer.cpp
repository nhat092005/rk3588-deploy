// NpuInfer on the board. Usage: test_npu_infer model.rknn
//   model.rknn: a model with a 1x640x640x3 uint8 input.
// Checks: a missing file and a too small buffer are rejected, an unknown input id fails, two runs
// on the same input give identical finite outputs, and logs the time of 100 runs.
// Exit code 0 when every check passes, 1 otherwise, 2 on bad arguments.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <random>
#include <vector>

#include "dmabuf.h"
#include "arguments.h"
#include "logger.h"
#include "npu_infer.h"

namespace {
constexpr std::size_t modelInputBytes{640UL * 640UL * 3UL};
constexpr unsigned int fixedSeed{42};
constexpr unsigned int byteMask{0xFF};
constexpr int timedRuns{100};
constexpr int timePrecision{2};
constexpr int exitUsage{2};
constexpr std::size_t modelArg{1};
constexpr std::size_t argsCount{2};
constexpr std::size_t smallBufferBytes{4096};
constexpr std::uint32_t unknownInput{1000};

bool expect(bool condition, const char* what, const Logger& log) {
    if (!condition) {
        log.error("check failed: ", what);
    }
    return condition;
}

// Fixed pseudo-random bytes in the buffer, flushed for the NPU.
bool fillInput(DmaBuf& buffer) {
    auto* bytes{static_cast<std::uint8_t*>(buffer.data())};
    std::minstd_rand engine{fixedSeed};
    std::generate(bytes, bytes + buffer.size(),
                  [&engine] { return static_cast<std::uint8_t>(engine() & byteMask); });
    return buffer.endCpuAccess();
}

bool allFinite(const std::vector<float>& values) {
    return std::all_of(values.begin(), values.end(),
                       [](float value) { return std::isfinite(value); });
}

bool anyNonZero(const std::vector<float>& values) {
    return std::any_of(values.begin(), values.end(), [](float value) { return value != 0.0F; });
}

// Requests that must be refused, each with its error logged.
bool checkRejects(const char* modelPath, NpuInfer& model, const Logger& log) {
    std::optional<DmaBuf> small{DmaBuf::allocate(DmaBufConfig{defaultHeap, smallBufferBytes}, log)};
    return expect(!NpuInfer::create(NpuConfig{"/nonexistent.rknn"}, log), "missing model rejected",
                  log) &&
           expect(small && !model.attachInput(*small), "too small buffer rejected", log) &&
           expect(!model.run(static_cast<InputId>(unknownInput)), "unknown input id fails", log) &&
           expect(modelPath != nullptr, "model path given", log);
}

bool checkRuns(NpuInfer& model, const Logger& log) {
    std::optional<DmaBuf> buffer{
        DmaBuf::allocate(DmaBufConfig{defaultHeap, model.inputBytes()}, log)};
    if (!buffer || !fillInput(*buffer)) {
        return false;
    }
    const std::optional<InputId> input{model.attachInput(*buffer)};
    if (!input || !model.run(*input)) {
        return false;
    }
    const std::vector<float> first{model.output(0)};
    if (!model.run(*input)) {
        return false;
    }
    bool passed{expect(model.outputCount() >= 1, "at least one output", log) &&
                expect(first == model.output(0), "two runs give the same output", log) &&
                expect(allFinite(first), "output is finite", log) &&
                expect(anyNonZero(first), "output is not all zero", log)};

    const std::chrono::steady_clock::time_point start{std::chrono::steady_clock::now()};
    for (int run{0}; run < timedRuns && passed; ++run) {
        passed = model.run(*input);
    }
    const std::chrono::duration<double, std::milli> elapsed{std::chrono::steady_clock::now() -
                                                            start};
    log.info("NpuInfer::run: ", Fixed{elapsed.count() / timedRuns, timePrecision}, " ms per call");
    return passed;
}
} // namespace

int main(int argc, char** argv) {
    const Logger log{"TEST_NPU", LogLevel::Debug};
    const std::vector<const char*> args{commandLine(argc, argv)};
    if (args.size() != argsCount) {
        log.error("usage: test_npu_infer model.rknn");
        return exitUsage;
    }
    const char* modelPath{args.at(modelArg)};
    std::optional<NpuInfer> model{NpuInfer::create(NpuConfig{modelPath}, log)};
    bool passed{false};
    if (model) {
        const bool sized{
            expect(model->inputBytes() == modelInputBytes, "input is 640x640x3 bytes", log)};
        const bool rejected{checkRejects(modelPath, *model, log)};
        const bool ran{checkRuns(*model, log)};
        passed = sized && rejected && ran;
    }
    if (passed) {
        log.info("PASS");
    } else {
        log.error("FAIL");
    }
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
