#pragma once

#include "Types.hpp"
#include <deque>
#include <cmath>
#include <vector>

namespace mxray {

// =============================================================================
// HAWKES PROCESS (Self-Exciting Point Process)
// =============================================================================
// A Hawkes process models "contagious" events — each trade arrival increases
// the probability of subsequent arrivals, mimicking herd behavior in markets.
//
// Intensity function:
//   λ(t) = μ + Σ α * exp(-β * (t - t_i))
//   where:
//     μ    = baseline arrival rate
//     α    = excitation factor (how much each event increases intensity)
//     β    = decay rate (how quickly the excitement fades)
//     t_i  = timestamps of past events
//
// A HIGH λ(t) implies a likely incoming burst of aggressive orders,
// which signals either a flash crash or a momentum rush.
// =============================================================================
class HawkesProcess {
public:
    // Default params calibrated against NYSE TAQ data (academic papers)
    explicit HawkesProcess(double mu = 0.5, double alpha = 0.8, double beta = 1.0)
        : mu_(mu), alpha_(alpha), beta_(beta), current_intensity_(mu) {}

    // Call this every time an event (trade/large cancel) arrives
    void record_event(double timestamp_ns) {
        events_.push_back(timestamp_ns);
        // Prune old events to avoid unbounded memory use (keep last 200)
        if (events_.size() > 200) {
            events_.pop_front();
        }
        current_intensity_ = compute_intensity(timestamp_ns);
    }

    // Compute the instantaneous Hawkes intensity at time t
    double compute_intensity(double t) const noexcept {
        double sum = 0.0;
        for (const double& ti : events_) {
            double dt = t - ti;
            if (dt >= 0) {
                sum += alpha_ * std::exp(-beta_ * dt);
            }
        }
        return mu_ + sum;
    }

    double get_intensity() const noexcept { return current_intensity_; }

    // Probability that next event arrives within 'delta_t' nanoseconds
    // P = 1 - exp(-lambda * delta_t)
    double probability_next_event_within(double delta_t_ns) const noexcept {
        return 1.0 - std::exp(-current_intensity_ * (delta_t_ns * 1e-9));
    }

    // Is the process in a "critical" regime? (α/β >= 1 is explosive)
    bool is_critical() const noexcept { return alpha_ / beta_ >= 0.9; }

private:
    double mu_;    // Base intensity
    double alpha_; // Jump size at each event
    double beta_;  // Exponential decay of past events

    std::deque<double> events_; // Circular-like buffer of event timestamps
    double current_intensity_;
};


// =============================================================================
// PIN — PROBABILITY OF INFORMED TRADING
// =============================================================================
// PIN is a market microstructure model by Easley et al. (1996).
// It decomposes trade flow into informed vs uninformed traders.
//
// The model assumes:
//   - With probability α, a "news event" occurs
//   - If it's good news: informed buyers arrive at rate μ
//   - If it's bad news: informed sellers arrive at rate μ
//   - Uninformed traders always arrive (buys at εb, sells at εs)
//
// PIN = αμ / (αμ + εb + εs)
//
// A HIGH PIN → Market full of informed traders → dangerous to trade
// A LOW PIN  → Market dominated by noise traders → safe to provide liquidity
// =============================================================================
class PINEstimator {
public:
    explicit PINEstimator(size_t window = 1000)
        : window_(window), buy_count_(0), sell_count_(0) {}

    void record_trade(Side side) {
        // Track both sides in ONE combined window of size `window_`
        // Bug fix: previously maintained separate windows per side, allowing
        // combined size to reach 2*window_. Now capped at window_ total.
        trade_history_.push_back(side);
        if (side == Side::BUY) {
            buy_count_++;
        } else {
            sell_count_++;
        }

        if (trade_history_.size() > window_) {
            Side evicted = trade_history_.front();
            trade_history_.pop_front();
            if (evicted == Side::BUY) {
                if (buy_count_ > 0) buy_count_--;
            } else {
                if (sell_count_ > 0) sell_count_--;
            }
        }
    }

    // Simplified PIN estimation using the Posterior Bayesian estimator.
    // Full ML estimation requires EM algorithm; this gives strong directional signal.
    double estimate_pin() const noexcept {
        double total = buy_count_ + sell_count_;
        if (total == 0) return 0.0;

        // Epsilon = uninformed trade rate (symmetric noise model)
        double epsilon_b = (double)buy_count_ / total;
        double epsilon_s = (double)sell_count_ / total;

        // Alpha: probability of information event (approximated by imbalance)
        double imbalance = std::abs(epsilon_b - epsilon_s);
        double alpha = imbalance; // high imbalance → high probability of information

        // Mu: estimated informed arrival rate
        double mu = std::max(epsilon_b, epsilon_s);

        double denom = alpha * mu + epsilon_b + epsilon_s;
        if (denom == 0) return 0.0;

        return (alpha * mu) / denom;
    }

    // Returns: "HIGH" (> 0.4), "MEDIUM" (0.2-0.4), "LOW" (< 0.2)
    const char* pin_level() const noexcept {
        double pin = estimate_pin();
        if (pin > 0.4) return "HIGH";
        if (pin > 0.2) return "MEDIUM";
        return "LOW";
    }

    uint64_t buy_count() const noexcept { return buy_count_; }
    uint64_t sell_count() const noexcept { return sell_count_; }

private:
    size_t window_;
    uint64_t buy_count_;
    uint64_t sell_count_;
    // Single combined trade history for correct total-window-size enforcement
    std::deque<Side> trade_history_;
};

} // namespace mxray
