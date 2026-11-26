// RGA sampling offset probe: the app's 4K NV12 to 640x640 letterbox job (rotation 180) with a
// sine pattern, on each core, with the source rectangle moved or shortened by a few pixels.
// Usage: probe_rga_shift OUTDIR    writes OUTDIR/<axis>_<core>_<variant>.rgb (640x640 RGB888) and
// OUTDIR/results.csv (axis, core, variant, status, message). The analysis is done off the board.
// Usage: probe_rga_shift OUTDIR FRAMEDIR    runs every FRAMEDIR/*.nv12 (3840x2160) with R2, R30 and
// R30 cropped by 4 pixels on each axis, writes OUTDIR/<name>_<config>.rgb and OUTDIR/frames.csv.
// Usage: probe_rga_shift --latency [ROUNDS]    times R2, R30, R30 cropped, Ra (kernel's choice) and
// Ra cropped on one frame, back to back, 300 runs after 30 warmup per round, the configs
// interleaved over ROUNDS rounds (default 3). Prints one CSV line per config and round. A variant
// "A<n>" is the source rectangle x=n, width=W-n; "B<n>" is x=0, width=W-n (the same for y and the
// height on the y axis).
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "dmabuf.h"
#include "im2d.hpp"
#include "logger.h"
#include "rga.h"

namespace {
constexpr int frameWidth{3840};
constexpr int frameHeight{2160};
constexpr int modelSide{640};
constexpr int scaledHeight{360};
constexpr int paddingTop{140};
constexpr double period{120.0};
constexpr double pi{3.14159265358979323846};
constexpr int noFence{-1};

struct Core {
    const char* name;
    int mask;
};
const std::array<Core, 4> cores{{{"R2", IM_SCHEDULER_RGA2_CORE0},
                                 {"R30", IM_SCHEDULER_RGA3_CORE0},
                                 {"R31", IM_SCHEDULER_RGA3_CORE1},
                                 {"Ra", IM_SCHEDULER_DEFAULT}}};
constexpr std::array<int, 10> offsets{0, 1, 2, 3, 4, 6, 8, 12, 16, 24};

struct Variant {
    std::string name;
    int shift;
    bool moveStart;
};

std::vector<Variant> variants() {
    std::vector<Variant> list;
    for (const int shift : offsets) {
        list.push_back({"A" + std::to_string(shift), shift, true});
        if (shift > 0) {
            list.push_back({"B" + std::to_string(shift), shift, false});
        }
    }
    return list;
}

std::optional<DmaBuf> make(std::size_t bytes, const Logger& log) {
    return DmaBuf::allocate(DmaBufConfig{lowHeap, bytes}, log);
}

void fillSource(DmaBuf& buffer, bool alongX) {
    auto* bytes{static_cast<std::uint8_t*>(buffer.data())};
    for (int y{0}; y < frameHeight; ++y) {
        for (int x{0}; x < frameWidth; ++x) {
            const double t{alongX ? x : y};
            bytes[(static_cast<std::size_t>(y) * frameWidth) + x] = static_cast<std::uint8_t>(
                std::lround(128.0 + (100.0 * std::sin(2.0 * pi * t / period))));
        }
    }
    const std::size_t chroma{static_cast<std::size_t>(frameWidth) * frameHeight / 2};
    for (std::size_t i{0}; i < chroma; ++i) {
        bytes[static_cast<std::size_t>(frameWidth) * frameHeight + i] = 128;
    }
}

// One job of the app: the whole frame, or the rectangle, to the letterbox, on the cores of the
// mask.
IM_STATUS letterbox(const RgaImage& source, const RgaImage& target, const im_rect& sourceRect,
                    int mask) {
    const rga_buffer_t input{wrapbuffer_handle(source.handle(), frameWidth, frameHeight,
                                               RK_FORMAT_YCbCr_420_SP, frameWidth, frameHeight)};
    const rga_buffer_t output{
        wrapbuffer_handle(target.handle(), modelSide, modelSide, RK_FORMAT_RGB_888)};
    const im_rect targetRect{0, paddingTop, modelSide, scaledHeight};
    const rga_buffer_t noPattern{};
    const im_rect noRect{};
    im_opt_t options{};
    options.core = mask;
    return improcess(input, output, noPattern, sourceRect, targetRect, noRect, noFence, nullptr,
                     &options, IM_HAL_TRANSFORM_ROT_180 | IM_SYNC);
}

int runFrames(const std::string& outDir, const std::string& frameDir, DmaBuf& source,
              DmaBuf& target, const RgaImage& sourceImage, const RgaImage& targetImage,
              const Logger& log) {
    struct Config {
        const char* name;
        int mask;
        im_rect rect;
    };
    constexpr int crop{4};
    const std::array<Config, 3> configs{
        {{"R2", IM_SCHEDULER_RGA2_CORE0, {0, 0, frameWidth, frameHeight}},
         {"R30", IM_SCHEDULER_RGA3_CORE0, {0, 0, frameWidth, frameHeight}},
         {"R30c4", IM_SCHEDULER_RGA3_CORE0, {crop, crop, frameWidth - crop, frameHeight - crop}}}};
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(frameDir)) {
        if (entry.path().extension() == ".nv12") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    const std::size_t nv12Bytes{static_cast<std::size_t>(frameWidth) * frameHeight * 3 / 2};
    const std::size_t targetBytes{static_cast<std::size_t>(modelSide) * modelSide * 3};
    std::ofstream csv{outDir + "/frames.csv"};
    csv << "frame,config,status,message\n";
    for (const auto& path : files) {
        std::ifstream in{path, std::ios::binary};
        in.read(static_cast<char*>(source.data()), static_cast<std::streamsize>(nv12Bytes));
        if (!in || !source.endCpuAccess()) {
            log.error("cannot read ", path.string());
            return 1;
        }
        for (const Config& config : configs) {
            const IM_STATUS status{letterbox(sourceImage, targetImage, config.rect, config.mask)};
            csv << path.stem().string() << ',' << config.name << ','
                << (status == IM_STATUS_SUCCESS ? "ok" : "fail") << ','
                << (status == IM_STATUS_SUCCESS ? "" : imStrError_t(status)) << '\n';
            if (status == IM_STATUS_SUCCESS) {
                if (!target.beginCpuAccess()) {
                    return 1;
                }
                std::ofstream file{outDir + "/" + path.stem().string() + "_" + config.name + ".rgb",
                                   std::ios::binary};
                file.write(static_cast<const char*>(target.data()),
                           static_cast<std::streamsize>(targetBytes));
            }
        }
    }
    log.info("frames done: ", files.size());
    return 0;
}
constexpr int warmupRuns{30};
constexpr int timedRuns{300};
constexpr int defaultRounds{3};
constexpr double nanosPerMilli{1.0e6};

double cpuSeconds() {
    timespec now{};
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &now);
    return static_cast<double>(now.tv_sec) + (static_cast<double>(now.tv_nsec) * 1.0e-9);
}

