#pragma once

#include "Types.hpp"
#include <deque>
#include <cmath>
#include <vector>

namespace mxray {

class HawkesProcess {
public:
    explicit HawkesProcess(double mu = 0.5, double alpha = 0.8, double beta = 1.0)
        : mu_(mu), alpha_(alpha), beta_(beta), current_intensity_(mu) {}

    void record_event(double timestamp_s) {
        events_.push_back(timestamp_s);
        if (events_.size() > 200) {
            events_.pop_front();
        }
        current_intensity_ = compute_intensity(timestamp_s);
    }

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

    double probability_next_event_within(double timestamp_s, double delta_t_ns) const noexcept {
        return 1.0 - std::exp(-compute_intensity(timestamp_s) * (delta_t_ns * 1e-9));
    }

    bool is_critical() const noexcept { return alpha_ / beta_ >= 0.9; }

private:
    double mu_;    // Base intensity
    double alpha_; // Jump size at each event
    double beta_;  // Exponential decay of past events

    std::deque<double> events_; // Circular-like buffer of event timestamps
    double current_intensity_;
};


class PINEstimator {
public:
    explicit PINEstimator(size_t window = 1000)
        : window_(window), buy_count_(0), sell_count_(0) {}

    void record_trade(Side side) {
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

    double estimate_pin() const noexcept {
        double total = buy_count_ + sell_count_;
        if (total == 0) return 0.0;

        double epsilon_b = (double)buy_count_ / total;
        double epsilon_s = (double)sell_count_ / total;

        double imbalance = std::abs(epsilon_b - epsilon_s);
        double alpha = imbalance; // high imbalance → high probability of information

        double mu = std::max(epsilon_b, epsilon_s);

        double denom = alpha * mu + epsilon_b + epsilon_s;
        if (denom == 0) return 0.0;

        return (alpha * mu) / denom;
    }

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
    std::deque<Side> trade_history_;
};

} // namespace mxray
