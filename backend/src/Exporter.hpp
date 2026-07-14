#pragma once

#include "Types.hpp"
#include "AnalyticsEngine.hpp"
#include "HawkesPIN.hpp"
#include "GraphAlgos.hpp"
#include "LimitOrderBook.hpp"
#include <string>
#include <sstream>
#include <iomanip>
#include <vector>

namespace mxray {

// Snapshot of all metrics at a point in time
struct EngineSnapshot {
    uint64_t timestamp_ns;
    uint64_t source_timestamp_ns;
    
    // Microstructure
    double ofi;
    double ofi_normalized;
    double vwap;
    uint32_t best_bid;
    uint32_t best_ask;
    double spread;
    
    // Hawkes process
    double hawkes_intensity;
    double hawkes_next_event_probability_1s;
    bool hawkes_critical;

    // PIN
    double pin_score;
    const char* pin_level;
    
    // Market impact (simulated 10,000-share market buy)
    double impact_avg_fill_price;
    double impact_slippage_bps;
    bool impact_fully_filled;
    uint32_t impact_levels_consumed;

    // Graph / Risk
    double flash_crash_probability;
    bool arbitrage_opportunity;
    double arbitrage_profit_factor;
    std::vector<int> arbitrage_cycle;   // asset index path of the detected cycle

    // Spoofing — summary + scored cancellation signals for the scatter plot
    bool spoofing_detected;
    double spoofing_cancel_ratio;
    struct SpoofingEntry {
        uint32_t price;
        uint64_t quantity;
        double   lifetime_ms;   // time between ADD and CANCEL for the order
        double   score;         // cancel_ratio at time of cancellation
    };
    std::vector<SpoofingEntry> spoofing_orders;

    // Actual executions observed since the previous snapshot.
    struct ExecutionEntry {
        uint64_t timestamp_ns;
        uint32_t price;
        uint32_t quantity;
        Side side;
    };
    std::vector<ExecutionEntry> executions;

    // Limit Order Book depth (top 50 non-empty levels per side)
    struct BookLevel {
        uint32_t price;
        uint64_t volume;
    };
    std::vector<BookLevel> book_bids;
    std::vector<BookLevel> book_asks;
};

// =============================================================================
// JSON SERIALIZER — for streaming metrics to the React frontend
// Outputs a compact JSON string suitable for WebSocket transmission.
// =============================================================================
inline std::string snapshot_to_json(const EngineSnapshot& s) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(6);
    oss << "{"
        << "\"timestamp_ns\":" << s.timestamp_ns << ","
        << "\"source_timestamp_ns\":" << s.source_timestamp_ns << ","
        << "\"microstructure\":{"
            << "\"ofi\":"            << s.ofi            << ","
            << "\"ofi_normalized\":" << s.ofi_normalized << ","
            << "\"vwap\":"           << s.vwap           << ","
            << "\"best_bid\":"       << s.best_bid       << ","
            << "\"best_ask\":"       << s.best_ask       << ","
            << "\"spread_ticks\":"   << s.spread
        << "},"
        << "\"hawkes\":{"
            << "\"intensity\":"        << s.hawkes_intensity                 << ","
            << "\"p_next_event_1s\":"  << s.hawkes_next_event_probability_1s << ","
            << "\"critical\":"         << (s.hawkes_critical ? "true" : "false")
        << "},"
        << "\"pin\":{"
            << "\"score\":"  << s.pin_score  << ","
            << "\"level\":\"" << s.pin_level << "\""
        << "},"
        << "\"market_impact\":{"
            << "\"avg_fill_price\":"  << s.impact_avg_fill_price  << ","
            << "\"slippage_bps\":"    << s.impact_slippage_bps    << ","
            << "\"fully_filled\":"    << (s.impact_fully_filled ? "true" : "false") << ","
            << "\"levels_consumed\":" << s.impact_levels_consumed
        << "},"
        << "\"risk\":{"
            << "\"flash_crash_probability\":" << s.flash_crash_probability << ","
            << "\"arbitrage_opportunity\":"   << (s.arbitrage_opportunity ? "true" : "false") << ","
            << "\"arbitrage_profit_factor\":" << s.arbitrage_profit_factor << ","
            << "\"arbitrage_cycle\":[";
    for (size_t i = 0; i < s.arbitrage_cycle.size(); ++i) {
        if (i) oss << ",";
        oss << s.arbitrage_cycle[i];
    }
    oss         << "],"
            << "\"spoofing_detected\":"     << (s.spoofing_detected ? "true" : "false") << ","
            << "\"spoofing_cancel_ratio\":"  << s.spoofing_cancel_ratio
        << "},"
        << "\"spoofing\":{"
            << "\"is_active\":"    << (s.spoofing_detected ? "true" : "false") << ","
            << "\"cancel_ratio\":" << s.spoofing_cancel_ratio << ","
            << "\"suspicious_orders\":[";
    for (size_t i = 0; i < s.spoofing_orders.size(); ++i) {
        if (i) oss << ",";
        const auto& o = s.spoofing_orders[i];
        oss << "{"
            << "\"price\":"       << o.price       << ","
            << "\"quantity\":"    << o.quantity    << ","
            << "\"lifetime_ms\":" << o.lifetime_ms << ","
            << "\"score\":"       << o.score
            << "}";
    }
    oss     << "]"
        << "},"
        << "\"executions\":[";
    for (size_t i = 0; i < s.executions.size(); ++i) {
        if (i) oss << ",";
        const auto& execution = s.executions[i];
        oss << "{"
            << "\"timestamp_ns\":" << execution.timestamp_ns << ","
            << "\"price\":" << execution.price << ","
            << "\"quantity\":" << execution.quantity << ","
            << "\"side\":\"" << (execution.side == Side::BUY ? "BUY" : "SELL") << "\""
            << "}";
    }
    oss     << "],"
        << "\"book_depth\":{"
            << "\"bids\":[";
    for (size_t i = 0; i < s.book_bids.size(); ++i) {
        if (i) oss << ",";
        oss << "[" << s.book_bids[i].price << "," << s.book_bids[i].volume << "]";
    }
    oss         << "],"
            << "\"asks\":[";
    for (size_t i = 0; i < s.book_asks.size(); ++i) {
        if (i) oss << ",";
        oss << "[" << s.book_asks[i].price << "," << s.book_asks[i].volume << "]";
    }
    oss         << "]"
        << "}"
        << "}";
    return oss.str();
}

} // namespace mxray
