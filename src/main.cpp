// =============================================================================
// main.cpp — MarketXray C++ Analytics Engine
// =============================================================================
// Supports two modes:
//   --benchmark   : pump 1M mock ticks and report performance metrics
//   --daemon      : attach to shared memory and run forever (live trading mode)
//
// Usage:
//   ./market_xray --benchmark
//   ./market_xray --daemon
//   ./market_xray --daemon --shm /marketxray_shm
// =============================================================================

#include "EngineLoop.hpp"
#include "SharedMemory.hpp"
#include "LimitOrderBook.hpp"
#include "AnalyticsEngine.hpp"
#include "HawkesPIN.hpp"
#include "GraphAlgos.hpp"
#include "Exporter.hpp"

#include <iostream>
#include <iomanip>
#include <random>
#include <chrono>
#include <vector>
#include <string>
#include <csignal>
#include <thread>

using namespace mxray;
using namespace std::chrono;

// ============================================================================
// Global running flag — set to false by SIGINT/SIGTERM for clean shutdown
// ============================================================================
static std::atomic<bool> g_running{true};
static EngineLoop* g_engine_ptr = nullptr;

void signal_handler(int) {
    std::cout << "\n[Engine] Shutdown signal received. Stopping...\n";
    g_running.store(false, std::memory_order_release);
    if (g_engine_ptr) g_engine_ptr->stop();
}

// ============================================================================
// Mock tick generator — for benchmark mode only
// Distributions constructed ONCE (BUG FIX from audit).
// ============================================================================
struct TickDist {
    std::uniform_int_distribution<uint32_t> price{10000, 20000};
    std::uniform_int_distribution<uint32_t> qty  {1, 1000};
    std::uniform_int_distribution<int>      side {0, 1};
    std::uniform_int_distribution<int>      type {0, 4};
};

Tick generate_tick(uint64_t id, std::mt19937_64& rng, TickDist& d) {
    Tick t{};
    t.order_id    = id;
    t.timestamp_ns = static_cast<uint64_t>(
        duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count());
    t.price    = d.price(rng);
    t.quantity = d.qty(rng);
    t.side     = (d.side(rng) == 0) ? Side::BUY : Side::SELL;
    int r      = d.type(rng);
    t.type     = (r <= 2) ? OrderType::ADD : (r == 3) ? OrderType::CANCEL : OrderType::EXECUTE;
    return t;
}

void print_sep() {
    std::cout << "\n" << std::string(70, '=') << "\n";
}

