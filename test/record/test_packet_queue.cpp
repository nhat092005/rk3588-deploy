// PacketQueue: order and content of the packets (also across the end of the ring), the drop rules
// when the queue is full (everything until the next key frame that fits, which comes out marked as
// after a gap), the limits, close(), and a producer and a slow consumer on two threads. Plain C++,
// runs on the PC. Exit code 0 when every check passes.
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <thread>
#include <vector>

#include "logger.h"
#include "packet_queue.h"

namespace {
constexpr std::uint8_t ringPackets{10}; // packets of smallPacket that fill the ring
constexpr std::uint8_t droppedFirst{10};
constexpr std::uint8_t droppedNext{11};
constexpr std::uint8_t droppedThird{12};
constexpr std::uint8_t keyAfterDrop{13};
constexpr std::uint8_t frameAfterKey{14};
constexpr std::uint8_t refusedOverflow{99};
constexpr std::uint8_t refusedKey{98};
constexpr std::uint8_t refusedBigKey{97};
constexpr std::uint8_t takenKey{96};
constexpr std::uint8_t closeIn{5};
constexpr std::uint8_t closeLate{6};
constexpr double halfSecond{0.5};
constexpr std::size_t ringBytes{1000};
constexpr std::size_t indexSlots{16};
constexpr std::size_t packetLimit{600};
constexpr std::size_t smallPacket{100};
constexpr std::size_t oddPacket{130};
constexpr std::size_t fiveHundred{500};
constexpr std::size_t fullSlots{4};
constexpr std::chrono::milliseconds shortWait{10};
constexpr std::chrono::milliseconds longWait{2000};
constexpr int producedPackets{2000};
constexpr std::size_t threadRing{4096};
constexpr std::size_t threadIndex{64};
constexpr std::size_t threadPacket{200};
constexpr std::chrono::microseconds consumerPause{200};

class Check {
public:
    explicit Check(const Logger& logger) : log{logger} {}

    void expect(bool condition, const char* what) {
        if (!condition) {
            log.error("FAIL: ", what);
            passed = false;
        }
    }

