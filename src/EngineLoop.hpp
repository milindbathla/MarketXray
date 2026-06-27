#pragma once

// =============================================================================
// EngineLoop.hpp — Real-Time Event Loop Daemon
// =============================================================================
// This is the "brain" of MarketXray at runtime.
// It continuously reads Ticks from the Shared Memory SPSC Queue,
// dispatches them to all analytics engines, and emits JSON snapshots
// at a configurable frequency for the frontend.
//
// Architecture:
//   [Rust Ingestor] → SHM SPSC Queue → [EngineLoop] → JSON snapshots
//                                            ↓
//                              LimitOrderBook / Analytics /
//                              Hawkes / PIN / Graph engines
// =============================================================================

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
#include <vector>

namespace mxray {

// Callback type: called every time a new JSON snapshot is ready.
// The frontend integration (WebSocket, stdout, file, etc.) hooks into this.
using SnapshotCallback = std::function<void(const std::string&)>;

// =============================================================================
// EngineConfig — Runtime tunable parameters
// =============================================================================
struct EngineConfig {
    size_t ofi_window          = 500;      // Rolling window for OFI
    size_t pin_window          = 1000;     // Rolling window for PIN
    uint32_t snapshot_every_n  = 1000;     // Emit a JSON snapshot every N ticks
    double hawkes_mu           = 0.5;
    double hawkes_alpha        = 0.8;
    double hawkes_beta         = 1.0;
    bool   verbose             = false;    // Print debug info per-tick
};

// =============================================================================
// EngineStats — Accumulated performance counters
// =============================================================================
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

// =============================================================================
// EngineLoop — The Main Daemon
// =============================================================================
class EngineLoop {
public:
    explicit EngineLoop(const EngineConfig& cfg = {})
        : cfg_(cfg),
          lob_(2000000),
          analytics_(cfg.ofi_window),
          hawkes_(cfg.hawkes_mu, cfg.hawkes_alpha, cfg.hawkes_beta),
          pin_(cfg.pin_window),
          running_(false)
    {}

    // Register a callback for when a snapshot is ready.
    // Can be called multiple times to add multiple sinks (stdout, WebSocket, file).
    void on_snapshot(SnapshotCallback cb) {
        callbacks_.push_back(std::move(cb));
    }

    // ===========================================================================
    // run_from_shared_memory()
    // Attaches to the named shared memory region and processes ticks in real time.
    // This runs FOREVER until stop() is called from another thread.
    // ===========================================================================
    void run_from_shared_memory(const std::string& shm_name = SharedMemoryManager::DEFAULT_SHM_NAME) {
        std::cout << "[Engine] Attaching to shared memory: " << shm_name << "\n";
        SharedMemoryManager shm(shm_name, true); // create = true (C++ owns the SHM region)

        std::cout << "[Engine] Waiting for Rust producer to become ready...\n";
        shm.wait_for_rust();
        shm.signal_cpp_ready();
        std::cout << "[Engine] Rust is live! Processing ticks...\n";

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
            // else: spin — no sleep, no mutex, no syscall. Pure busy-wait for minimum latency.
        }

        auto end = clock::now();
        stats_.elapsed_ms =
            std::chrono::duration_cast<std::chrono::microseconds>(end - start).count() / 1000.0;
    }

    // ===========================================================================
    // run_from_benchmark()
    // Pumps ticks directly into the engine without shared memory (for testing).
    // ===========================================================================
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

    // Signal the event loop to stop (thread-safe)
    void stop() noexcept {
        running_.store(false, std::memory_order_release);
    }

    const EngineStats& stats() const noexcept { return stats_; }
    const LimitOrderBook& lob() const noexcept { return lob_; }
    const AnalyticsEngine& analytics() const noexcept { return analytics_; }

private:
    // ===========================================================================
    // dispatch() — Routes a single tick to every engine
    // This is the HOT PATH — must be as fast as possible.
    // ===========================================================================
    void dispatch(const Tick& tick) {
        switch (tick.type) {
            case OrderType::ADD: {
                lob_.add_order(tick);
                analytics_.process(tick, lob_);
                spoof_.record_add(tick.order_id, tick.price, tick.quantity, tick.timestamp_ns);
                stats_.total_adds++;
                break;
            }
            case OrderType::CANCEL: {
                lob_.cancel_order(tick.order_id, tick.side);
                analytics_.process(tick, lob_);
                auto alert = spoof_.check_cancel(
                    tick.order_id, tick.price, tick.quantity, tick.timestamp_ns);
                if (alert.is_suspected_spoof) [[unlikely]] {
                    stats_.spoof_alerts++;
                    if (cfg_.verbose) {
                        std::cerr << "[SPOOF ALERT] order=" << tick.order_id
                                  << " price=" << tick.price
                                  << " cancel_ratio=" << alert.cancel_ratio << "\n";
                    }
                }
                stats_.total_cancels++;
                break;
            }
            case OrderType::EXECUTE: {
                lob_.execute_order(tick.order_id, tick.quantity, tick.side);
                analytics_.process(tick, lob_);
                hawkes_.record_event(static_cast<double>(tick.timestamp_ns));
                pin_.record_trade(tick.side);
                stats_.total_executes++;
                break;
            }
            default: [[unlikely]] break; // Unknown tick type — silently drop
        }
    }

    // ===========================================================================
    // emit_snapshot() — Build and broadcast a JSON snapshot
    // ===========================================================================
    void emit_snapshot() {
        double ts_now = static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch()
            ).count()
        );

        EngineSnapshot snap{};
        snap.timestamp_ns                       = static_cast<uint64_t>(ts_now);
        snap.ofi                                = analytics_.get_ofi();
        snap.ofi_normalized                     = analytics_.get_ofi_normalized();
        snap.vwap                               = analytics_.get_vwap();
        snap.best_bid                           = lob_.get_best_bid();
        snap.best_ask                           = lob_.get_best_ask();
        snap.spread                             = analytics_.get_spread(lob_);
        snap.hawkes_intensity                   = hawkes_.compute_intensity(ts_now);
        snap.hawkes_next_event_probability_1s   = hawkes_.probability_next_event_within(1e9);
        snap.hawkes_critical                    = hawkes_.is_critical();
        snap.pin_score                          = pin_.estimate_pin();
        snap.pin_level                          = pin_.pin_level();
        snap.flash_crash_probability            = graph_.compute_flash_crash_probability(
                                                    {}, lob_.get_best_ask(), 20);
        snap.arbitrage_opportunity              = false; // computed on-demand, not per-tick
        snap.arbitrage_profit_factor            = 1.0;
        snap.spoofing_detected                  = (stats_.spoof_alerts > 0);
        snap.spoofing_cancel_ratio              = (stats_.total_cancels > 0)
                                                    ? (double)stats_.spoof_alerts / stats_.total_cancels
                                                    : 0.0;
        // Market impact: simulate a 10,000-share buy
        snap.impact_avg_fill_price              = 0.0;
        snap.impact_slippage_bps                = 0.0;
        snap.impact_fully_filled                = false;

        std::string json = snapshot_to_json(snap);
        stats_.snapshots_emitted++;

        // Broadcast to all registered sinks
        for (auto& cb : callbacks_) {
            cb(json);
        }
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

    std::vector<SnapshotCallback> callbacks_;
};

} // namespace mxray
