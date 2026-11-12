// One RKNN model, repeated inference, optional raw input and output dump.
// Usage: probe_infer model.rknn loops [input.raw output.f32]
//   input.raw : uint8 NHWC 1x640x640x3, same bytes as the Python preprocess. Default: fixed random.
//   output.f32: first output of the first run, float32 as returned by want_float=1.
// Prints per-run wall time of rknn_run (not a benchmark: frequencies are not locked) and RSS.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "rknn_api.h"

namespace {
constexpr int IMG_SIZE = 640;
constexpr int IMG_CHANNELS = 3;
constexpr int WARMUP_RUNS = 10;
constexpr int RSS_PROBE_RUN = 20;
constexpr int FIXED_SEED = 42;
constexpr int BYTE_MASK = 0xFF;
constexpr std::size_t VMRSS_PREFIX_LEN = 6;

long rssKb() {
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        if (line.rfind("VmRSS:", 0) == 0) {
            return std::atol(line.c_str() + VMRSS_PREFIX_LEN);
        }
    }
    return -1;
}
} // namespace

int main(int argc, char** argv) {
    constexpr int ARGS_MIN = 3;
    constexpr int ARGS_DUMP = 5;
    if (argc < ARGS_MIN) {
        std::fprintf(stderr, "usage: %s model.rknn loops [input.raw output.f32]\n", argv[0]);
        return 2;
    }
    std::ifstream modelFile(argv[1], std::ios::binary);
    std::vector<char> blob((std::istreambuf_iterator<char>(modelFile)),
                           std::istreambuf_iterator<char>());
    const int loops = std::atoi(argv[2]);

    rknn_context ctx = 0;
    int ret = rknn_init(&ctx, blob.data(), static_cast<uint32_t>(blob.size()), 0, nullptr);
    std::printf("init ret=%d\n", ret);
    if (ret != 0) {
        return 1;
    }

    rknn_sdk_version version;
    rknn_query(ctx, RKNN_QUERY_SDK_VERSION, &version, sizeof(version));
    std::printf("api=%s drv=%s\n", version.api_version, version.drv_version);

    rknn_input_output_num ioNum;
    rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &ioNum, sizeof(ioNum));
    std::printf("n_input=%u n_output=%u\n", ioNum.n_input, ioNum.n_output);

    std::vector<unsigned char> image(static_cast<std::size_t>(IMG_SIZE) * IMG_SIZE * IMG_CHANNELS);
    std::srand(FIXED_SEED);
    for (auto& pixel : image) {
        pixel = static_cast<unsigned char>(std::rand() & BYTE_MASK);
    }
    if (argc >= ARGS_DUMP) {
        std::ifstream inputFile(argv[3], std::ios::binary);
        inputFile.read(reinterpret_cast<char*>(image.data()),
                       static_cast<std::streamsize>(image.size()));
        if (static_cast<std::size_t>(inputFile.gcount()) != image.size()) {
            std::fprintf(stderr, "bad input file\n");
            return 2;
        }
    }

    rknn_input input;
    std::memset(&input, 0, sizeof(input));
    input.index = 0;
    input.type = RKNN_TENSOR_UINT8;
    input.fmt = RKNN_TENSOR_NHWC;
    input.size = static_cast<uint32_t>(image.size());
    input.buf = image.data();

    long rssEarly = -1;
    double sumMs = 0;
    double maxMs = 0;
    int bad = 0;
    for (int run = 0; run < loops && bad == 0; ++run) {
        if (rknn_inputs_set(ctx, 1, &input) != 0) {
            ++bad;
            break;
        }
        const auto start = std::chrono::steady_clock::now();
        ret = rknn_run(ctx, nullptr);
        const auto stop = std::chrono::steady_clock::now();
        if (ret != 0) {
            std::printf("run ret=%d\n", ret);
            ++bad;
            break;
        }
        std::vector<rknn_output> outputs(ioNum.n_output);
        std::memset(outputs.data(), 0, outputs.size() * sizeof(rknn_output));
        for (auto& output : outputs) {
            output.want_float = 1;
        }
        if (rknn_outputs_get(ctx, ioNum.n_output, outputs.data(), nullptr) != 0) {
            ++bad;
            break;
        }
        if (run == 0 && argc >= ARGS_DUMP) {
            std::ofstream dump(argv[4], std::ios::binary);
            dump.write(static_cast<const char*>(outputs[0].buf),
                       static_cast<std::streamsize>(outputs[0].size));
        }
        rknn_outputs_release(ctx, ioNum.n_output, outputs.data());

        const double elapsedMs = std::chrono::duration<double, std::milli>(stop - start).count();
        if (run >= WARMUP_RUNS) {
            sumMs += elapsedMs;
            maxMs = elapsedMs > maxMs ? elapsedMs : maxMs;
        }
        if (run == RSS_PROBE_RUN) {
            rssEarly = rssKb();
        }
    }
    const int timed = loops > WARMUP_RUNS ? loops - WARMUP_RUNS : 1;
    std::printf("loops=%d bad=%d avg_run_ms=%.2f max_run_ms=%.2f rss_run%d=%ldkB rss_end=%ldkB\n",
                loops, bad, sumMs / timed, maxMs, RSS_PROBE_RUN, rssEarly, rssKb());
    rknn_destroy(ctx);
    return bad == 0 ? 0 : 1;
}
