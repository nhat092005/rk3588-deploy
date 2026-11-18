#include "packet_queue.h"

#include <algorithm>
#include <cstring>

std::unique_ptr<PacketQueue> PacketQueue::create(std::size_t capacityBytes, std::size_t maxPackets,
                                                 std::size_t maxPacketBytes) {
    if (capacityBytes == 0 || maxPackets == 0 || maxPacketBytes == 0) {
        return nullptr;
    }
    return std::unique_ptr<PacketQueue>{
        new PacketQueue{Sizes{capacityBytes, maxPackets, maxPacketBytes}}};
}

PacketQueue::PacketQueue(const Sizes& sizes)
    : ring(sizes.capacityBytes), index(sizes.maxPackets), maxPacket{sizes.maxPacketBytes} {}

bool PacketQueue::push(const std::uint8_t* data, std::size_t size, bool keyFrame, double seconds) {
    const std::lock_guard<std::mutex> lock{guard};
    if (finished) {
        return false;
    }
    // After a drop only a key frame can start again.
    const bool fits{size <= maxPacket && size <= ring.size() - used && count < index.size()};
    if (dropping && (!keyFrame || !fits)) {
        ++dropped;
        return false;
    }
    if (!fits) {
        dropping = true;
        ++overflows;
        ++dropped;
        return false;
    }
    const std::size_t firstPart{std::min(size, ring.size() - head)};
    std::memcpy(ring.data() + head, data, firstPart);
    std::memcpy(ring.data(), data + firstPart, size - firstPart);
    index.at((first + count) % index.size()) = Entry{head, size, keyFrame, seconds, dropping};
    ++count;
    head = (head + size) % ring.size();
    used += size;
    dropping = false;
    ready.notify_one();
    return true;
}

PopResult PacketQueue::popFor(QueuedPacket& out, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock{guard};
    ready.wait_for(lock, timeout, [this] { return count > 0 || finished; });
    if (count == 0) {
        return finished ? PopResult::Closed : PopResult::Timeout;
    }
    const Entry entry{index.at(first)};
    out.bytes.resize(entry.size);
    const std::size_t firstPart{std::min(entry.size, ring.size() - entry.offset)};
    std::memcpy(out.bytes.data(), ring.data() + entry.offset, firstPart);
    std::memcpy(out.bytes.data() + firstPart, ring.data(), entry.size - firstPart);
    out.keyFrame = entry.keyFrame;
    out.seconds = entry.seconds;
    out.afterGap = entry.afterGap;
    first = (first + 1) % index.size();
    --count;
    used -= entry.size;
    return PopResult::Packet;
}

void PacketQueue::close() {
    const std::lock_guard<std::mutex> lock{guard};
    finished = true;
    ready.notify_all();
}

std::size_t PacketQueue::overflowCount() const {
    const std::lock_guard<std::mutex> lock{guard};
    return overflows;
}

std::size_t PacketQueue::droppedCount() const {
    const std::lock_guard<std::mutex> lock{guard};
    return dropped;
}
