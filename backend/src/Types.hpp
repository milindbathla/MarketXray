#pragma once

#include <cstdint>

namespace mxray {

// Aligning to 64 bytes (cache line) to prevent false sharing in IPC
// and ensuring strict layout compatibility with Rust #[repr(C)].

enum class Side : uint8_t {
    BUY = 1,
    SELL = 2
};

enum class OrderType : uint8_t {
    ADD = 1,
    CANCEL = 2,
    EXECUTE = 3
};

// Represents an incoming tick from Rust via Shared Memory.
// #pragma pack(1) ensures no implicit struct padding (Rust ABI compatibility).
// alignas(64) places the struct on its own cache line to prevent false sharing.
// Total size: 8+8+4+4+1+1+1+37 padding = exactly 64 bytes.
#pragma pack(push, 1)
struct alignas(64) Tick {
    uint64_t timestamp_ns; // Nanosecond precision timestamp
    uint64_t order_id;     // Unique identifier for the order
    uint32_t price;        // Fixed-point (e.g. price * 100 → $150.00 = 15000)
    uint32_t quantity;     // Number of shares
    Side side;             // BUY = 1, SELL = 2
    OrderType type;        // ADD = 1, CANCEL = 2, EXECUTE = 3
    uint8_t trade_side_is_aggressor; // EXECUTE: 1 for standalone trade messages
    uint8_t padding[37];   // Explicit padding to fill the 64-byte cache line
};
#pragma pack(pop)

// An Order Node inside our Limit Order Book
struct OrderNode {
    uint64_t order_id;
    uint32_t price;
    uint32_t quantity;
    Side side;
    uint64_t timestamp_ns;
    OrderNode* next;
    OrderNode* prev;
};

// A Price Level in our Order Book
struct PriceLevel {
    uint32_t price;
    uint64_t total_volume;
    OrderNode* head;
    OrderNode* tail;
};

} // namespace mxray
