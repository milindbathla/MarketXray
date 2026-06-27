#pragma once

#include "Types.hpp"
#include "MemoryPool.hpp"
#include <unordered_map>
#include <vector>
#include <array>
#include <algorithm>

namespace mxray {

class LimitOrderBook {
public:
    // Max price ticks supported. E.g., if price is *100, 1000000 = $10,000 max price.
    // In HFT, price levels are often pre-allocated flat arrays.
    static constexpr size_t MAX_PRICE_TICKS = 1000000;

    explicit LimitOrderBook(size_t max_orders = 1000000) 
        : order_pool_(max_orders), best_bid_(0), best_ask_(MAX_PRICE_TICKS) {
        
        // Pre-allocate the hash map to avoid rehashing
        order_map_.reserve(max_orders);
        
        // Initialize price levels array
        // We use std::vector here to avoid stack overflow, but pre-allocate MAX_PRICE_TICKS
        bids_.resize(MAX_PRICE_TICKS, {0, 0, nullptr, nullptr});
        asks_.resize(MAX_PRICE_TICKS, {0, 0, nullptr, nullptr});
    }

    void add_order(const Tick& tick) {
        // BOUNDS CHECK: Prevent out-of-bounds vector access on malformed/extreme prices
        if (tick.price == 0 || tick.price >= MAX_PRICE_TICKS) [[unlikely]] {
            return; // silently drop invalid ticks
        }

        OrderNode* node = order_pool_.allocate();
        node->order_id = tick.order_id;
        node->price = tick.price;
        node->quantity = tick.quantity;
        node->timestamp_ns = tick.timestamp_ns;
        node->next = nullptr;
        node->prev = nullptr;

        order_map_[tick.order_id] = node;

        if (tick.side == Side::BUY) {
            insert_into_level(bids_[tick.price], node);
            if (tick.price > best_bid_) {
                best_bid_ = tick.price;
            }
        } else {
            insert_into_level(asks_[tick.price], node);
            if (tick.price < best_ask_) {
                best_ask_ = tick.price;
            }
        }
    }

    void cancel_order(uint64_t order_id, Side side) {
        auto it = order_map_.find(order_id);
        if (it == order_map_.end()) [[unlikely]] return;

        OrderNode* node = it->second;
        order_map_.erase(it);

        if (side == Side::BUY) {
            remove_from_level(bids_[node->price], node);
            if (bids_[node->price].head == nullptr && node->price == best_bid_) {
                update_best_bid();
            }
        } else {
            remove_from_level(asks_[node->price], node);
            if (asks_[node->price].head == nullptr && node->price == best_ask_) {
                update_best_ask();
            }
        }

        order_pool_.deallocate(node);
    }

    void execute_order(uint64_t order_id, uint32_t executed_qty, Side side) {
        auto it = order_map_.find(order_id);
        if (it == order_map_.end()) [[unlikely]] return;

        OrderNode* node = it->second;
        if (node->quantity <= executed_qty) {
            // Full execution is identical to a cancel operation structurally
            cancel_order(order_id, side);
        } else {
            // Partial execution
            node->quantity -= executed_qty;
            if (side == Side::BUY) {
                bids_[node->price].total_volume -= executed_qty;
            } else {
                asks_[node->price].total_volume -= executed_qty;
            }
        }
    }

    uint32_t get_best_bid() const noexcept { return best_bid_; }
    uint32_t get_best_ask() const noexcept { return best_ask_; }

private:
    void insert_into_level(PriceLevel& level, OrderNode* node) {
        if (!level.head) {
            level.head = node;
            level.tail = node;
        } else {
            level.tail->next = node;
            node->prev = level.tail;
            level.tail = node;
        }
        level.price = node->price;
        level.total_volume += node->quantity;
    }

    void remove_from_level(PriceLevel& level, OrderNode* node) {
        if (node->prev) node->prev->next = node->next;
        else level.head = node->next;

        if (node->next) node->next->prev = node->prev;
        else level.tail = node->prev;

        level.total_volume -= node->quantity;
    }

    void update_best_bid() {
        while (best_bid_ > 0 && bids_[best_bid_].head == nullptr) {
            best_bid_--;
        }
        // If best_bid_ reached 0 and it's still empty, book is empty: reset to 0
        if (best_bid_ == 0 && bids_[0].head == nullptr) {
            best_bid_ = 0; // sentinel: no bids
        }
    }

    void update_best_ask() {
        while (best_ask_ < MAX_PRICE_TICKS - 1 && asks_[best_ask_].head == nullptr) {
            best_ask_++;
        }
        // If best_ask_ reached the ceiling and is still empty, book is empty: reset sentinel
        if (best_ask_ == MAX_PRICE_TICKS - 1 && asks_[best_ask_].head == nullptr) {
            best_ask_ = MAX_PRICE_TICKS; // sentinel: no asks
        }
    }

    MemoryPool<OrderNode> order_pool_;
    
    // O(1) Lookup for cancel/execute
    // We can swap this for a robin-hood hash map later for more speed.
    std::unordered_map<uint64_t, OrderNode*> order_map_; 

    // Flat arrays for O(1) price level access
    std::vector<PriceLevel> bids_;
    std::vector<PriceLevel> asks_;
    
    uint32_t best_bid_;
    uint32_t best_ask_;
};

} // namespace mxray
