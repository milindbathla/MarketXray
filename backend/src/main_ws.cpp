
#include "EngineLoop.hpp"
#include "SharedMemory.hpp"
#include "WebSocketServer.hpp"

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>

using namespace mxray;

static std::atomic<bool> g_running{true};
static EngineLoop*       g_engine_ptr = nullptr;

void signal_handler(int) {
    std::cout << "\n[WS Daemon] Shutdown signal received. Stopping...\n";
    g_running.store(false, std::memory_order_release);
    if (g_engine_ptr) g_engine_ptr->stop();
}

int main(int argc, char* argv[]) {
    std::string shm_name = SharedMemoryManager::DEFAULT_SHM_NAME;
    uint16_t    port     = 9001;
    uint32_t    snapshot_every_n = 50;
    uint32_t    min_snapshot_ms  = 100;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--shm"  && i + 1 < argc) shm_name = argv[++i];
        else if (arg == "--port" && i + 1 < argc) port = static_cast<uint16_t>(std::stoi(argv[++i]));
        else if (arg == "--snapshot-every" && i + 1 < argc) {
            snapshot_every_n = static_cast<uint32_t>(std::stoul(argv[++i]));
        }
        else if (arg == "--snapshot-ms" && i + 1 < argc) {
            min_snapshot_ms = static_cast<uint32_t>(std::stoul(argv[++i]));
        }
        if (arg == "--help") {
            std::cout << "Usage: market_xray_ws [--port N] [--shm NAME]\n"
                      << "  --port N    WebSocket port (default: 9001)\n"
                      << "  --shm NAME  POSIX SHM region name (default: /marketxray_shm)\n"
                      << "  --snapshot-every N  Emit one snapshot per N ticks (default: 50)\n"
                      << "  --snapshot-ms N     Minimum milliseconds between WebSocket snapshots (default: 100)\n";
            return 0;
        }
    }

    if (snapshot_every_n == 0) {
        std::cerr << "[WS Daemon] --snapshot-every must be greater than 0.\n";
        return 1;
    }

    std::cout << R"(
  __  __            _        _    __  __
 |  \/  | __ _ _ __| | _____| |_  \ \/ / _ __ __ _ _   _
 | |\/| |/ _` | '__| |/ / _ \ __|  \  / | '__/ _` | | | |
 | |  | | (_| | |  |   <  __/ |_   /  \ | | | (_| | |_| |
 |_|  |_|\__,_|_|  |_|\_\___|\__| /_/\_\|_|  \__,_|\__, |
                                                      |___/
  C++ HFT Analytics Engine — WebSocket Broadcaster
)";

    std::cout << "[WS Daemon] SHM region : " << shm_name << "\n";
    std::cout << "[WS Daemon] WebSocket   : ws://0.0.0.0:" << port << "\n";
    std::cout << "[WS Daemon] Snapshots   : every " << snapshot_every_n << " ticks";
    if (min_snapshot_ms > 0) {
        std::cout << ", paced at >= " << min_snapshot_ms << " ms";
    } else {
        std::cout << ", unpaced burst mode";
    }
    std::cout << "\n";
    std::cout << "[WS Daemon] Press Ctrl+C to stop.\n\n";

    WebSocketServer ws_server(port);
    if (!ws_server.start()) {
        std::cerr << "[WS Daemon] Failed to start WebSocket server. Exiting.\n";
        return 1;
    }

    EngineConfig cfg;
    cfg.snapshot_every_n = snapshot_every_n;
    cfg.verbose          = false;  // no per-tick stderr noise

    EngineLoop engine(cfg);
    g_engine_ptr = &engine;

    uint64_t broadcast_count = 0;
    auto last_broadcast = std::chrono::steady_clock::now();
    engine.on_snapshot([&](const std::string& json) {
        auto now = std::chrono::steady_clock::now();
        if (min_snapshot_ms > 0 && broadcast_count > 0) {
            const auto min_interval = std::chrono::milliseconds(min_snapshot_ms);
            if (now < last_broadcast + min_interval) {
                return;
            }
        }

        ws_server.broadcast(json);
        last_broadcast = now;
        ++broadcast_count;

        if (broadcast_count % 100 == 0) {
            std::cout << "[WS Daemon] " << broadcast_count
                      << " snapshots broadcast | "
                      << ws_server.client_count() << " client(s) connected\n";
            std::cout.flush();
        }
    });

    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);

    engine.run_from_shared_memory(shm_name);

    const auto& s = engine.stats();
    std::cout << "\n" << std::string(70, '=') << "\n";
    std::cout << "[WS Daemon] Shutdown complete.\n";
    std::cout << std::fixed << std::setprecision(3);
    std::cout << "  Total ticks processed : " << s.total_ticks       << "\n";
    std::cout << "  Snapshots broadcast   : " << s.snapshots_emitted << "\n";
    std::cout << "  Spoof alerts          : " << s.spoof_alerts      << "\n";
    std::cout << "  Throughput            : "
              << s.throughput_per_sec() / 1e6 << " M ticks/sec\n";

    ws_server.stop();
    g_engine_ptr = nullptr;
    return 0;
}
