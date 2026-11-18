#ifndef FRAME_SLOT_H
#define FRAME_SLOT_H

#include <cstddef>
#include <optional>
#include <string_view>

#include "dmabuf.h"
#include "logger.h"
#include "mpp_source.h"
#include "rga.h"

/**
 * @brief A buffer that the RGA writes, the MPP encoders read, and a thread owns for a while: the
 *        DMA-BUF with its RGA and MPP imports. Move-only through its members.
 */
struct FrameSlot {
    DmaBuf buffer;
    RgaImage image;
    MppSource source;
};

/**
 * @brief Allocates a buffer and imports it into the RGA and the MPP.
 * @param bytes Size in bytes, above 0.
 * @param heap dma-heap name. The RGA2 draws only on memory below 4 GB: use lowHeap for a buffer
 *        that is drawn on.
 * @param log Logger that the buffer keeps.
 * @return The slot, or empty on failure, which is logged.
 */
std::optional<FrameSlot> makeFrameSlot(std::size_t bytes, std::string_view heap, const Logger& log);

#endif // FRAME_SLOT_H
