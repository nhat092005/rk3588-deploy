#ifndef MAILBOX_H
#define MAILBOX_H

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <type_traits>

/** @brief What Mailbox::takeFor found. */
enum class TakeResult : std::uint8_t { Item, Timeout, Closed };

/**
 * @brief A few items between two threads, in order. put() never waits: an item that finds the
 *        mailbox full is refused, so a producer is never slowed by a slow consumer. take() waits
 *        for an item or for close(). Not copyable, not movable (it owns a mutex).
 * @tparam Item A trivially copyable type, for example a small struct of indexes.
 * @tparam Capacity Items that fit, at least 1.
 */
template <typename Item, std::size_t Capacity = 1> class Mailbox {
    static_assert(std::is_trivially_copyable_v<Item>, "the mailbox copies items under a lock");
    static_assert(Capacity >= 1, "the mailbox holds at least one item");

public:
    Mailbox() = default;
    Mailbox(const Mailbox&) = delete;
    Mailbox& operator=(const Mailbox&) = delete;
    Mailbox(Mailbox&&) = delete;
    Mailbox& operator=(Mailbox&&) = delete;
    ~Mailbox() = default;

    /**
     * @brief Puts an item at the end.
     * @param item The item to hand over.
     * @return false when the mailbox is full or closed. The item is then not kept.
     */
    bool put(const Item& item) {
        const std::lock_guard<std::mutex> lock{guard};
        if (count >= Capacity || finished) {
            return false;
        }
        items.at((first + count) % Capacity) = item;
        ++count;
        ready.notify_one();
        return true;
    }

    /**
     * @brief Waits for an item and takes the oldest.
     * @param item Receives the item.
     * @return false after close() when no item is left. Items that were put before close() are
     *         still delivered.
     */
    bool take(Item& item) {
        std::unique_lock<std::mutex> lock{guard};
        ready.wait(lock, [this] { return count > 0 || finished; });
        if (count == 0) {
            return false;
        }
        item = items.at(first);
        first = (first + 1) % Capacity;
        --count;
        return true;
    }

    /**
     * @brief Like take(), but gives up after a time.
     * @param item Receives the item when the result is Item.
     * @param timeout How long to wait for an item.
     * @return Item, Timeout when nothing came in time, or Closed after close() with nothing left.
     */
    TakeResult takeFor(Item& item, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock{guard};
        ready.wait_for(lock, timeout, [this] { return count > 0 || finished; });
        if (count == 0) {
            return finished ? TakeResult::Closed : TakeResult::Timeout;
        }
        item = items.at(first);
        first = (first + 1) % Capacity;
        --count;
        return TakeResult::Item;
    }

    /** @brief Refuses new items and wakes a waiting take(). */
    void close() {
        const std::lock_guard<std::mutex> lock{guard};
        finished = true;
        ready.notify_all();
    }

private:
    std::mutex guard;
    std::condition_variable ready;
    std::array<Item, Capacity> items{};
    std::size_t first{0};
    std::size_t count{0};
    bool finished{false};
};

#endif // MAILBOX_H
