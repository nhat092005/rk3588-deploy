// Mailbox: one item at a time, put() refuses without waiting, take() waits, close() wakes and lets
// the last item through. Plain C++, runs on the PC. Exit code 0 when every check passes.
#include <chrono>
#include <cstdlib>
#include <thread>

#include "logger.h"
#include "mailbox.h"

namespace {
struct Item {
    int number{0};
};

constexpr int firstNumber{7};
constexpr int secondNumber{8};
constexpr int thirdNumber{9};
constexpr std::chrono::milliseconds waitTime{50};

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

void checkOneAtATime(Check& check) {
    Mailbox<Item> box;
    check.expect(box.put(Item{firstNumber}), "an empty mailbox takes an item");
    check.expect(!box.put(Item{secondNumber}), "a full mailbox refuses");
    Item got;
    check.expect(box.take(got) && got.number == firstNumber, "the first item is kept");
    check.expect(box.put(Item{thirdNumber}), "the place is free after a take");
    check.expect(box.take(got) && got.number == thirdNumber, "the new item arrives");
}

constexpr std::size_t roomForThree{3};

void checkOrderAndRoom(Check& check) {
    Mailbox<Item, roomForThree> box;
    check.expect(box.put(Item{firstNumber}) && box.put(Item{secondNumber}) &&
                     box.put(Item{thirdNumber}),
                 "three items fit");
    check.expect(!box.put(Item{firstNumber}), "a fourth is refused");
    Item got;
    check.expect(box.take(got) && got.number == firstNumber, "the oldest comes first");
    check.expect(box.put(Item{firstNumber}), "room after a take");
    check.expect(box.take(got) && got.number == secondNumber, "then the second");
    check.expect(box.take(got) && got.number == thirdNumber, "then the third");
    check.expect(box.take(got) && got.number == firstNumber, "the one that wrapped around");
}

void checkTimeout(Check& check) {
    Mailbox<Item> box;
    Item got;
    constexpr std::chrono::milliseconds shortWait{20};
    check.expect(box.takeFor(got, shortWait) == TakeResult::Timeout, "an empty mailbox times out");
    check.expect(box.put(Item{firstNumber}), "put");
    check.expect(box.takeFor(got, shortWait) == TakeResult::Item && got.number == firstNumber,
                 "an item that is there is taken at once");
    std::thread producer{[&] {
        std::this_thread::sleep_for(waitTime);
        (void)box.put(Item{secondNumber});
    }};
    constexpr std::chrono::milliseconds longWait{2000};
    check.expect(box.takeFor(got, longWait) == TakeResult::Item && got.number == secondNumber,
                 "an item that comes while waiting is taken");
    producer.join();
    box.close();
    check.expect(box.takeFor(got, longWait) == TakeResult::Closed,
                 "a closed empty mailbox says so");
}

void checkClose(Check& check) {
    Mailbox<Item> box;
    check.expect(box.put(Item{firstNumber}), "put before close");
    box.close();
    check.expect(!box.put(Item{secondNumber}), "a closed mailbox refuses");
    Item got;
    check.expect(box.take(got) && got.number == firstNumber, "an item put before close arrives");
    check.expect(!box.take(got), "an empty closed mailbox ends the take");
}

void checkWaiting(Check& check) {
    Mailbox<Item> box;
    Item got;
    bool taken{false};
    std::thread consumer{[&] { taken = box.take(got); }};
    std::this_thread::sleep_for(waitTime);
    check.expect(box.put(Item{firstNumber}), "put while a consumer waits");
    consumer.join();
    check.expect(taken && got.number == firstNumber, "the waiting take wakes with the item");

    bool endedTake{true};
    std::thread waiting{[&] {
        Item unused;
        endedTake = box.take(unused);
    }};
    std::this_thread::sleep_for(waitTime);
    box.close();
    waiting.join();
    check.expect(!endedTake, "close wakes a waiting take");
}
} // namespace

int main() {
    const Logger log{"TEST_MAILBOX", LogLevel::Debug};
    Check check{log};
    checkOneAtATime(check);
    checkOrderAndRoom(check);
    checkTimeout(check);
    checkClose(check);
    checkWaiting(check);
    if (check.ok()) {
        log.info("PASS");
        return EXIT_SUCCESS;
    }
    log.error("FAIL");
    return EXIT_FAILURE;
}
