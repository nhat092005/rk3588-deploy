#ifndef PACKET_QUEUE_H
#define PACKET_QUEUE_H

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

/** @brief A packet that PacketQueue::popFor() hands out. */
struct QueuedPacket {
    /** The bytes, in a vector that the consumer reserved: popFor() does not allocate. */
    std::vector<std::uint8_t> bytes;
    bool keyFrame{false};
    /** Time of the frame, in seconds. */
    double seconds{0.0};
    /** True when packets before this one were dropped (the queue was full), so the stream has a
     *  break here. Only a key frame has it. */
    bool afterGap{false};
};

/** @brief What PacketQueue::popFor() found. */
enum class PopResult : std::uint8_t { Packet, Timeout, Closed };

/**
 * @brief The encoded video between the thread that encodes and the thread that writes files:
 *        a ring of bytes and an index of the packets in it, both sized by create(), so push() and
 *        popFor() allocate nothing.
 *
 * One producer and one consumer. push() never waits: when the queue is full the packet is
 * dropped, and so is every packet after it until the next key frame that fits, which comes out
 * marked `afterGap`. A stream with a hole in the middle cannot be decoded from there, so the
 * consumer starts a new file at that key frame. Not copyable, not movable (it owns a mutex).
 */
class PacketQueue {
public:
    /**
     * @brief Allocates the ring.
     * @param capacityBytes Bytes of the ring, above 0.
     * @param maxPackets Packets of the index, above 0.
     * @param maxPacketBytes Largest packet that popFor() can be asked to hold. A larger one is
     *        refused by push() as if the queue were full. Above 0.
     * @return The queue, or null when an argument is 0.
     */
    static std::unique_ptr<PacketQueue> create(std::size_t capacityBytes, std::size_t maxPackets,
                                               std::size_t maxPacketBytes);

    PacketQueue(const PacketQueue&) = delete;
    PacketQueue& operator=(const PacketQueue&) = delete;
    PacketQueue(PacketQueue&&) = delete;
    PacketQueue& operator=(PacketQueue&&) = delete;
    ~PacketQueue() = default;

    /**
     * @brief Copies a packet into the queue.
     * @param data Annex B bytes of one frame.
     * @param size Number of bytes.
     * @param keyFrame Whether the packet starts with a key frame.
     * @param seconds Time of the frame.
     * @return false when the packet was not kept: the queue is closed, full, or dropping until
     *         the next key frame.
     */
    bool push(const std::uint8_t* data, std::size_t size, bool keyFrame, double seconds);

    /**
     * @brief Waits for a packet and takes the oldest.
     * @param out Receives the packet. `out.bytes` must have room for maxPacketBytes.
     * @param timeout How long to wait.
     * @return Packet, Timeout, or Closed after close() when nothing is left.
     */
    PopResult popFor(QueuedPacket& out, std::chrono::milliseconds timeout);

    /** @brief Refuses new packets and wakes a waiting popFor(). Packets already in are still
     *         delivered. */
    void close();

    /** @brief Times the queue became full (each starts a break in the stream). */
    std::size_t overflowCount() const;

    /** @brief Packets dropped, in total. */
    std::size_t droppedCount() const;

private:
    struct Entry {
        std::size_t offset{0};
        std::size_t size{0};
        bool keyFrame{false};
        double seconds{0.0};
        bool afterGap{false};
    };

    /** @brief Sizes of the queue, grouped so that they cannot be swapped. */
    struct Sizes {
        std::size_t capacityBytes;
        std::size_t maxPackets;
        std::size_t maxPacketBytes;
    };

    explicit PacketQueue(const Sizes& sizes);

    mutable std::mutex guard;
    std::condition_variable ready;
    std::vector<std::uint8_t> ring;
    std::vector<Entry> index;
    std::size_t maxPacket;
    std::size_t head{0};
    std::size_t used{0};
    std::size_t first{0};
    std::size_t count{0};
    bool dropping{false};
    bool finished{false};
    std::size_t overflows{0};
    std::size_t dropped{0};
};

#endif // PACKET_QUEUE_H
