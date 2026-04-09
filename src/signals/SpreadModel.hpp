#pragma once
/**
 * @file SpreadModel.hpp
 * @brief Rolling z-score spread model with optional Kalman-filtered hedge ratio.
 *
 * Two modes of operation:
 *   1. Static beta: spread = log(A) - beta * log(B), z = (spread - mu) / sigma
 *      where mu and sigma are rolling window estimates. Simple and fast.
 *
 *   2. Kalman-filtered beta: hedge ratio adapts online via the Kalman filter
 *      from KalmanFilter.hpp. The z-score uses the Kalman innovation variance
 *      which naturally accounts for parameter estimation uncertainty.
 *
 * Mode 2 is preferred because:
 *   - Beta tracks structural changes (regime shifts, corporate events)
 *   - Z-score uncertainty correctly reflects estimation error
 *   - No arbitrary lookback window for beta estimation
 *
 * The rolling window z-score (mode 1) is still useful as a baseline
 * for comparison and for contexts where simplicity is preferred.
 */

#include <cmath>
#include <deque>

#include "KalmanFilter.hpp"

namespace signals {

/**
 * @brief Rolling z-score spread model with optional Kalman integration.
 *
 * Usage (static beta):
 * @code
 *   SpreadModel model(100, 1.0);
 *   double z = model.update(mid_AAPL, mid_MSFT);
 * @endcode
 *
 * Usage (Kalman-filtered beta):
 * @code
 *   KalmanHedgeRatio kf(0.9999, 1e-3);
 *   SpreadModel model(100);
 *   model.setKalmanFilter(&kf);
 *   double z = model.update(mid_AAPL, mid_MSFT);
 *   double hedgeRatio = model.beta();  // Dynamic!
 * @endcode
 */
class SpreadModel {
public:
    /**
     * @brief Construct spread model.
     * @param lookback Rolling window size for mean/std estimation
     * @param beta Static hedge ratio (ignored if Kalman filter is set)
     */
    explicit SpreadModel(size_t lookback = 100, double beta = 1.0)
        : lookback_(lookback), beta_(beta),
          sum_(0), sumSq_(0), count_(0), spread_(0), z_(0),
          kalman_(nullptr) {}

    /**
     * @brief Attach a Kalman filter for dynamic hedge ratio tracking.
     * When set, the filter's beta replaces the static beta_ on each update.
     * @param kf Pointer to KalmanHedgeRatio (not owned, must outlive this)
     */
    void setKalmanFilter(KalmanHedgeRatio* kf) noexcept { kalman_ = kf; }

    /**
     * @brief Update model with new prices.
     * @param priceA First asset price (e.g., AAPL)
     * @param priceB Second asset price (e.g., MSFT)
     * @return Current z-score (Kalman or rolling, depending on mode)
     */
    double update(double priceA, double priceB) {
        double logA = std::log(priceA);
        double logB = std::log(priceB);

        if (kalman_) {
            // Mode 2: Kalman-filtered hedge ratio
            kalmanState_ = kalman_->update(logA, logB);
            beta_ = kalmanState_.beta;
            spread_ = kalmanState_.spread;  // Innovation = actual - predicted
            z_ = kalmanState_.zScore();     // Uses Kalman uncertainty

            // Also update rolling window for backward compatibility
            updateRollingWindow(spread_);

            return z_;
        }

        // Mode 1: Static beta, rolling z-score
        spread_ = logA - beta_ * logB;
        updateRollingWindow(spread_);

        // Compute rolling z-score
        if (count_ >= 2) {
            double mean = sum_ / static_cast<double>(count_);
            double variance = (sumSq_ / static_cast<double>(count_)) - (mean * mean);
            double std = std::sqrt(std::max(variance, 1e-10));
            z_ = (spread_ - mean) / std;
        } else {
            z_ = 0;
        }

        return z_;
    }

    // ========================================================================
    // Accessors
    // ========================================================================

    [[nodiscard]] double zScore() const noexcept { return z_; }
    [[nodiscard]] double spread() const noexcept { return spread_; }
    [[nodiscard]] double beta() const noexcept { return beta_; }

    [[nodiscard]] double mean() const noexcept {
        return count_ > 0 ? sum_ / static_cast<double>(count_) : 0;
    }

    [[nodiscard]] double stdDev() const noexcept {
        if (count_ < 2) return 0;
        double m = sum_ / static_cast<double>(count_);
        double var = (sumSq_ / static_cast<double>(count_)) - (m * m);
        return std::sqrt(std::max(var, 0.0));
    }

    /**
     * @brief Get Kalman state (only valid if Kalman filter is attached).
     */
    [[nodiscard]] const KalmanState& kalmanState() const noexcept {
        return kalmanState_;
    }

    /**
     * @brief Check if Kalman filter is active.
     */
    [[nodiscard]] bool isKalmanActive() const noexcept { return kalman_ != nullptr; }

    /**
     * @brief Set static hedge ratio (only effective if Kalman is not attached).
     */
    void setBeta(double beta) noexcept { beta_ = beta; }

    /**
     * @brief Reset model state.
     */
    void reset() {
        window_.clear();
        sum_ = sumSq_ = spread_ = z_ = 0;
        count_ = 0;
        kalmanState_ = KalmanState{};
        if (kalman_) kalman_->reset();
    }

private:
    size_t lookback_;
    double beta_;
    std::deque<double> window_;
    double sum_;
    double sumSq_;
    size_t count_;
    double spread_;
    double z_;

    // Kalman filter integration
    KalmanHedgeRatio* kalman_;
    KalmanState kalmanState_{};

    void updateRollingWindow(double value) {
        window_.push_back(value);
        sum_ += value;
        sumSq_ += value * value;
        count_++;

        if (window_.size() > lookback_) {
            double old = window_.front();
            window_.pop_front();
            sum_ -= old;
            sumSq_ -= old * old;
            count_--;
        }
    }
};

} // namespace signals
