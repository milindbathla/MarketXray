#pragma once


#include "Types.hpp"
#include "SharedMemory.hpp"
#include "LimitOrderBook.hpp"
#include "AnalyticsEngine.hpp"
#include "HawkesPIN.hpp"
#include "GraphAlgos.hpp"
#include "Exporter.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <thread>
#include <string>
#include <unordered_map>
#include <vector>

namespace mxray {

static constexpr uint64_t ITCH_DATE_OFFSET_NS = 1'567'123'200'000'000'000ULL;
static constexpr uint64_t MARKET_OPEN_NS = 34'200'000'000'000ULL; // 09:30:00 ET

using SnapshotCallback = std::function<void(const std::string&)>;

struct EngineConfig {
    size_t ofi_window          = 500;      // Rolling window for OFI
    size_t pin_window          = 1000;     // Rolling window for PIN
    size_t max_orders          = 1'000'000; // Preallocated active-order capacity
    uint32_t snapshot_every_n  = 1000;     // Emit a JSON snapshot every N ticks
    double hawkes_mu           = 0.5;
    double hawkes_alpha        = 0.8;
    double hawkes_beta         = 1.0;
    bool   verbose             = false;    // Print debug info per-tick
};

struct EngineStats {
    uint64_t total_ticks       = 0;
    uint64_t total_adds        = 0;
    uint64_t total_cancels     = 0;
    uint64_t total_executes    = 0;
    uint64_t spoof_alerts      = 0;
    uint64_t snapshots_emitted = 0;
    double   elapsed_ms        = 0.0;

    double throughput_per_sec() const noexcept {
        return (elapsed_ms > 0)
            ? (total_ticks / (elapsed_ms / 1000.0))
            : 0.0;
    }

    double avg_latency_ns() const noexcept {
        return (total_ticks > 0)
            ? (elapsed_ms * 1e6 / total_ticks)
            : 0.0;
    }
};

class EngineLoop {
public:
    explicit EngineLoop(const EngineConfig& cfg = {})
        : cfg_(cfg),
          lob_(cfg.max_orders),
          analytics_(cfg.ofi_window),
          hawkes_(cfg.hawkes_mu, cfg.hawkes_alpha, cfg.hawkes_beta),
          pin_(cfg.pin_window),
          running_(false)
    {}

    void on_snapshot(SnapshotCallback cb) {
        callbacks_.push_back(std::move(cb));
    }

    void run_from_shared_memory(const std::string& shm_name = SharedMemoryManager::DEFAULT_SHM_NAME) {
        std::cerr << "[Engine] Attaching to shared memory: " << shm_name << "\n";
        SharedMemoryManager shm(shm_name, true); // create = true (C++ owns the SHM region)

        std::cerr << "[Engine] Waiting for Rust producer to become ready...\n";
        shm.wait_for_rust();
        shm.signal_cpp_ready();
        std::cerr << "[Engine] Rust is live! Processing ticks...\n";

        running_.store(true, std::memory_order_relaxed);
        auto& queue = shm.block()->queue;

        Tick tick;
        using clock = std::chrono::high_resolution_clock;
        auto start = clock::now();

        while (running_.load(std::memory_order_relaxed)) {
            if (queue.pop(tick)) [[likely]] {
                dispatch(tick);
                stats_.total_ticks++;

                if (stats_.total_ticks % cfg_.snapshot_every_n == 0) [[unlikely]] {
                    emit_snapshot();
                }
            }
        }

        auto end = clock::now();
        stats_.elapsed_ms =
            std::chrono::duration_cast<std::chrono::microseconds>(end - start).count() / 1000.0;
    }

    void run_from_benchmark(const std::vector<Tick>& ticks) {
        running_.store(true, std::memory_order_relaxed);

        using clock = std::chrono::high_resolution_clock;
        auto start  = clock::now();

        for (const Tick& tick : ticks) {
            if (!running_.load(std::memory_order_relaxed)) break;
            dispatch(tick);
            stats_.total_ticks++;

            if (stats_.total_ticks % cfg_.snapshot_every_n == 0) [[unlikely]] {
                emit_snapshot();
            }
        }

        auto end = clock::now();
        stats_.elapsed_ms =
            std::chrono::duration_cast<std::chrono::microseconds>(end - start).count() / 1000.0;
        running_.store(false, std::memory_order_relaxed);
    }

    void stop() noexcept {
        running_.store(false, std::memory_order_release);
    }

