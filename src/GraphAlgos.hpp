#pragma once

#include "Types.hpp"
#include "LimitOrderBook.hpp"
#include <vector>
#include <unordered_map>
#include <deque>
#include <cmath>
#include <algorithm>
#include <limits>
#include <string>
#include <stdexcept>

namespace mxray {

// =============================================================================
// MARKET IMPACT SIMULATOR — "Walk the Book"
// =============================================================================
// Simulates what price you would actually get if you tried to execute a large
// market order right NOW against the existing order book.
// The impact = the slippage between the best bid/ask and the average fill price.
// =============================================================================
struct MarketImpactResult {
    double average_fill_price;    // Average price of all fills
    double slippage_bps;          // Slippage in basis points (1 bps = 0.01%)
    uint32_t total_qty_filled;    // How much we actually got filled
    uint32_t levels_consumed;     // How many price levels we "walked through"
    bool fully_filled;            // Did we get the full order filled?
};

// =============================================================================
// LIQUIDITY CONTAGION GRAPH — Flash Crash Predictor
// =============================================================================
// Models each price level as a node and the volume at each level as the node weight.
// "Contagion" = if top-of-book volume is very thin, cancellations at the top
// can cascade and propagate through the graph, causing a flash crash.
//
// We detect this via a "Centrality Deficit" score:
//   - High liquidity at many levels  → low crash risk
//   - Thin top, heavy tail            → high crash risk
// =============================================================================
struct GraphNode {
    uint32_t price;
    uint64_t volume;
    double centrality; // Betweenness-approximation
};

class GraphEngine {
public:
    // Walk the book to compute market impact for a hypothetical order of 'qty' shares
    MarketImpactResult simulate_market_impact(
        const LimitOrderBook& lob,
        const std::vector<PriceLevel>& levels, // pass bids for SELL, asks for BUY
        uint32_t qty,
        Side aggressor_side) const {
        
        MarketImpactResult result{0.0, 0.0, 0, 0, false};

        if (levels.empty()) return result;

        uint32_t remaining = qty;
        double notional_filled = 0.0;
        uint32_t best_price = (aggressor_side == Side::BUY) 
            ? lob.get_best_ask() 
            : lob.get_best_bid();

        for (const auto& level : levels) {
            if (remaining == 0) break;
            if (level.head == nullptr) continue;

            uint32_t fill_qty = std::min(remaining, (uint32_t)level.total_volume);
            notional_filled += (double)level.price * fill_qty;
            result.total_qty_filled += fill_qty;
            remaining -= fill_qty;
            result.levels_consumed++;
        }

        if (result.total_qty_filled > 0) {
            result.average_fill_price = notional_filled / result.total_qty_filled;

            // Slippage in bps = |fill_price - best_price| / best_price * 10000
            if (best_price > 0) {
                result.slippage_bps = std::abs(result.average_fill_price - best_price) 
                                      / (double)best_price * 10000.0;
            }
        }
        result.fully_filled = (remaining == 0);
        return result;
    }

    // =============================================================================
    // FLASH CRASH PROBABILITY SCORING
    // Uses a lightweight Liquidity Contagion model:
    //   score = 1 - (top_10_volume / total_volume)^0.5
    // If the top 10 levels are thin relative to total book depth, crash risk is high.
    // =============================================================================
    double compute_flash_crash_probability(
        const std::vector<PriceLevel>& ask_levels,
        uint32_t best_ask,
        uint32_t depth = 20) const noexcept {
        
        if (ask_levels.empty() || best_ask == 0) return 0.0;

        uint64_t top_volume = 0;
        uint64_t total_volume = 0;
        uint32_t count = 0;

        for (uint32_t price = best_ask; price < best_ask + depth && price < ask_levels.size(); ++price) {
            if (ask_levels[price].head != nullptr) {
                top_volume += ask_levels[price].total_volume;
                count++;
            }
        }

        // Total volume across first 200 levels
        for (uint32_t price = best_ask; price < best_ask + 200 && price < ask_levels.size(); ++price) {
            total_volume += ask_levels[price].total_volume;
        }

        if (total_volume == 0) return 0.0;
        double ratio = (double)top_volume / (double)total_volume;
        
        // Low ratio = thin top of book = high crash risk
        return std::clamp(1.0 - std::sqrt(ratio), 0.0, 1.0);
    }

    // =============================================================================
    // BELLMAN-FORD NEGATIVE CYCLE DETECTION — Triangular Arbitrage
    // Detects if a profitable cycle exists across N assets.
    // edge_weight[i][j] = -log(exchange_rate[i][j])
    // If there's a negative cycle, it means a "free money" arbitrage path exists.
    // =============================================================================
    struct ArbitrageResult {
        bool opportunity_found;
        std::vector<int> cycle;   // Indices of the arbitrage cycle
        double profit_factor;     // How many times initial investment is returned
    };