    bool ok() const {
        return passed;
    }

private:
    Logger log;
    bool passed{true};
};

std::unique_ptr<PacketQueue> makeQueue(std::size_t bytes = ringBytes,
                                       std::size_t slots = indexSlots) {
    return PacketQueue::create(bytes, slots, packetLimit);
}

QueuedPacket makeOut() {
    QueuedPacket out;
    out.bytes.reserve(packetLimit);
    return out;
}

// A packet of `size` bytes that all equal `value`.
bool pushPacket(PacketQueue& queue, std::size_t size, std::uint8_t value, bool key = false,
                double seconds = 0.0) {
    const std::vector<std::uint8_t> bytes(size, value);
    return queue.push(bytes.data(), size, key, seconds);
}

// What a packet should hold: this many bytes, all equal to `value`.
struct Fill {
    std::size_t size;
    std::uint8_t value;
};

bool isFilled(const QueuedPacket& packet, const Fill& expected) {
    return packet.bytes.size() == expected.size &&
           std::all_of(packet.bytes.begin(), packet.bytes.end(),
                       [&expected](std::uint8_t byte) { return byte == expected.value; });
}

void checkOrderAndWrap(Check& check) {
    const std::unique_ptr<PacketQueue> queue{makeQueue()};
    QueuedPacket out{makeOut()};
    // 130 bytes at a time through a ring of 1000: the packets straddle its end many times.
    constexpr int rounds{40};
    bool intact{true};
    for (int round{0}; round < rounds; ++round) {
        const auto value{static_cast<std::uint8_t>(round)};
        intact = intact && pushPacket(*queue, oddPacket, value, round % 2 == 0, round * halfSecond);
        intact = intact && queue->popFor(out, shortWait) == PopResult::Packet &&
                 isFilled(out, Fill{oddPacket, value}) && out.keyFrame == (round % 2 == 0) &&
                 out.seconds == round * halfSecond && !out.afterGap;
    }
    check.expect(intact, "packets come out whole, in order, with their flags, across the ring end");
    check.expect(queue->overflowCount() == 0 && queue->droppedCount() == 0, "nothing was dropped");
    // Several in a row.
    check.expect(pushPacket(*queue, smallPacket, 1) && pushPacket(*queue, oddPacket, 2) &&
                     pushPacket(*queue, smallPacket, 3),
                 "three packets in");
    check.expect(queue->popFor(out, shortWait) == PopResult::Packet &&
                     isFilled(out, Fill{smallPacket, 1}),
                 "first");
    check.expect(queue->popFor(out, shortWait) == PopResult::Packet &&
                     isFilled(out, Fill{oddPacket, 2}),
                 "second");
    check.expect(queue->popFor(out, shortWait) == PopResult::Packet &&
                     isFilled(out, Fill{smallPacket, 3}),
                 "third");
    check.expect(queue->popFor(out, shortWait) == PopResult::Timeout, "an empty queue times out");
}

void checkDropUntilKeyFrame(Check& check) {
    const std::unique_ptr<PacketQueue> queue{makeQueue()};
    QueuedPacket out{makeOut()};
    // Fill the ring: ten packets of 100 bytes.
    for (std::uint8_t number{0}; number < ringPackets; ++number) {
        check.expect(pushPacket(*queue, smallPacket, number, number == 0), "fills");
    }
    check.expect(!pushPacket(*queue, smallPacket, droppedFirst),
                 "the first packet that does not fit is dropped");
    check.expect(queue->overflowCount() == 1, "and counts as an overflow");
    // Room again, but every packet is dropped until a key frame.
    check.expect(queue->popFor(out, shortWait) == PopResult::Packet, "pop frees room");
    check.expect(queue->popFor(out, shortWait) == PopResult::Packet, "pop frees more");
    check.expect(!pushPacket(*queue, smallPacket, droppedNext),
                 "an ordinary frame is dropped while dropping");
    check.expect(!pushPacket(*queue, smallPacket, droppedThird), "and the next one");
    check.expect(queue->overflowCount() == 1, "still one overflow");
    check.expect(pushPacket(*queue, smallPacket, keyAfterDrop, true),
                 "a key frame that fits is taken");
    check.expect(pushPacket(*queue, smallPacket, frameAfterKey), "and the stream goes on");
    // Drain: the eight old ones, then the key frame marked after a gap, then the next frame.
    for (std::uint8_t number{2}; number < ringPackets; ++number) {
        check.expect(queue->popFor(out, shortWait) == PopResult::Packet &&
                         isFilled(out, Fill{smallPacket, number}) && !out.afterGap,
                     "the packets before the drop are untouched");
    }
    check.expect(queue->popFor(out, shortWait) == PopResult::Packet &&
                     isFilled(out, Fill{smallPacket, keyAfterDrop}) && out.keyFrame && out.afterGap,
                 "the key frame after the drop is marked");
    check.expect(queue->popFor(out, shortWait) == PopResult::Packet &&
                     isFilled(out, Fill{smallPacket, frameAfterKey}) && !out.afterGap,
                 "the frame after it is not");
    check.expect(queue->droppedCount() == 3, "three packets were dropped");
}

void checkKeyFrameThatDoesNotFit(Check& check) {
    const std::unique_ptr<PacketQueue> queue{makeQueue()};
    QueuedPacket out{makeOut()};
    for (std::uint8_t number{0}; number < ringPackets; ++number) {
        (void)pushPacket(*queue, smallPacket, number);
    }
    check.expect(!pushPacket(*queue, smallPacket, refusedOverflow), "overflow");
    check.expect(!pushPacket(*queue, smallPacket, refusedKey, true),
                 "a key frame with no room is dropped too");
    check.expect(queue->popFor(out, shortWait) == PopResult::Packet, "pop");
    check.expect(!pushPacket(*queue, fiveHundred, refusedBigKey, true),
                 "a key frame that is too large for the room");
    check.expect(pushPacket(*queue, smallPacket, takenKey, true),
                 "the next key frame that fits is taken");
}

void checkLimits(Check& check) {
    {
        const std::unique_ptr<PacketQueue> queue{makeQueue(ringBytes, fullSlots)};
        for (std::size_t number{0}; number < fullSlots; ++number) {
            check.expect(pushPacket(*queue, 1, 0), "small packets fill the index");
        }
        check.expect(!pushPacket(*queue, 1, 0), "the index is full before the bytes");
    }
    {
        const std::unique_ptr<PacketQueue> queue{makeQueue(ringBytes * 2)};
        check.expect(!pushPacket(*queue, packetLimit + 1, 0),
                     "a packet above maxPacketBytes is refused");
        check.expect(queue->overflowCount() == 1, "and counts");
    }
    check.expect(!PacketQueue::create(0, 1, 1) && !PacketQueue::create(1, 0, 1) &&
                     !PacketQueue::create(1, 1, 0),
                 "a zero size is refused");
}

void checkClose(Check& check) {
    const std::unique_ptr<PacketQueue> queue{makeQueue()};
    QueuedPacket out{makeOut()};
    check.expect(pushPacket(*queue, smallPacket, closeIn), "a packet in");
    queue->close();
    check.expect(!pushPacket(*queue, smallPacket, closeLate), "a closed queue refuses");
    check.expect(queue->popFor(out, longWait) == PopResult::Packet &&
                     isFilled(out, Fill{smallPacket, closeIn}),
                 "a packet that was in still comes out");
    check.expect(queue->popFor(out, longWait) == PopResult::Closed, "then the queue says closed");
}

// A producer at full speed and a consumer that pauses: whatever the queue kept arrives in order,
// intact, and what it did not keep is counted.
void checkThreads(Check& check) {
    const std::unique_ptr<PacketQueue> queue{
        PacketQueue::create(threadRing, threadIndex, packetLimit)};
    std::size_t accepted{0};
    std::thread producer{[&] {
        for (int number{0}; number < producedPackets; ++number) {
            const std::vector<std::uint8_t> bytes(threadPacket, static_cast<std::uint8_t>(number));
            const bool kept{queue->push(bytes.data(), bytes.size(), number % 10 == 0,
                                        static_cast<double>(number))};
            accepted += kept ? 1U : 0U;
        }
        queue->close();
    }};
    std::size_t received{0};
    bool inOrder{true};
    bool intact{true};
    double last{-1.0};
    QueuedPacket out{makeOut()};
    while (queue->popFor(out, longWait) == PopResult::Packet) {
        ++received;
        inOrder = inOrder && out.seconds > last;
        last = out.seconds;
        intact =
            intact && isFilled(out, Fill{threadPacket,
                                         static_cast<std::uint8_t>(static_cast<int>(out.seconds))});
        std::this_thread::sleep_for(consumerPause);
    }
    producer.join();
    check.expect(received == accepted, "every packet that was accepted arrives");
    check.expect(inOrder && intact, "in order and intact");
    check.expect(accepted + queue->droppedCount() == static_cast<std::size_t>(producedPackets),
                 "accepted plus dropped is everything");
    check.expect(queue->droppedCount() > 0, "the slow consumer made it overflow");
}
} // namespace

int main() {
    const Logger log{"TEST_PACKET_QUEUE", LogLevel::Debug};
    Check check{log};
    checkOrderAndWrap(check);
    checkDropUntilKeyFrame(check);
    checkKeyFrameThatDoesNotFit(check);
    checkLimits(check);
    checkClose(check);
    checkThreads(check);
    if (check.ok()) {
        log.info("PASS");
        return EXIT_SUCCESS;
    }
    log.error("FAIL");
    return EXIT_FAILURE;
}