    const EngineStats& stats() const noexcept { return stats_; }
    const LimitOrderBook& lob() const noexcept { return lob_; }
    const AnalyticsEngine& analytics() const noexcept { return analytics_; }

private:
    void dispatch(const Tick& tick) {
        last_tick_ts_ns_ = tick.timestamp_ns;

        switch (tick.type) {
            case OrderType::ADD: {
                if (!lob_.add_order(tick)) break;
                analytics_.process(tick, lob_);
                spoof_.record_add(tick.order_id, tick.price, tick.quantity, tick.timestamp_ns);
                add_timestamps_[tick.order_id] = tick.timestamp_ns;
                stats_.total_adds++;
                break;
            }
            case OrderType::CANCEL: {
                const auto mutation = lob_.cancel_order(tick.order_id, tick.quantity);
                if (!mutation.found || mutation.affected_quantity == 0) break;
                Tick normalized = tick;
                normalized.quantity = mutation.affected_quantity;
                analytics_.process(normalized, lob_);
                auto alert = spoof_.check_cancel(
                    tick.order_id, tick.price, normalized.quantity, tick.timestamp_ns);
                double lifetime_ms = 0.0;
                if (auto timestamp = add_timestamps_.find(tick.order_id);
                    timestamp != add_timestamps_.end() && tick.timestamp_ns > timestamp->second) {
                    lifetime_ms = static_cast<double>(tick.timestamp_ns - timestamp->second) * 1e-6;
                }
                if (alert.quantity > 0 && spoof_alerts_window_.size() < 256) {
                    spoof_alerts_window_.push_back({
                        alert.price,
                        alert.quantity,
                        lifetime_ms,
                        alert.cancel_ratio
                    });
                }
                if (alert.is_suspected_spoof) [[unlikely]] {
                    stats_.spoof_alerts++;
                    spoof_alerts_in_window_++;

                    if (cfg_.verbose) {
                        std::cerr << "[SPOOF ALERT] order=" << tick.order_id
                                  << " price=" << tick.price
                                  << " cancel_ratio=" << alert.cancel_ratio << "\n";
                    }
                }
                if (mutation.order_removed) add_timestamps_.erase(tick.order_id);
                stats_.total_cancels++;
                cancels_in_window_++;
                break;
            }
            case OrderType::EXECUTE: {
                const bool standalone_trade = tick.trade_side_is_aggressor != 0;
                const auto mutation = standalone_trade
                    ? LimitOrderBook::MutationResult{true, false, tick.quantity}
                    : lob_.execute_order(tick.order_id, tick.quantity);
                if (!mutation.found || mutation.affected_quantity == 0) break;

                Tick normalized = tick;
                normalized.quantity = mutation.affected_quantity;
                analytics_.process(normalized, lob_);
                hawkes_.record_event(static_cast<double>(tick.timestamp_ns) * 1e-9);
                const Side aggressor = standalone_trade
                    ? tick.side
                    : (tick.side == Side::BUY ? Side::SELL : Side::BUY);
                pin_.record_trade(aggressor);
                if (!standalone_trade) {
                    spoof_.record_execute(tick.order_id, normalized.quantity);
                    if (mutation.order_removed) add_timestamps_.erase(tick.order_id);
                }
                if (executions_window_.size() < 256) {
                    executions_window_.push_back({tick.timestamp_ns, tick.price, normalized.quantity, aggressor});
                }
                stats_.total_executes++;
                break;
            }
            default: [[unlikely]] break; // Unknown tick type — silently drop
        }
    }

    void emit_snapshot() {
        uint64_t ts_now_ns = (last_tick_ts_ns_ != 0)
            ? last_tick_ts_ns_
            : static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::system_clock::now().time_since_epoch()
                ).count());


        if (ts_now_ns < ITCH_DATE_OFFSET_NS + MARKET_OPEN_NS) {
            spoof_alerts_in_window_ = 0;
            cancels_in_window_ = 0;
            spoof_alerts_window_.clear();
            executions_window_.clear();
            return;
        }
        EngineSnapshot snap{};
        snap.timestamp_ns                     = ts_now_ns;
        snap.source_timestamp_ns              = (ts_now_ns >= ITCH_DATE_OFFSET_NS)
                                                    ? (ts_now_ns - ITCH_DATE_OFFSET_NS)
                                                    : ts_now_ns;
        snap.ofi                              = analytics_.get_ofi();
        snap.ofi_normalized                   = analytics_.get_ofi_normalized();
        snap.vwap                             = analytics_.get_vwap();
        snap.best_bid                         = lob_.get_best_bid();
        snap.best_ask                         = lob_.get_best_ask();
        snap.spread                           = analytics_.get_spread(lob_);