    ArbitrageResult detect_arbitrage(
        const std::vector<std::vector<double>>& exchange_rates, 
        int n_assets) const {
        
        ArbitrageResult result{false, {}, 1.0};

        // Convert to log-space for additive path-finding
        std::vector<std::vector<double>> w(n_assets, std::vector<double>(n_assets, 1e9));
        for (int i = 0; i < n_assets; ++i) {
            for (int j = 0; j < n_assets; ++j) {
                if (i != j && exchange_rates[i][j] > 0) {
                    w[i][j] = -std::log(exchange_rates[i][j]);
                }
            }
        }

        // Standard Bellman-Ford from source node 0
        std::vector<double> dist(n_assets, 0.0); // Start from "all 0" to detect any cycle
        std::vector<int> pred(n_assets, -1);

        for (int iter = 0; iter < n_assets - 1; ++iter) {
            for (int u = 0; u < n_assets; ++u) {
                for (int v = 0; v < n_assets; ++v) {
                    if (w[u][v] < 1e9 && dist[u] + w[u][v] < dist[v]) {
                        dist[v] = dist[u] + w[u][v];
                        pred[v] = u;
                    }
                }
            }
        }

        // N-th iteration: if we can still relax, there's a negative cycle
        for (int u = 0; u < n_assets; ++u) {
            for (int v = 0; v < n_assets; ++v) {
                if (w[u][v] < 1e9 && dist[u] + w[u][v] < dist[v]) {
                    // Trace the cycle back
                    result.opportunity_found = true;
                    std::vector<bool> visited(n_assets, false);
                    int curr = v;
                    // BUGFIX: Guard against pred[curr] == -1 (unvisited node with no predecessor)
                    // which previously caused undefined behavior via pred[-1] access.
                    while (curr != -1 && !visited[curr]) {
                        visited[curr] = true;
                        result.cycle.push_back(curr);
                        curr = pred[curr];
                    }
                    if (curr != -1) result.cycle.push_back(curr); // close the cycle
                    std::reverse(result.cycle.begin(), result.cycle.end());

                    // Compute profit factor
                    result.profit_factor = 1.0;
                    for (size_t k = 0; k + 1 < result.cycle.size(); ++k) {
                        int a = result.cycle[k];
                        int b = result.cycle[k + 1];
                        if (a >= 0 && b >= 0) {
                            result.profit_factor *= exchange_rates[a][b];
                        }
                    }
                    return result;
                }
            }
        }
        return result;
    }
};

// =============================================================================
// SPOOFING / LAYERING DETECTOR
// =============================================================================
// Real spoofing pattern: large orders appear at multiple price levels to
// create the illusion of depth, then all get canceled just before execution.
// We detect this by tracking: large ADD followed by fast CANCEL at same price cluster.
// =============================================================================
struct SpoofingAlert {
    uint64_t suspected_order_id;
    uint32_t price;
    uint64_t quantity;
    double cancel_ratio; // cancellations / adds at this price cluster (0-1)
    bool is_suspected_spoof;
};

class SpoofingDetector {
public:
    explicit SpoofingDetector(size_t window_ns = 500000000ULL, // 500ms
                               double cancel_ratio_threshold = 0.85,
                               uint32_t min_quantity = 5000)
        : window_ns_(window_ns),
          threshold_(cancel_ratio_threshold),
          min_qty_(min_quantity) {}

    void record_add(uint64_t order_id, uint32_t price, uint32_t qty, uint64_t ts_ns) {
        ClusterKey key = price_cluster(price);
        auto& cluster = clusters_[key];
        cluster.add_count++;
        cluster.total_add_volume += qty;
        cluster.orders[order_id] = {qty, ts_ns};
        evict_old(cluster, ts_ns);
    }

    SpoofingAlert check_cancel(uint64_t order_id, uint32_t price, uint32_t qty, uint64_t ts_ns) {
        ClusterKey key = price_cluster(price);
        auto& cluster = clusters_[key];
        cluster.cancel_count++;
        cluster.total_cancel_volume += qty;
        evict_old(cluster, ts_ns);

        double ratio = 0.0;
        if (cluster.add_count > 0) {
            ratio = (double)cluster.cancel_count / (double)cluster.add_count;
        }

        bool suspected = (ratio >= threshold_) && (cluster.total_add_volume >= min_qty_);
        return SpoofingAlert{order_id, price, qty, ratio, suspected};
    }

private:
    using ClusterKey = uint32_t;

    struct OrderRecord {
        uint32_t quantity;
        uint64_t timestamp_ns;
    };

    struct Cluster {
        uint64_t add_count = 0;
        uint64_t cancel_count = 0;
        uint64_t total_add_volume = 0;
        uint64_t total_cancel_volume = 0;
        std::unordered_map<uint64_t, OrderRecord> orders;
    };

    ClusterKey price_cluster(uint32_t price) const noexcept {
        // Cluster prices into buckets of 50 ticks
        return (price / 50) * 50;
    }

    void evict_old(Cluster& cluster, uint64_t current_ts) {
        // Suppress unused-parameter warning; time-based eviction is a prod enhancement.
        // Current implementation uses count-based decay for simplicity.
        (void)current_ts;
        if (cluster.orders.size() > 1000) {
            cluster.orders.clear();
            cluster.add_count /= 2;
            cluster.cancel_count /= 2;
        }
    }

    uint64_t window_ns_;
    double threshold_;
    uint32_t min_qty_;
    std::unordered_map<ClusterKey, Cluster> clusters_;
};

} // namespace mxray
