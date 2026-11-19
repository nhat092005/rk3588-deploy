// RGA core check: every RGA job of the pipeline that reads or writes a buffer of the "system"
// heap (the camera frames and the staging buffer), run on the RGA3 only, on the RGA2 only, and
// with the core left to the kernel as the application does it.
// Usage: probe_rga_core [rounds]    rounds: 20 (each round allocates new buffers)
// The RGA2 maps only memory below 4 GB and a "system" buffer may have a page above it (lowHeap in
// dmabuf.h), so a job that only the RGA2 can do fails on such a buffer ("no core match"), while
// a job that the RGA3 can do never does. To see the RGA2 fail, free memory first (a process that
// takes and releases most of it), otherwise the free pages are below 4 GB.
// The jobs follow RgaLetterbox and RgaSnapshot with a camera that is mounted at 180 degrees.
// Exit code 0 when every job passes on the RGA3 and with the kernel's choice, 1 otherwise, 2 on
// bad arguments. The RGA2 line is information.
#include <array>
#include <cstdlib>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "dmabuf.h"
#include "im2d.hpp"
#include "logger.h"
#include "rga.h"

namespace {
constexpr int defaultRounds{20};
constexpr int maxRounds{1000};
constexpr int frameWidth{3840};
constexpr int frameHeight{2160};
constexpr int modelSide{640};
constexpr int scaledHeight{360};
constexpr int paddingTop{140};
constexpr std::size_t nv12Bytes{static_cast<std::size_t>(frameWidth) * frameHeight * 3 / 2};
constexpr std::size_t rgbBytes{static_cast<std::size_t>(frameWidth) * frameHeight * 3};
constexpr std::size_t inputBytes{static_cast<std::size_t>(modelSide) * modelSide * 3};
constexpr int noFence{-1};
constexpr int usageArguments{2};

// What a job may use: the kernel's choice, only the RGA3 (both cores) or only the RGA2.
enum class Cores : std::uint8_t { Kernel, Rga3, Rga2 };

constexpr std::array<Cores, 3> coreChoices{Cores::Kernel, Cores::Rga3, Cores::Rga2};

int coreMask(Cores cores) {
    switch (cores) {
    case Cores::Rga3:
        return IM_SCHEDULER_RGA3_CORE0 | IM_SCHEDULER_RGA3_CORE1;
    case Cores::Rga2:
        return IM_SCHEDULER_RGA2_CORE0;
    case Cores::Kernel:
        break;
    }
    return IM_SCHEDULER_DEFAULT;
}

std::string_view coreName(Cores cores) {
    switch (cores) {
    case Cores::Rga3:
        return "RGA3 only";
    case Cores::Rga2:
        return "RGA2 only";
    case Cores::Kernel:
        break;
    }
    return "kernel's choice";
}

// A buffer and its RGA import, from one heap.
struct Held {
    DmaBuf buffer;
    RgaImage image;
};

std::optional<Held> hold(std::string_view heap, std::size_t bytes, const Logger& log) {
    std::optional<DmaBuf> buffer{DmaBuf::allocate(DmaBufConfig{heap, bytes}, log)};
    if (!buffer) {
        return std::nullopt;
    }
    std::optional<RgaImage> image{RgaImage::import(*buffer, log)};
    if (!image) {
        return std::nullopt;
    }
    return Held{std::move(*buffer), std::move(*image)};
}

// The buffers of one round: where the pipeline keeps them.
struct Round {
    Held camera;  // system heap, NV12, the V4L2 frame
    Held staging; // system heap, RGB888, the full range conversion
    Held input;   // low heap, RGB888, the NPU input
    Held slot;    // low heap, NV12, the upright copy
};

std::optional<Round> makeRound(const Logger& log) {
    std::optional<Held> camera{hold(defaultHeap, nv12Bytes, log)};
    std::optional<Held> staging{hold(defaultHeap, rgbBytes, log)};
    std::optional<Held> input{hold(lowHeap, inputBytes, log)};
    std::optional<Held> slot{hold(lowHeap, nv12Bytes, log)};
    if (!camera || !staging || !input || !slot) {
        return std::nullopt;
    }
    return Round{std::move(*camera), std::move(*staging), std::move(*input), std::move(*slot)};
}

rga_buffer_t nv12Of(const Held& held) {
    return wrapbuffer_handle(held.image.handle(), frameWidth, frameHeight, RK_FORMAT_YCbCr_420_SP,
                             frameWidth, frameHeight);
}

rga_buffer_t rgbOf(const Held& held, int width, int height) {
    return wrapbuffer_handle(held.image.handle(), width, height, RK_FORMAT_RGB_888);
}

bool process(const rga_buffer_t& source, const rga_buffer_t& target, const im_rect& targetRect,
             int usage, Cores cores) {
    im_opt_t options{};
    options.core = coreMask(cores);
    const rga_buffer_t noPattern{};
    const im_rect wholeSource{};
    const im_rect noRect{};
    return improcess(source, target, noPattern, wholeSource, targetRect, noRect, noFence, nullptr,
                     &options, usage | IM_SYNC) == IM_STATUS_SUCCESS;
}

struct Job {
    std::string_view name;
    bool (*run)(const Round&, Cores);
};

constexpr int rotate180{IM_HAL_TRANSFORM_ROT_180};

// RgaLetterbox::convertFullRange: NV12 to RGB888 with the full range matrix.
bool fullRangeConvert(const Round& round, Cores cores) {
    return process(nv12Of(round.camera), rgbOf(round.staging, frameWidth, frameHeight), im_rect{},
                   IM_YUV_TO_RGB_BT601_FULL, cores);
}

// RgaLetterbox::run, limited range source: NV12 scaled into the NPU input.
bool letterboxFromNv12(const Round& round, Cores cores) {
    const im_rect placed{0, paddingTop, modelSide, scaledHeight};
    return process(nv12Of(round.camera), rgbOf(round.input, modelSide, modelSide), placed,
                   rotate180, cores);
}

// RgaLetterbox::run, full range source: the RGB888 staging buffer scaled into the NPU input.
bool letterboxFromRgb(const Round& round, Cores cores) {
    const im_rect placed{0, paddingTop, modelSide, scaledHeight};
    return process(rgbOf(round.staging, frameWidth, frameHeight),
                   rgbOf(round.input, modelSide, modelSide), placed, rotate180, cores);
}

// RgaSnapshot::run: the camera frame copied upright into the writer's slot.
bool snapshotCopy(const Round& round, Cores cores) {
    return process(nv12Of(round.camera), nv12Of(round.slot), im_rect{}, rotate180, cores);
}

constexpr std::array<Job, 4> jobs{{{"full range convert", fullRangeConvert},
                                   {"letterbox from NV12", letterboxFromNv12},
                                   {"letterbox from RGB888", letterboxFromRgb},
                                   {"snapshot copy", snapshotCopy}}};

std::optional<int> parseRounds(int argc, char** argv) {
    if (argc > 2) {
        return std::nullopt;
    }
    const int rounds{argc == 2 ? std::atoi(argv[1]) : defaultRounds};
    if (rounds < 1 || rounds > maxRounds) {
        return std::nullopt;
    }
    return rounds;
}
} // namespace

