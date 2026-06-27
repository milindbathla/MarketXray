#pragma once

#include "Types.hpp"
#include "AnalyticsEngine.hpp"
#include "HawkesPIN.hpp"
#include "GraphAlgos.hpp"
#include "LimitOrderBook.hpp"
#include <string>
#include <sstream>
#include <iomanip>

namespace mxray {

// Snapshot of all metrics at a point in time
struct EngineSnapshot {
    uint64_t timestamp_ns;
    
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

    // Graph / Risk
    double flash_crash_probability;
    bool arbitrage_opportunity;
    double arbitrage_profit_factor;

    // Spoofing
    bool spoofing_detected;
    double spoofing_cancel_ratio;
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
        << "\"microstructure\":{"
            << "\"ofi\":" << s.ofi << ","
            << "\"ofi_normalized\":" << s.ofi_normalized << ","
            << "\"vwap\":" << s.vwap << ","
            << "\"best_bid\":" << s.best_bid << ","
            << "\"best_ask\":" << s.best_ask << ","
            << "\"spread_ticks\":" << s.spread
        << "},"
        << "\"hawkes\":{"
            << "\"intensity\":" << s.hawkes_intensity << ","
            << "\"p_next_event_1s\":" << s.hawkes_next_event_probability_1s << ","
            << "\"critical\":" << (s.hawkes_critical ? "true" : "false")
        << "},"
        << "\"pin\":{"
            << "\"score\":" << s.pin_score << ","
            << "\"level\":\"" << s.pin_level << "\""
        << "},"
        << "\"market_impact\":{"
            << "\"avg_fill_price\":" << s.impact_avg_fill_price << ","
            << "\"slippage_bps\":" << s.impact_slippage_bps << ","
            << "\"fully_filled\":" << (s.impact_fully_filled ? "true" : "false")
        << "},"
        << "\"risk\":{"
            << "\"flash_crash_probability\":" << s.flash_crash_probability << ","
            << "\"arbitrage_opportunity\":" << (s.arbitrage_opportunity ? "true" : "false") << ","
            << "\"arbitrage_profit_factor\":" << s.arbitrage_profit_factor << ","
            << "\"spoofing_detected\":" << (s.spoofing_detected ? "true" : "false") << ","
            << "\"spoofing_cancel_ratio\":" << s.spoofing_cancel_ratio
        << "}"
        << "}";
    return oss.str();
}

} // namespace mxray
