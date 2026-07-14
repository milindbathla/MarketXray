#pragma once

#include "Types.hpp"
#include "LimitOrderBook.hpp"
#include <vector>
#include <deque>
#include <numeric>
#include <cmath>
#include <stdexcept>
#include <utility>
#include <algorithm>

namespace mxray {

// Rolling window metrics for OFI and VWAP using circular buffers.
// All computations are designed for constant-time updates.
class AnalyticsEngine {
public:
    explicit AnalyticsEngine(size_t window_size = 500)
        : window_(window_size),
          ofi_window_size_(window_size),
          vwap_volume_(0),
          vwap_notional_(0.0),
          total_trade_count_(0) {
        // No reservation needed: deque grows from both ends in O(1)
    }

    // -------------------------------------------------------------------
    // OFI: Order Flow Imbalance
    // OFI measures the net order flow pressure.
    // OFI = sum( delta_bid_size - delta_ask_size ) over a rolling window.
    // A strongly positive OFI → imminent price rise.
    // A strongly negative OFI → imminent price fall.
    // -------------------------------------------------------------------
    void update_ofi(const LimitOrderBook& lob) {
        const uint32_t curr_bid_px = lob.get_best_bid();
        const uint32_t curr_ask_px = lob.get_best_ask();
        const int64_t curr_bid_sz = static_cast<int64_t>(lob.get_best_bid_volume());
        const int64_t curr_ask_sz = static_cast<int64_t>(lob.get_best_ask_volume());

        if (!ofi_initialized_) {
            prev_best_bid_px_ = curr_bid_px;
            prev_best_ask_px_ = curr_ask_px;
            prev_best_bid_sz_ = curr_bid_sz;
            prev_best_ask_sz_ = curr_ask_sz;
            ofi_initialized_ = true;
            return;
        }

        int64_t delta = 0;
        if (curr_bid_px >= prev_best_bid_px_) delta += curr_bid_sz;
        if (curr_bid_px <= prev_best_bid_px_) delta -= prev_best_bid_sz_;
        if (curr_ask_px <= prev_best_ask_px_) delta -= curr_ask_sz;
        if (curr_ask_px >= prev_best_ask_px_) delta += prev_best_ask_sz_;

        if (ofi_buffer_.size() == ofi_window_size_) {
            // O(1) pop_front on deque (was O(N) erase on vector — fixed)
            ofi_sum_ -= ofi_buffer_.front();
            ofi_buffer_.pop_front();
        }
        ofi_buffer_.push_back(delta);
        ofi_sum_ += delta;

        prev_best_bid_px_ = curr_bid_px;
        prev_best_ask_px_ = curr_ask_px;
        prev_best_bid_sz_ = curr_bid_sz;
        prev_best_ask_sz_ = curr_ask_sz;
    }

    double get_ofi() const noexcept {
        if (ofi_buffer_.empty()) return 0.0;
        return static_cast<double>(ofi_sum_);
    }

    // Normalized OFI in range [-1, 1]
    double get_ofi_normalized() const noexcept {
        double max_possible = (double)ofi_window_size_ * 1e7; // rough normalization
        if (max_possible == 0) return 0.0;
        return std::clamp(get_ofi() / max_possible, -1.0, 1.0);
    }

    // -------------------------------------------------------------------
    // VWAP: Volume-Weighted Average Price
    // The "true" average price weighted by volume. Tracks where large
    // institutions are actually transacting.
    // VWAP = sum(price_i * volume_i) / sum(volume_i)
    // -------------------------------------------------------------------
    void update_vwap(uint32_t price, uint32_t quantity) {
        if (price == 0 || quantity == 0) return;

        if (vwap_buffer_.size() == window_) {
            const auto& oldest = vwap_buffer_.front();
            vwap_notional_ -= static_cast<double>(oldest.first) * oldest.second;
            vwap_volume_ -= oldest.second;
            vwap_buffer_.pop_front();
        }

        vwap_buffer_.push_back({price, quantity});
        vwap_notional_ += (double)price * quantity;
        vwap_volume_ += quantity;
        total_trade_count_++;
    }

    double get_vwap() const noexcept {
        if (vwap_volume_ == 0) return 0.0;
        return vwap_notional_ / vwap_volume_;
    }

    // -------------------------------------------------------------------
    // Bid-Ask Spread
    // -------------------------------------------------------------------
    double get_spread(const LimitOrderBook& lob) const noexcept {
        const int64_t ask = static_cast<int64_t>(lob.get_best_ask());
        const int64_t bid = static_cast<int64_t>(lob.get_best_bid());
        if (bid == 0 || ask >= static_cast<int64_t>(LimitOrderBook::MAX_PRICE_TICKS) || ask <= bid) {
            return -1.0;
        }
        return static_cast<double>(ask - bid);
    }

    uint64_t total_trade_count() const noexcept { return total_trade_count_; }

    // Process a tick: route it to OFI and VWAP
    void process(const Tick& tick, const LimitOrderBook& lob) {
        update_ofi(lob);
        if (tick.type == OrderType::EXECUTE) {
            update_vwap(tick.price, tick.quantity);
        }
    }

private:
    size_t window_;
    size_t ofi_window_size_;

    std::deque<int64_t> ofi_buffer_; // O(1) push_back and pop_front
    int64_t ofi_sum_ = 0;
    bool ofi_initialized_ = false;
    uint32_t prev_best_bid_px_ = 0;
    uint32_t prev_best_ask_px_ = LimitOrderBook::MAX_PRICE_TICKS;
    int64_t prev_best_bid_sz_ = 0;
    int64_t prev_best_ask_sz_ = 0;

    std::deque<std::pair<uint32_t, uint32_t>> vwap_buffer_;
    uint64_t vwap_volume_;
    double vwap_notional_;
    uint64_t total_trade_count_;
};

} // namespace mxray