        double ts_now_s = static_cast<double>(ts_now_ns) * 1e-9;
        snap.hawkes_intensity                 = hawkes_.compute_intensity(ts_now_s);
        snap.hawkes_next_event_probability_1s = hawkes_.probability_next_event_within(ts_now_s, 1e9);
        snap.hawkes_critical                  = hawkes_.is_critical();

        snap.pin_score                        = pin_.estimate_pin();
        snap.pin_level                        = pin_.pin_level();

        const auto asks = lob_.top_asks(200);
        const auto bids = lob_.top_bids(50);

        snap.flash_crash_probability          = graph_.compute_flash_crash_probability(
                                                    asks, lob_.get_best_ask(), 20);

        {
            auto impact = graph_.simulate_market_impact(
                lob_, asks, 10000, Side::BUY);
            snap.impact_avg_fill_price  = impact.average_fill_price;
            snap.impact_slippage_bps    = impact.slippage_bps;
            snap.impact_fully_filled    = impact.fully_filled;
            snap.impact_levels_consumed = impact.levels_consumed;
        }

        {
            double mid = (snap.best_bid > 0 && snap.best_ask > 0)
                ? (snap.best_bid + snap.best_ask) * 0.5
                : 1.0;
            double spread_frac = (mid > 0) ? snap.spread / mid : 0.0;
            double r = (mid > 0) ? mid : 1.0;
            std::vector<std::vector<double>> rates = {
                {1.000,        1.0 / r,       1.0 / (r * 15.0)},
                {r,            1.000,          1.0 / 15.0      },
                {r * 15.0,     15.0,           1.000           }
            };
            if (spread_frac > 0.0) {
                for (int i = 0; i < 3; ++i)
                    for (int j = 0; j < 3; ++j)
                        if (i != j) rates[i][j] *= (1.0 - spread_frac * 0.5);
            }
            auto arb = graph_.detect_arbitrage(rates, 3);
            snap.arbitrage_opportunity    = arb.opportunity_found;
            snap.arbitrage_profit_factor  = arb.profit_factor;
            snap.arbitrage_cycle          = std::move(arb.cycle);
        }

        snap.spoofing_detected     = (spoof_alerts_in_window_ > 0);
        snap.spoofing_cancel_ratio = (cancels_in_window_ > 0)
                                        ? (double)spoof_alerts_in_window_ / cancels_in_window_
                                        : 0.0;

        snap.spoofing_orders.reserve(spoof_alerts_window_.size());
        for (const auto& a : spoof_alerts_window_) {
            snap.spoofing_orders.push_back({
                a.price, a.quantity, a.lifetime_ms, a.score
            });
        }
        snap.executions = executions_window_;

        for (const auto& level : bids) snap.book_bids.push_back({level.price, level.total_volume});
        for (size_t i = 0; i < asks.size() && i < 50; ++i) {
            snap.book_asks.push_back({asks[i].price, asks[i].total_volume});
        }

        std::string json = snapshot_to_json(snap);
        stats_.snapshots_emitted++;

        for (auto& cb : callbacks_) {
            cb(json);
        }

        spoof_alerts_in_window_ = 0;
        cancels_in_window_ = 0;
        spoof_alerts_window_.clear();
        executions_window_.clear();
    }

    EngineConfig        cfg_;
    LimitOrderBook      lob_;
    AnalyticsEngine     analytics_;
    HawkesProcess       hawkes_;
    PINEstimator        pin_;
    GraphEngine         graph_;
    SpoofingDetector    spoof_;

    std::atomic<bool>   running_;
    EngineStats         stats_;
    uint64_t            last_tick_ts_ns_ = 0;
    uint64_t            spoof_alerts_in_window_ = 0;
    uint64_t            cancels_in_window_ = 0;

    struct SpoofAlertEntry {
        uint32_t price;
        uint64_t quantity;
        double   lifetime_ms;
        double   score;
    };
    std::vector<SpoofAlertEntry> spoof_alerts_window_;
    std::vector<EngineSnapshot::ExecutionEntry> executions_window_;

    std::unordered_map<uint64_t, uint64_t> add_timestamps_;

    std::vector<SnapshotCallback> callbacks_;
};

} // namespace mxray