int main(int argc, char** argv) {
    const Logger log{"PROBE_RGA_CORE", LogLevel::Info};
    const std::optional<int> rounds{parseRounds(argc, argv)};
    if (!rounds) {
        log.error("usage: probe_rga_core [rounds 1..", maxRounds, "]");
        return usageArguments;
    }
    // passes[core][job]
    std::array<std::array<int, jobs.size()>, coreChoices.size()> passes{};
    for (int index{0}; index < *rounds; ++index) {
        const std::optional<Round> round{makeRound(log)};
        if (!round) {
            return 1;
        }
        for (std::size_t choice{0}; choice < coreChoices.size(); ++choice) {
            for (std::size_t job{0}; job < jobs.size(); ++job) {
                if (jobs.at(job).run(*round, coreChoices.at(choice))) {
                    ++passes.at(choice).at(job);
                }
            }
        }
    }
    bool allPass{true};
    for (std::size_t choice{0}; choice < coreChoices.size(); ++choice) {
        for (std::size_t job{0}; job < jobs.size(); ++job) {
            const int passed{passes.at(choice).at(job)};
            log.info(coreName(coreChoices.at(choice)), ", ", jobs.at(job).name, ": ", passed,
                     " of ", *rounds);
            if (coreChoices.at(choice) != Cores::Rga2 && passed != *rounds) {
                allPass = false;
            }
        }
    }
    log.info(allPass ? "PASS" : "FAIL");
    return allPass ? 0 : 1;
}
