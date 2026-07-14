#pragma once

// =============================================================================
// SharedMemory.hpp — Cross-Platform Named Shared Memory
// =============================================================================
// Creates or opens a named shared memory region that both the C++ Analytics
// Engine and the Rust Ingestor can map into their own address spaces.
//
// On Windows: uses CreateFileMapping / MapViewOfFile
// On Linux:   uses shm_open / mmap (POSIX)
//
// The layout of the shared memory block is:
//   [ SharedMemoryHeader (64 bytes) ][ SHMSPSCQueue<Tick, N> (flat array) ]
//
// Both Rust and C++ map the SAME physical RAM — zero copy, zero latency.
// =============================================================================

#include "Types.hpp"
#include <cstring>
#include <stdexcept>
#include <string>
#include <atomic>
#include <cstdint>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#else
  #include <sys/mman.h>
  #include <sys/stat.h>
  #include <fcntl.h>
  #include <unistd.h>
#endif

namespace mxray {

// =============================================================================
// SHMSPSCQueue — Shared-Memory-Safe SPSC Ring Buffer
// =============================================================================
// CRITICAL DIFFERENCE from SPSCQueue.hpp:
//   The old queue used std::vector (heap allocation) — CANNOT be in shared memory.
//   This version embeds a flat C-style array directly in the struct body.
//   The entire struct can be placed at any memory-mapped address.
//
// Rust side must mirror this layout exactly with #[repr(C)].
// =============================================================================
template<typename T, size_t CAPACITY>
struct alignas(64) SHMSPSCQueue {
    static_assert((CAPACITY & (CAPACITY - 1)) == 0,
        "CAPACITY must be a power of 2 for branchless modulo.");
    static_assert(std::is_trivially_copyable_v<T>,
        "T must be trivially copyable to be safe across process boundaries.");

    // Cache-line separated atomics to prevent false sharing
    alignas(64) std::atomic<uint64_t> head{0}; // Consumer reads from head
    alignas(64) std::atomic<uint64_t> tail{0}; // Producer writes to tail

    // Flat array embedded directly in shared memory — NO heap allocation
    T buffer[CAPACITY];

    // Called by PRODUCER (Rust ingestor thread)
    bool push(const T& item) noexcept {
        const uint64_t current_tail = tail.load(std::memory_order_relaxed);
        const uint64_t next_tail    = (current_tail + 1) & (CAPACITY - 1);

        if (next_tail == head.load(std::memory_order_acquire)) [[unlikely]] {
            return false; // Queue full — drop tick or spin
        }

        buffer[current_tail] = item;
        tail.store(next_tail, std::memory_order_release);
        return true;
    }

    // Called by CONSUMER (C++ analytics thread)
    bool pop(T& out) noexcept {
        const uint64_t current_head = head.load(std::memory_order_relaxed);

        if (current_head == tail.load(std::memory_order_acquire)) [[unlikely]] {
            return false; // Queue empty — spin
        }

        out = buffer[current_head];
        head.store((current_head + 1) & (CAPACITY - 1), std::memory_order_release);
        return true;
    }

    bool empty() const noexcept {
        return head.load(std::memory_order_acquire) ==
               tail.load(std::memory_order_acquire);
    }

    uint64_t size() const noexcept {
        return (tail.load(std::memory_order_acquire) -
                head.load(std::memory_order_acquire)) & (CAPACITY - 1);
    }

    void reset() noexcept {
        head.store(0, std::memory_order_relaxed);
        tail.store(0, std::memory_order_relaxed);
    }
};

// =============================================================================
// Shared Memory Layout
// =============================================================================
// Header occupies the first 64 bytes of the shared memory block.
// It contains version info and a "ready" flag so C++ waits until Rust is live.
struct alignas(64) SharedMemoryHeader {
    uint32_t magic;             // 0xMXRAY to validate correct mapping
    uint32_t version;           // Layout version for forward compatibility
    std::atomic<uint32_t> rust_ready;  // Set to 1 by Rust once it starts writing
    std::atomic<uint32_t> cpp_ready;   // Set to 1 by C++ once it starts reading
    uint64_t queue_capacity;    // CAPACITY baked in for sanity checks
    uint8_t padding[36];        // Pad to 64 bytes
};

static constexpr uint32_t SHM_MAGIC   = 0x4D585259; // "MXRY"
static constexpr uint32_t SHM_VERSION = 1;

// The full shared memory block:
// This struct is what BOTH Rust and C++ will map their pointers to.
static constexpr size_t SHM_QUEUE_CAPACITY = 65536; // Must be power of 2

struct SharedMemoryBlock {
    SharedMemoryHeader header;
    SHMSPSCQueue<Tick, SHM_QUEUE_CAPACITY> queue;
};

static_assert(sizeof(Tick) == 64, "Tick must be exactly 64 bytes for cache-line alignment.");

// =============================================================================
// SharedMemoryManager — RAII wrapper for the OS shared memory region
// =============================================================================
class SharedMemoryManager {
public:
    static constexpr const char* DEFAULT_SHM_NAME = "/marketxray_shm";

