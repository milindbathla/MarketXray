#pragma once

#include <vector>
#include <stdexcept>

namespace mxray {

template <typename T>
class MemoryPool {
public:
    explicit MemoryPool(size_t capacity) : capacity_(capacity) {
        pool_.resize(capacity);
        free_list_.resize(capacity);
        for (size_t i = 0; i < capacity; ++i) {
            free_list_[i] = i;
        }
    }

    MemoryPool(const MemoryPool&) = delete;
    MemoryPool& operator=(const MemoryPool&) = delete;
    MemoryPool(MemoryPool&&) = delete;
    MemoryPool& operator=(MemoryPool&&) = delete;

    [[nodiscard]] T* allocate() {
        if (free_list_.empty()) [[unlikely]] {
            throw std::runtime_error("Memory pool exhausted. Increase capacity.");
        }
        size_t index = free_list_.back();
        free_list_.pop_back();
        return &pool_[index];
    }

    void deallocate(T* ptr) {
        if (ptr < &pool_.front() || ptr > &pool_.back()) [[unlikely]] {
            throw std::invalid_argument("Pointer does not belong to this memory pool.");
        }
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
};

} // namespace mxray
