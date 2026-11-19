// RGA colour fill on buffers of a dma-heap: imfill of a 640x640 RGB888 buffer (the padding of the
// NPU input), with many buffers alive at once so that they come from different pages.
// Usage: probe_fill_heap <heap> [count]    heap: system, system-uncached or reserved, count: 40
// The RGA2 does the fill and maps only memory below 4 GB (see lowHeap in dmabuf.h), so a buffer
// with a page above 4 GB is refused by the kernel with "no core match".
// Exit code 0 when every fill works, 1 when one fails, 2 on bad arguments.
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

#include "dmabuf.h"
#include "im2d.hpp"
#include "logger.h"
#include "rga.h"

namespace {
constexpr int defaultCount{40};
constexpr int maxCount{1000};
constexpr int fillSide{640};
constexpr int rgbBytes{3};
constexpr int grayFill{static_cast<int>(0xff727272U)};
constexpr int usageArguments{2};

struct Settings {
    std::string heap;
    int count{defaultCount};
};

std::optional<Settings> parseSettings(int argc, char** argv) {
    if (argc < 2 || argc > 3) {
        return std::nullopt;
    }
    Settings settings;
    settings.heap = argv[1];
    if (argc == 3) {
        settings.count = std::atoi(argv[2]);
    }
    if (settings.count < 1 || settings.count > maxCount) {
        return std::nullopt;
    }
    return settings;
}
} // namespace

int main(int argc, char** argv) {
    const Logger log{"PROBE_FILL_HEAP", LogLevel::Info};
    const std::optional<Settings> settings{parseSettings(argc, argv)};
    if (!settings) {
        log.error("usage: probe_fill_heap <heap> [count 1..", maxCount, "]");
        return usageArguments;
    }
    const std::size_t bytes{static_cast<std::size_t>(fillSide) * fillSide * rgbBytes};
    std::vector<DmaBuf> held;
    std::vector<RgaImage> images;
    held.reserve(static_cast<std::size_t>(settings->count));
    images.reserve(static_cast<std::size_t>(settings->count));
    int failed{0};
    for (int index{0}; index < settings->count; ++index) {
        std::optional<DmaBuf> buffer{DmaBuf::allocate(DmaBufConfig{settings->heap, bytes}, log)};
        if (!buffer) {
            return 1;
        }
        held.push_back(std::move(*buffer));
        std::optional<RgaImage> image{RgaImage::import(held.back(), log)};
        if (!image) {
            ++failed;
            continue;
        }
        const rga_buffer_t target{
            wrapbuffer_handle(image->handle(), fillSide, fillSide, RK_FORMAT_RGB_888)};
        const im_rect whole{0, 0, fillSide, fillSide};
        if (imfill_t(target, whole, grayFill, 1) != IM_STATUS_SUCCESS) {
            ++failed;
        }
        images.push_back(std::move(*image));
    }
    log.info("heap ", settings->heap, ": ", settings->count - failed, " fills ok, ", failed,
             " failed of ", settings->count);
    log.info(failed == 0 ? "PASS" : "FAIL");
    return failed == 0 ? 0 : 1;
}