int runLatency(int rounds, DmaBuf& source, const RgaImage& sourceImage, const RgaImage& targetImage,
               const Logger& log) {
    struct Config {
        const char* name;
        int mask;
        im_rect rect;
    };
    constexpr int crop{4};
    const im_rect whole{0, 0, frameWidth, frameHeight};
    const im_rect cropped{crop, crop, frameWidth - crop, frameHeight - crop};
    const std::array<Config, 5> configs{{{"R2", IM_SCHEDULER_RGA2_CORE0, whole},
                                         {"R30", IM_SCHEDULER_RGA3_CORE0, whole},
                                         {"R30c4", IM_SCHEDULER_RGA3_CORE0, cropped},
                                         {"Ra", IM_SCHEDULER_DEFAULT, whole},
                                         {"Rac4", IM_SCHEDULER_DEFAULT, cropped}}};
    fillSource(source, true);
    if (!source.endCpuAccess()) {
        return 1;
    }
    std::printf("round,config,p50_ms,p95_ms,p99_ms,mean_ms,cpu_ms_per_frame\n");
    for (int round{0}; round < rounds; ++round) {
        for (const Config& config : configs) {
            std::vector<double> millis;
            millis.reserve(timedRuns);
            for (int index{0}; index < warmupRuns; ++index) {
                if (letterbox(sourceImage, targetImage, config.rect, config.mask) !=
                    IM_STATUS_SUCCESS) {
                    log.error(config.name, ": the job failed");
                    return 1;
                }
            }
            const double cpuStart{cpuSeconds()};
            for (int index{0}; index < timedRuns; ++index) {
                const auto start{std::chrono::steady_clock::now()};
                const IM_STATUS status{
                    letterbox(sourceImage, targetImage, config.rect, config.mask)};
                const auto stop{std::chrono::steady_clock::now()};
                if (status != IM_STATUS_SUCCESS) {
                    log.error(config.name, ": the job failed");
                    return 1;
                }
                millis.push_back(
                    static_cast<double>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start)
                            .count()) /
                    nanosPerMilli);
            }
            const double cpuMillis{(cpuSeconds() - cpuStart) * 1000.0 / timedRuns};
            std::sort(millis.begin(), millis.end());
            double sum{0.0};
            for (const double value : millis) {
                sum += value;
            }
            const auto percentile = [&millis](double fraction) {
                return millis.at(static_cast<std::size_t>(fraction * (millis.size() - 1)));
            };
            std::printf("%d,%s,%.3f,%.3f,%.3f,%.3f,%.3f\n", round, config.name, percentile(0.50),
                        percentile(0.95), percentile(0.99),
                        sum / static_cast<double>(millis.size()), cpuMillis);
        }
    }
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    const Logger log{"PROBE_RGA_SHIFT", LogLevel::Info};
    const bool latency{argc >= 2 && std::string{argv[1]} == "--latency"};
    if (!latency && argc != 2 && argc != 3) {
        log.error("usage: probe_rga_shift OUTDIR [FRAMEDIR] | --latency [ROUNDS]");
        return 2;
    }
    const std::string outDir{latency ? "" : argv[1]};
    const std::size_t nv12Bytes{static_cast<std::size_t>(frameWidth) * frameHeight * 3 / 2};
    const std::size_t targetBytes{static_cast<std::size_t>(modelSide) * modelSide * 3};
    std::optional<DmaBuf> source{make(nv12Bytes, log)};
    std::optional<DmaBuf> target{make(targetBytes, log)};
    if (!source || !target) {
        return 1;
    }
    std::optional<RgaImage> sourceImage{RgaImage::import(*source, log)};
    std::optional<RgaImage> targetImage{RgaImage::import(*target, log)};
    if (!sourceImage || !targetImage) {
        return 1;
    }
    if (latency) {
        return runLatency(argc >= 3 ? std::atoi(argv[2]) : defaultRounds, *source, *sourceImage,
                          *targetImage, log);
    }
    if (argc == 3) {
        return runFrames(outDir, argv[2], *source, *target, *sourceImage, *targetImage, log);
    }
    std::ofstream csv{outDir + "/results.csv"};
    csv << "axis,core,variant,status,message\n";
    for (const bool alongX : {true, false}) {
        fillSource(*source, alongX);
        if (!source->endCpuAccess()) {
            return 1;
        }
        for (const Core& core : cores) {
            for (const Variant& variant : variants()) {
                auto* out{static_cast<std::uint8_t*>(target->data())};
                for (std::size_t i{0}; i < targetBytes; ++i) {
                    out[i] = 114;
                }
                if (!target->endCpuAccess()) {
                    return 1;
                }
                im_rect sourceRect{0, 0, frameWidth, frameHeight};
                if (alongX) {
                    sourceRect.x = variant.moveStart ? variant.shift : 0;
                    sourceRect.width = frameWidth - variant.shift;
                } else {
                    sourceRect.y = variant.moveStart ? variant.shift : 0;
                    sourceRect.height = frameHeight - variant.shift;
                }
                const rga_buffer_t input{wrapbuffer_handle(sourceImage->handle(), frameWidth,
                                                           frameHeight, RK_FORMAT_YCbCr_420_SP,
                                                           frameWidth, frameHeight)};
                const rga_buffer_t output{wrapbuffer_handle(targetImage->handle(), modelSide,
                                                            modelSide, RK_FORMAT_RGB_888)};
                const im_rect targetRect{0, paddingTop, modelSide, scaledHeight};
                const rga_buffer_t noPattern{};
                const im_rect noRect{};
                im_opt_t options{};
                options.core = core.mask;
                const IM_STATUS status{improcess(input, output, noPattern, sourceRect, targetRect,
                                                 noRect, noFence, nullptr, &options,
                                                 IM_HAL_TRANSFORM_ROT_180 | IM_SYNC)};
                const char* axisName{alongX ? "x" : "y"};
                csv << axisName << ',' << core.name << ',' << variant.name << ','
                    << (status == IM_STATUS_SUCCESS ? "ok" : "fail") << ','
                    << (status == IM_STATUS_SUCCESS ? "" : imStrError_t(status)) << '\n';
                if (status == IM_STATUS_SUCCESS) {
                    if (!target->beginCpuAccess()) {
                        return 1;
                    }
                    std::ofstream file{outDir + "/" + axisName + "_" + core.name + "_" +
                                           variant.name + ".rgb",
                                       std::ios::binary};
                    file.write(static_cast<const char*>(target->data()),
                               static_cast<std::streamsize>(targetBytes));
                }
            }
        }
    }
    log.info("done");
    return 0;
}
