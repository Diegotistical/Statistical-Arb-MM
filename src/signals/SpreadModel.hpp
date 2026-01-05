#pragma once
/**
 * @file SpreadModel.hpp
 * @brief Rolling z-score spread model for stat-arb.
 * 
 * Computes:
 *   spread = log(price_A) - β * log(price_B)
 *   z = (spread - μ) / σ
 * 
 * Where μ and σ are rolling estimates.
 */

#include <cmath>
#include <deque>

namespace signals {

/**
 * @brief Rolling z-score spread model.
 * 
 * Usage:
 * @code
 *   SpreadModel model(100);  // 100-tick lookback
 *   double z = model.update(mid_AAPL, mid_MSFT);
 *   if (z > 2.0) { // Spread is 2σ above mean }
 * @endcode
 */
class SpreadModel {
public:
    /**
     * @brief Construct spread model.
     * @param lookback Rolling window size
     * @param beta Hedge ratio (default 1.0 for pairs)
     */
    explicit SpreadModel(size_t lookback = 100, double beta = 1.0)
        : lookback_(lookback), beta_(beta), 
          sum_(0), sumSq_(0), count_(0), spread_(0), z_(0) {}

    /**
     * @brief Update model with new prices.
     * @param priceA First asset price (e.g., AAPL)
     * @param priceB Second asset price (e.g., MSFT)
     * @return Current z-score
     */
    double update(double priceA, double priceB) {
        // Compute log spread
        spread_ = std::log(priceA) - beta_ * std::log(priceB);
        
        // Add to window
        window_.push_back(spread_);
        sum_ += spread_;
        sumSq_ += spread_ * spread_;
        count_++;
        
        // Remove old if window full
        if (window_.size() > lookback_) {
            double old = window_.front();
            window_.pop_front();
            sum_ -= old;
            sumSq_ -= old * old;
            count_--;
        }
        
        // Compute z-score
        if (count_ >= 2) {
            double mean = sum_ / count_;
            double variance = (sumSq_ / count_) - (mean * mean);
            double std = std::sqrt(std::max(variance, 1e-10));
            z_ = (spread_ - mean) / std;
        } else {
            z_ = 0;
        }
        
        return z_;
    }

    /**
     * @brief Get current z-score.
     */
    [[nodiscard]] double zScore() const noexcept { return z_; }
    
    /**
     * @brief Get current spread value.
     */
    [[nodiscard]] double spread() const noexcept { return spread_; }
    
    /**
     * @brief Get rolling mean.
     */
    [[nodiscard]] double mean() const noexcept { 
        return count_ > 0 ? sum_ / count_ : 0; 
    }
    
    /**
     * @brief Get rolling standard deviation.
     */
    [[nodiscard]] double stdDev() const noexcept {
        if (count_ < 2) return 0;
        double m = sum_ / count_;
        double var = (sumSq_ / count_) - (m * m);
        return std::sqrt(std::max(var, 0.0));
    }

    /**
     * @brief Set hedge ratio.
     */
    void setBeta(double beta) noexcept { beta_ = beta; }
    
    /**
     * @brief Get hedge ratio.
     */
    [[nodiscard]] double beta() const noexcept { return beta_; }

    /**
     * @brief Reset model state.
     */
    void reset() {
        window_.clear();
        sum_ = sumSq_ = spread_ = z_ = 0;
        count_ = 0;
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
};

} // namespace signals