    // Creator = C++ side. Opens (or creates) the shared memory region.
    explicit SharedMemoryManager(const std::string& name = DEFAULT_SHM_NAME, bool create = true)
        : name_(name), create_(create), block_(nullptr) {
        map(create);
    }

    ~SharedMemoryManager() {
        unmap();
    }

    // No copy/move — ownership of OS handle is unique
    SharedMemoryManager(const SharedMemoryManager&) = delete;
    SharedMemoryManager& operator=(const SharedMemoryManager&) = delete;

    SharedMemoryBlock* block() const noexcept { return block_; }

    // Spin-wait until the Rust producer sets rust_ready = 1
    void wait_for_rust(uint32_t timeout_ms = 120000) const {
        uint32_t elapsed = 0;
        while (block_->header.rust_ready.load(std::memory_order_acquire) != 1) {
            #ifdef _WIN32
                Sleep(10);
            #else
                usleep(10000);
            #endif
            elapsed += 10;
            if (elapsed >= timeout_ms) {
                throw std::runtime_error(
                    "[SharedMemory] Timed out waiting for Rust producer to become ready.");
            }
        }
    }

    // Signal to Rust that C++ is ready to consume
    void signal_cpp_ready() const noexcept {
        block_->header.cpp_ready.store(1, std::memory_order_release);
    }

private:
    void map(bool create) {
        const size_t shm_size = sizeof(SharedMemoryBlock);

#ifdef _WIN32
        if (create) {
            handle_ = CreateFileMappingA(
                INVALID_HANDLE_VALUE,
                nullptr,
                PAGE_READWRITE,
                0,
                static_cast<DWORD>(shm_size),
                name_.c_str()
            );
            if (!handle_) {
                throw std::runtime_error("[SharedMemory] CreateFileMapping failed: " +
                    std::to_string(GetLastError()));
            }
        } else {
            handle_ = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name_.c_str());
            if (!handle_) {
                throw std::runtime_error("[SharedMemory] OpenFileMapping failed: " +
                    std::to_string(GetLastError()));
            }
        }

        void* ptr = MapViewOfFile(handle_, FILE_MAP_ALL_ACCESS, 0, 0, shm_size);
        if (!ptr) {
            CloseHandle(handle_);
            throw std::runtime_error("[SharedMemory] MapViewOfFile failed: " +
                std::to_string(GetLastError()));
        }
        block_ = static_cast<SharedMemoryBlock*>(ptr);

#else // POSIX (Linux/Mac)
        int flags = O_RDWR;
        if (create) flags |= O_CREAT | O_TRUNC;

        fd_ = shm_open(name_.c_str(), flags, 0666);
        if (fd_ < 0) {
            throw std::runtime_error("[SharedMemory] shm_open failed");
        }

        if (create) {
            if (ftruncate(fd_, static_cast<off_t>(shm_size)) < 0) {
                throw std::runtime_error("[SharedMemory] ftruncate failed");
            }
        }

        void* ptr = mmap(nullptr, shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        if (ptr == MAP_FAILED) {
            throw std::runtime_error("[SharedMemory] mmap failed");
        }
        block_ = static_cast<SharedMemoryBlock*>(ptr);
#endif

        if (create) {
            // Zero out and initialize the header
            std::memset(block_, 0, shm_size);
            block_->header.magic          = SHM_MAGIC;
            block_->header.version        = SHM_VERSION;
            block_->header.queue_capacity = SHM_QUEUE_CAPACITY;
            block_->header.rust_ready.store(0, std::memory_order_relaxed);
            block_->header.cpp_ready.store(0, std::memory_order_relaxed);
            block_->queue.reset();
        } else {
            // Validate magic number
            if (block_->header.magic != SHM_MAGIC) {
                throw std::runtime_error("[SharedMemory] Magic mismatch! Rust struct layout mismatch.");
            }
        }
    }

    void unmap() {
        if (!block_) return;
        const size_t shm_size = sizeof(SharedMemoryBlock);

#ifdef _WIN32
        UnmapViewOfFile(block_);
        CloseHandle(handle_);
#else
        munmap(block_, shm_size);
        if (fd_ >= 0) close(fd_);
        if (create_) shm_unlink(name_.c_str());
#endif
        block_ = nullptr;
    }

    std::string name_;
    bool create_;
    SharedMemoryBlock* block_;

#ifdef _WIN32
    HANDLE handle_ = nullptr;
#else
    int fd_ = -1;
#endif
};

} // namespace mxray
