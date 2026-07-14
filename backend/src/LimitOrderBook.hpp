#pragma once

#include "MemoryPool.hpp"
#include "Types.hpp"

#include <algorithm>
#include <set>
#include <unordered_map>
#include <vector>

namespace mxray {

// Sparse price levels avoid allocating two million slots for every symbol.
class LimitOrderBook {
public:
    static constexpr uint32_t MAX_PRICE_TICKS = 1'000'000;
    static constexpr uint32_t MIN_VALID_PRICE = 100;

    struct MutationResult {
        bool found = false;
        bool order_removed = false;
        uint32_t affected_quantity = 0;
    };

    explicit LimitOrderBook(size_t max_orders = 1'000'000)
        : order_pool_(max_orders), best_bid_(0), best_ask_(MAX_PRICE_TICKS) {
        order_map_.reserve(max_orders);
        bids_.reserve(max_orders / 8);
        asks_.reserve(max_orders / 8);
    }

    bool add_order(const Tick& tick) {
        if (tick.price < MIN_VALID_PRICE || tick.price >= MAX_PRICE_TICKS || tick.quantity == 0) {
            return false;
        }
        // Reusing an order id would orphan its existing price-time node.
        if (order_map_.contains(tick.order_id)) return false;

        OrderNode* node = order_pool_.allocate();
        node->order_id = tick.order_id;
        node->price = tick.price;
        node->quantity = tick.quantity;
        node->side = tick.side;
        node->timestamp_ns = tick.timestamp_ns;
        node->next = nullptr;
        node->prev = nullptr;
        order_map_[tick.order_id] = node;

        auto& levels = tick.side == Side::BUY ? bids_ : asks_;
        auto& prices = tick.side == Side::BUY ? bid_prices_ : ask_prices_;
        auto [level, inserted] = levels.try_emplace(tick.price, PriceLevel{tick.price, 0, nullptr, nullptr});
        insert_into_level(level->second, node);
        if (inserted) prices.insert(tick.price);
        update_best_prices();
        return true;
    }

    MutationResult cancel_order(uint64_t order_id, uint32_t cancelled_qty) {
        auto it = order_map_.find(order_id);
        if (it == order_map_.end()) return {};

        OrderNode* node = it->second;
        const uint32_t quantity = cancelled_qty == 0
            ? node->quantity
            : std::min(cancelled_qty, node->quantity);
        if (quantity == 0) return {true, false, 0};

        auto& levels = node->side == Side::BUY ? bids_ : asks_;
        auto& prices = node->side == Side::BUY ? bid_prices_ : ask_prices_;
        auto level = levels.find(node->price);
        if (level == levels.end()) return {};

        if (quantity < node->quantity) {
            node->quantity -= quantity;
            level->second.total_volume -= quantity;
            return {true, false, quantity};
        }

        remove_from_level(level->second, node);
        if (level->second.head == nullptr) {
            prices.erase(node->price);
            levels.erase(level);
        }
        order_map_.erase(it);
        order_pool_.deallocate(node);
        update_best_prices();
        return {true, true, quantity};
    }

    MutationResult execute_order(uint64_t order_id, uint32_t executed_qty) {
        auto it = order_map_.find(order_id);
        if (it == order_map_.end()) return {};

        OrderNode* node = it->second;
        const uint32_t quantity = std::min(executed_qty, node->quantity);
        if (quantity == 0) return {true, false, 0};
        if (quantity == node->quantity) return cancel_order(order_id, quantity);

        node->quantity -= quantity;
        auto& levels = node->side == Side::BUY ? bids_ : asks_;
        levels.at(node->price).total_volume -= quantity;
        return {true, false, quantity};
    }

    uint32_t get_best_bid() const noexcept { return best_bid_; }
    uint32_t get_best_ask() const noexcept { return best_ask_; }

    uint64_t get_best_bid_volume() const noexcept { return level_volume(Side::BUY, best_bid_); }
    uint64_t get_best_ask_volume() const noexcept { return level_volume(Side::SELL, best_ask_); }

    uint64_t level_volume(Side side, uint32_t price) const noexcept {
        const auto& levels = side == Side::BUY ? bids_ : asks_;
        auto level = levels.find(price);
        return level == levels.end() ? 0 : level->second.total_volume;
    }

    std::vector<PriceLevel> top_bids(size_t depth) const {
        return collect_levels(bid_prices_.rbegin(), bid_prices_.rend(), bids_, depth);
    }

    std::vector<PriceLevel> top_asks(size_t depth) const {
        return collect_levels(ask_prices_.begin(), ask_prices_.end(), asks_, depth);
    }

private:
    template <typename Iterator>
    static std::vector<PriceLevel> collect_levels(
        Iterator begin,
        Iterator end,
        const std::unordered_map<uint32_t, PriceLevel>& levels,
        size_t depth) {
        std::vector<PriceLevel> result;
        result.reserve(depth);
        for (auto it = begin; it != end && result.size() < depth; ++it) {
            auto level = levels.find(*it);
            if (level != levels.end() && level->second.total_volume > 0) result.push_back(level->second);
        }
        return result;
    }

    static void insert_into_level(PriceLevel& level, OrderNode* node) {
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

    static void remove_from_level(PriceLevel& level, OrderNode* node) {
        if (node->prev) node->prev->next = node->next;
        else level.head = node->next;
        if (node->next) node->next->prev = node->prev;
        else level.tail = node->prev;
        level.total_volume -= node->quantity;
        node->next = nullptr;
        node->prev = nullptr;
    }

    void update_best_prices() noexcept {
        best_bid_ = bid_prices_.empty() ? 0 : *bid_prices_.rbegin();
        best_ask_ = ask_prices_.empty() ? MAX_PRICE_TICKS : *ask_prices_.begin();
    }

    MemoryPool<OrderNode> order_pool_;
    std::unordered_map<uint64_t, OrderNode*> order_map_;
    std::set<uint32_t> bid_prices_;
    std::set<uint32_t> ask_prices_;
    std::unordered_map<uint32_t, PriceLevel> bids_;
    std::unordered_map<uint32_t, PriceLevel> asks_;
    uint32_t best_bid_;
    uint32_t best_ask_;
};

} // namespace mxray
