#include "frame_slot.h"

#include <utility>

std::optional<FrameSlot> makeFrameSlot(std::size_t bytes, std::string_view heap,
                                       const Logger& log) {
    std::optional<DmaBuf> buffer{DmaBuf::allocate(DmaBufConfig{heap, bytes}, log)};
    if (!buffer) {
        return std::nullopt;
    }
    std::optional<RgaImage> image{RgaImage::import(*buffer, log)};
    std::optional<MppSource> source{MppSource::import(*buffer, log)};
    if (!image || !source) {
        return std::nullopt;
    }
    return FrameSlot{std::move(*buffer), std::move(*image), std::move(*source)};
}
