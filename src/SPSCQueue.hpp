#pragma once

#include "Types.hpp"
#include <atomic>
#include <vector>
#include <cstddef>

namespace mxray {

// Single-Producer Single-Consumer Lock-Free Ring Buffer
// Uses atomic head/tail pointers — no mutexes, no OS calls, no latency spikes.
// Cache-line padded to prevent false sharing between producer (Rust) and consumer (C++) threads.
template<typename T, size_t CAPACITY>
class SPSCQueue {
    static_assert((CAPACITY & (CAPACITY - 1)) == 0, "CAPACITY must be a power of 2 for fast modulo.");

public:
    SPSCQueue() : head_(0), tail_(0) {
        buffer_.resize(CAPACITY);
    }

    // Called by the PRODUCER (Rust side)
    // Returns true if push was successful, false if queue is full.
    bool push(const T& item) noexcept {
        const size_t current_tail = tail_.load(std::memory_order_relaxed);
        const size_t next_tail = (current_tail + 1) & (CAPACITY - 1);

        if (next_tail == head_.load(std::memory_order_acquire)) [[unlikely]] {
            return false; // Queue is full
        }

        buffer_[current_tail] = item;
        tail_.store(next_tail, std::memory_order_release);
        return true;
    }

    // Called by the CONSUMER (C++ analytics thread)
    // Returns true if an item was popped into 'item', false if queue is empty.
    bool pop(T& item) noexcept {
        const size_t current_head = head_.load(std::memory_order_relaxed);

        if (current_head == tail_.load(std::memory_order_acquire)) [[unlikely]] {
            return false; // Queue is empty
        }

        item = buffer_[current_head];
        head_.store((current_head + 1) & (CAPACITY - 1), std::memory_order_release);
        return true;
    }

    bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
    }

    size_t size() const noexcept {
        return (tail_.load(std::memory_order_acquire) - head_.load(std::memory_order_acquire)) & (CAPACITY - 1);
    }

private:
    std::vector<T> buffer_;

    // Pad with 64 bytes between head and tail to avoid false sharing across CPU cache lines
    alignas(64) std::atomic<size_t> head_;
    alignas(64) std::atomic<size_t> tail_;
};

// Convenience alias: queue for 65536 ticks (power of 2)
using TickQueue = SPSCQueue<Tick, 65536>;

} // namespace mxray
