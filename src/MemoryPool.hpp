#pragma once

#include <vector>
#include <stdexcept>

namespace mxray {

template <typename T>
class MemoryPool {
public:
    // Pre-allocate the entire pool upfront
    explicit MemoryPool(size_t capacity) : capacity_(capacity) {
        // resize() allocates all objects at once and NEVER reallocates on subsequent
        // push_backs — guaranteeing pointer stability for OrderNode* pointers in the LOB.
        pool_.resize(capacity);
        free_list_.resize(capacity);
        // Build the free list: indices 0..capacity-1
        for (size_t i = 0; i < capacity; ++i) {
            free_list_[i] = i;
        }
    }

    // Prohibit copying and moving to ensure pointer stability
    MemoryPool(const MemoryPool&) = delete;
    MemoryPool& operator=(const MemoryPool&) = delete;
    MemoryPool(MemoryPool&&) = delete;
    MemoryPool& operator=(MemoryPool&&) = delete;

    // Get a pointer to an unused object
    // Uses [[likely]] since pool exhaustion should be extremely rare
    [[nodiscard]] T* allocate() {
        if (free_list_.empty()) [[unlikely]] {
            throw std::runtime_error("Memory pool exhausted. Increase capacity.");
        }
        size_t index = free_list_.back();
        free_list_.pop_back();
        return &pool_[index];
    }

    // Return the object to the pool
    // In a real HFT engine, we'd calculate index by pointer arithmetic 
    // to avoid O(N) search or storing index inside T.
    void deallocate(T* ptr) {
        if (ptr < &pool_.front() || ptr > &pool_.back()) [[unlikely]] {
            throw std::invalid_argument("Pointer does not belong to this memory pool.");
        }
        // Calculate the index using pointer arithmetic
        size_t index = ptr - &pool_.front();
        free_list_.push_back(index);
    }

    size_t available() const noexcept {
        return free_list_.size();
    }

    size_t capacity() const noexcept {
        return capacity_;
    }

private:
    std::vector<T> pool_;
    std::vector<size_t> free_list_;
    size_t capacity_;
    // NOTE: head_ removed — was unused. Free list managed by free_list_ vector.
};

} // namespace mxray
