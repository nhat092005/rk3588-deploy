// DmaBuf test. Needs /dev/dma_heap/system, so on a machine without it the test prints SKIP and
// exits 0. Exit code 1 when a check fails. Run it on the board.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <utility>

#include <unistd.h>

#include "dmabuf.h"
#include "logger.h"

namespace {
constexpr std::size_t testSize{1'000'000}; // not a multiple of the page size
constexpr std::uint8_t pattern{0x5A};

bool expect(bool condition, const char* what, const Logger& log) {
    if (!condition) {
        log.error("check failed: ", what);
    }
    return condition;
}

bool heapAvailable() {
    return access("/dev/dma_heap/system", R_OK | W_OK) == 0;
}

// Write, sync both ways, read back.
bool checkReadWrite(const Logger& log) {
    std::optional<DmaBuf> buffer{DmaBuf::allocate(DmaBufConfig{defaultHeap, testSize}, log)};
    if (!buffer) {
        return false;
    }
    auto* bytes{static_cast<std::uint8_t*>(buffer->data())};
    std::fill(bytes, bytes + buffer->size(), pattern);
    const bool synced{buffer->endCpuAccess() && buffer->beginCpuAccess()};
    const auto* readBack{static_cast<const std::uint8_t*>(std::as_const(*buffer).data())};
    const bool equal{std::all_of(readBack, readBack + buffer->size(),
                                 [](std::uint8_t byte) { return byte == pattern; })};
    return expect(buffer->size() == testSize, "size is the requested size", log) &&
           expect(buffer->fd() >= 0, "descriptor is valid", log) &&
           expect(synced, "sync succeeds", log) && expect(equal, "bytes read back", log);
}

// A moved buffer keeps working, the source is left empty of ownership.
bool checkMove(const Logger& log) {
    std::optional<DmaBuf> first{DmaBuf::allocate(DmaBufConfig{defaultHeap, testSize}, log)};
    if (!first) {
        return false;
    }
    const int descriptor{first->fd()};
    const DmaBuf second{std::move(*first)};
    return expect(second.fd() == descriptor, "move keeps the descriptor", log) &&
           expect(first->fd() < 0, "moved-from buffer owns no descriptor", log);
}

// Requests that must fail without a crash. Each one logs an error, which is expected here.
bool checkRejects(const Logger& log) {
    return expect(!DmaBuf::allocate(DmaBufConfig{defaultHeap, 0}, log), "size 0 rejected", log) &&
           expect(!DmaBuf::allocate(DmaBufConfig{"", testSize}, log), "empty heap rejected", log) &&
           expect(!DmaBuf::allocate(DmaBufConfig{"../dma_heap/system", testSize}, log),
                  "heap name with a slash rejected", log) &&
           expect(!DmaBuf::allocate(DmaBufConfig{"no-such-heap", testSize}, log),
                  "unknown heap rejected", log);
}
} // namespace

int main() {
    const Logger log{"TEST_DMABUF", LogLevel::Debug};
    if (!heapAvailable()) {
        log.warn("SKIP: /dev/dma_heap/system is not accessible on this machine");
        return EXIT_SUCCESS;
    }
    const bool readWrite{checkReadWrite(log)};
    const bool moved{checkMove(log)};
    const bool rejects{checkRejects(log)};
    const bool passed{readWrite && moved && rejects};
    if (passed) {
        log.info("PASS");
    } else {
        log.error("FAIL");
    }
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
