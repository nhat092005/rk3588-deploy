#ifndef DMABUF_H
#define DMABUF_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

#include "handles.h"
#include "logger.h"

/**
 * @brief The dma-heap the pipeline allocates from by default.
 *
 * Measured on the board: cached memory, fast CPU access, accepted by RGA, RKNN and MPP. Every CPU
 * access that a device reads or writes needs beginCpuAccess() and endCpuAccess().
 */
constexpr std::string_view defaultHeap{"system"};

/**
 * @brief The dma-heap for buffers that the RGA2 draws on: the CMA region of this kernel, below
 *        4 GB.
 *
 * The RGA2 maps only memory below 4 GB, and a buffer of the "system" heap is built from pages of
 * any zone, so it has pages above 4 GB whenever that zone has free pages (measured on the board:
 * after the page cache was dropped, 0 of 40 system buffers were accepted, and 40 of 40 right
 * before). Buffers of this heap are always accepted. The device node needs the rule of
 * sh/env/install_udev_rules.sh, and the region is small (128 MB), so allocate only what the RGA2
 * draws on.
 */
constexpr std::string_view lowHeap{"reserved"};

/** @brief What to allocate. */
struct DmaBufConfig {
    /** Name of a node in /dev/dma_heap, without a path, for example "system". */
    std::string_view heap{defaultHeap};
    /** Size in bytes, greater than 0. The kernel rounds the buffer up to whole pages. */
    std::size_t size{0};
};

/**
 * @brief One DMA-BUF from a dma-heap with a read-write mapping. Move-only, the destructor unmaps
 *        the memory and closes the descriptor.
 *
 * The descriptor goes to RGA (importbuffer_fd), RKNN (rknn_create_mem_from_fd) and MPP
 * (mpp_buffer_import with MPP_BUFFER_TYPE_DRM). The CPU follows the DMA-BUF rules: call
 * endCpuAccess() after the CPU writes and before a device reads, and beginCpuAccess() before the
 * CPU reads what a device wrote.
 */
class DmaBuf {
public:
    /**
     * @brief Allocates a buffer from a dma-heap and maps it.
     * @param config Heap name and size. A heap name that is empty or contains '/' is rejected.
     * @param log Logger of the buffer. It reports a failure to sync, unmap or close later.
     * @return The buffer, or empty on failure, which is logged once with the cause.
     */
    static std::optional<DmaBuf> allocate(const DmaBufConfig& config, const Logger& log);

    /** @brief The DMA-BUF descriptor to hand to a device API. Owned by this object. */
    int fd() const noexcept {
        return buffer.get();
    }

    /** @brief The requested size in bytes (the mapping has this length). */
    std::size_t size() const noexcept {
        return length;
    }

    /** @brief Start of the mapping, for the CPU to read. Never null. */
    const void* data() const noexcept {
        return mapped.data();
    }

    /** @brief Start of the mapping, for the CPU to write. Never null. */
    void* data() noexcept {
        return mapped.data();
    }

    /**
     * @brief Starts a CPU access: DMA_BUF_IOCTL_SYNC with START, read and write.
     * @return False when the ioctl fails, which is logged.
     *
     * Call it before the CPU reads what a device wrote.
     */
    [[nodiscard]] bool beginCpuAccess() const noexcept;

    /**
     * @brief Ends a CPU access: DMA_BUF_IOCTL_SYNC with END, read and write.
     * @return False when the ioctl fails, which is logged.
     *
     * Call it after the CPU wrote, before a device reads.
     */
    [[nodiscard]] bool endCpuAccess() const noexcept;

private:
    DmaBuf(FileDescriptor&& owned, Mapping&& region, std::size_t byteCount, Logger logger)
        : buffer{std::move(owned)}, mapped{std::move(region)}, length{byteCount}, log{logger} {}

    [[nodiscard]] bool sync(std::uint64_t flags) const noexcept;

    // The mapping is declared after the descriptor, so it is released first.
    FileDescriptor buffer;
    Mapping mapped;
    std::size_t length{0};
    Logger log;
};

#endif // DMABUF_H