// ============================================================================
// BENCHMARK MODE
// ============================================================================
void run_benchmark() {
    std::cout << R"(
  __  __            _        _    __  __
 |  \/  | __ _ _ __| | _____| |_  \ \/ / _ __ __ _ _   _
 | |\/| |/ _` | '__| |/ / _ \ __|  \  / | '__/ _` | | | |
 | |  | | (_| | |  |   <  __/ |_   /  \ | | | (_| | |_| |
 |_|  |_|\__,_|_|  |_|\_\___|\__| /_/\_\|_|  \__,_|\__, |
                                                      |___/
  C++ HFT Analytics Engine — Beast Mode v2.0 [BENCHMARK]
)";

    const size_t N = 1000000;
    print_sep();
    std::cout << "[BENCH] Generating " << N << " mock ticks...\n";

    std::mt19937_64 rng(42);
    TickDist dist;
    std::vector<Tick> ticks;
    ticks.reserve(N);
    for (size_t i = 1; i <= N; ++i) {
        ticks.push_back(generate_tick(i, rng, dist));
    }

    EngineConfig cfg;
    cfg.snapshot_every_n = 100000; // emit 10 snapshots during benchmark
    cfg.verbose = false;

    EngineLoop engine(cfg);

    // Register stdout sink for snapshots
    engine.on_snapshot([](const std::string& json) {
        std::cout << "[SNAPSHOT] " << json.substr(0, 120) << "...\n";
    });

    std::cout << "[BENCH] Running engine...\n";
    engine.run_from_benchmark(ticks);

    const auto& s = engine.stats();
    print_sep();
    std::cout << "[PERFORMANCE]\n";
    std::cout << std::fixed << std::setprecision(3);
    std::cout << "  Total ticks:      " << s.total_ticks    << "\n";
    std::cout << "  ADDs:             " << s.total_adds     << "\n";
    std::cout << "  CANCELs:          " << s.total_cancels  << "\n";
    std::cout << "  EXECUTEs:         " << s.total_executes << "\n";
    std::cout << "  Spoof alerts:     " << s.spoof_alerts   << "\n";
    std::cout << "  Elapsed:          " << s.elapsed_ms     << " ms\n";
    std::cout << "  Throughput:       " << s.throughput_per_sec() / 1e6 << " M ticks/sec\n";
    std::cout << "  Avg latency/tick: " << s.avg_latency_ns() << " ns\n";

    print_sep();
    const auto& lob = engine.lob();
    const auto& ana = engine.analytics();
    std::cout << "[ORDER BOOK]\n";
    std::cout << "  Best Bid: $" << lob.get_best_bid() / 100.0 << "\n";
    std::cout << "  Best Ask: $" << lob.get_best_ask() / 100.0 << "\n";
    std::cout << "  Spread:   " << ana.get_spread(lob) << " ticks\n";

    print_sep();
    std::cout << "[MICROSTRUCTURE]\n";
    std::cout << "  OFI:            " << ana.get_ofi()            << "\n";
    std::cout << "  OFI Normalized: " << ana.get_ofi_normalized() << "\n";
    std::cout << "  VWAP:           $" << ana.get_vwap() / 100.0  << "\n";

    print_sep();
    // Arbitrage demo
    std::cout << "[ARBITRAGE — BELLMAN-FORD]\n";
    GraphEngine graph;
    std::vector<std::vector<double>> rates = {
        {1.000, 1.300, 1.750},
        {0.769, 1.000, 1.320},
        {0.593, 0.769, 1.000}
    };
    auto arb = graph.detect_arbitrage(rates, 3);
    std::cout << "  Opportunity: " << (arb.opportunity_found ? "YES 🚨" : "No") << "\n";
    if (arb.opportunity_found) {
        std::cout << "  Profit:      " << arb.profit_factor << "x\n";
    }

    print_sep();
    std::cout << "✅ Benchmark complete.\n";
}

// ============================================================================
// DAEMON MODE — Real Shared Memory Live Trading
// ============================================================================
void run_daemon(const std::string& shm_name) {
    std::cout << R"(
  __  __            _        _    __  __
 |  \/  | __ _ _ __| | _____| |_  \ \/ / _ __ __ _ _   _
 | |\/| |/ _` | '__| |/ / _ \ __|  \  / | '__/ _` | | | |
 | |  | | (_| | |  |   <  __/ |_   /  \ | | | (_| | |_| |
 |_|  |_|\__,_|_|  |_|\_\___|\__| /_/\_\|_|  \__,_|\__, |
                                                      |___/
  C++ HFT Analytics Engine — Beast Mode v2.0 [DAEMON — SHARED MEMORY]
)";

    std::cout << "[Daemon] SHM region: " << shm_name << "\n";
    std::cout << "[Daemon] Press Ctrl+C to stop.\n\n";

    EngineConfig cfg;
    cfg.snapshot_every_n = 1000; // emit snapshot every 1000 live ticks
    cfg.verbose = true;

    EngineLoop engine(cfg);
    g_engine_ptr = &engine;

    // Register snapshot sink: prints JSON to stdout.
    // In production: replace with WebSocket::send(json) call.
    engine.on_snapshot([](const std::string& json) {
        std::cout << json << "\n";
        std::cout.flush();
    });

    // Install signal handlers for clean shutdown
    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Blocks until stop() is called
    engine.run_from_shared_memory(shm_name);

    const auto& s = engine.stats();
    print_sep();
    std::cout << "[Daemon] Shutdown complete.\n";
    std::cout << "  Total ticks processed: " << s.total_ticks      << "\n";
    std::cout << "  Snapshots emitted:     " << s.snapshots_emitted << "\n";
    std::cout << "  Spoof alerts:          " << s.spoof_alerts      << "\n";
    std::cout << "  Throughput:            " << s.throughput_per_sec() / 1e6 << " M ticks/sec\n";
    g_engine_ptr = nullptr;
}

// ============================================================================
// Entry Point
// ============================================================================
int main(int argc, char* argv[]) {
    std::string mode    = "--benchmark";
    std::string shm_name = SharedMemoryManager::DEFAULT_SHM_NAME;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--daemon")    mode = "--daemon";
        if (arg == "--benchmark") mode = "--benchmark";
        if (arg == "--shm" && i + 1 < argc) {
            shm_name = argv[++i];
        }
    }

    if (mode == "--daemon") {
        run_daemon(shm_name);
    } else {
        run_benchmark();
    }

    return 0;
}
