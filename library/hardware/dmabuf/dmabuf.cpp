#include "dmabuf.h"

#include <cerrno>
#include <cstdint>
#include <string>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

namespace {
constexpr const char* heapDirectory{"/dev/dma_heap/"};
constexpr std::uint64_t syncBegin{DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW};
constexpr std::uint64_t syncEnd{DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW};

// ioctl that retries on EINTR.
int retryIoctl(int descriptor, unsigned long request, void* argument) noexcept {
    int result{ioctl(descriptor, request, argument)};
    while (result == -1 && errno == EINTR) {
        result = ioctl(descriptor, request, argument);
    }
    return result;
}

// A heap name is a file name in /dev/dma_heap, not a path.
bool validHeapName(std::string_view name) noexcept {
    return !name.empty() && name.find('/') == std::string_view::npos;
}
} // namespace

std::optional<DmaBuf> DmaBuf::allocate(const DmaBufConfig& config, const Logger& log) {
    if (!validHeapName(config.heap) || config.size == 0) {
        log.error("bad dma-buf request: heap='", config.heap, "' size=", config.size);
        return std::nullopt;
    }
    const std::string path{std::string{heapDirectory} + std::string{config.heap}};
    const FileDescriptor heap{open(path.c_str(), O_RDWR | O_CLOEXEC), log};
    if (!heap.valid()) {
        log.error("open ", path, ": ", lastError());
        return std::nullopt;
    }
    dma_heap_allocation_data request{};
    request.len = config.size;
    request.fd_flags = static_cast<std::uint32_t>(O_RDWR | O_CLOEXEC);
    if (retryIoctl(heap.get(), DMA_HEAP_IOCTL_ALLOC, &request) != 0) {
        log.error("DMA_HEAP_IOCTL_ALLOC ", path, " ", config.size, " bytes: ", lastError());
        return std::nullopt;
    }
    FileDescriptor buffer{static_cast<int>(request.fd), log};
    void* address{mmap(nullptr, config.size, PROT_READ | PROT_WRITE, MAP_SHARED, buffer.get(), 0)};
    const std::error_code mapError{lastError()};
    Mapping mapped{address, config.size, log};
    if (!mapped.valid()) {
        log.error("mmap of ", path, " buffer, ", config.size, " bytes: ", mapError);
        return std::nullopt;
    }
    log.debug("allocated ", config.size, " bytes from ", path, ", fd=", buffer.get());
    return std::optional<DmaBuf>{DmaBuf{std::move(buffer), std::move(mapped), config.size, log}};
}

bool DmaBuf::beginCpuAccess() const noexcept {
    return sync(syncBegin);
}

bool DmaBuf::endCpuAccess() const noexcept {
    return sync(syncEnd);
}

bool DmaBuf::sync(std::uint64_t flags) const noexcept {
    dma_buf_sync request{};
    request.flags = flags;
    if (retryIoctl(buffer.get(), DMA_BUF_IOCTL_SYNC, &request) != 0) {
        log.error("DMA_BUF_IOCTL_SYNC fd=", buffer.get(), ": ", lastError());
        return false;
    }
    return true;
}
