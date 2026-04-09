#pragma once
/**
 * @file VPIN.hpp
 * @brief Volume-Synchronized Probability of Informed Trading.
 *
 * VPIN (Easley, Lopez de Prado, O'Hara 2012) estimates the probability
 * of informed trading using volume-synchronized sampling. Unlike time-based
 * metrics, VPIN samples at constant volume intervals, which naturally
 * adjusts for varying activity levels.
 *
 * Algorithm:
 *   1. Partition trading volume into fixed-size "volume buckets" of size V
 *   2. For each bucket, classify buy/sell volume using Bulk Volume
 *      Classification (BVC): V_buy = V * Phi(deltaP / sigma_deltaP)
 *   3. Compute VPIN over a rolling window of n buckets:
 *      VPIN = sum(|V_buy_i - V_sell_i|) / (n * V)
 *
 * VPIN ranges from 0 (no informed trading) to 1 (all informed).
 * Values above 0.5 typically indicate elevated toxicity.
 *
 * Design: Fixed-size std::array for bucket storage. Zero heap allocation
 * after construction. All operations O(1) per trade.
 *
 * Reference: Easley, Lopez de Prado, O'Hara (2012) "Flow Toxicity and
 * Liquidity in a High-Frequency World"
 */

#include <array>
#include <cmath>
#include <algorithm>
#include <cstdint>

#include "../core/Order.hpp"

namespace signals {

/// Maximum number of volume buckets in the rolling window
inline constexpr size_t MAX_VPIN_BUCKETS = 50;

/**
 * @brief Volume-Synchronized Probability of Informed Trading.
 *
 * Usage:
 * @code
 *   VPIN vpin(1000, 50);  // 1000-share buckets, 50-bucket window
 *   vpin.onTrade(150.25, 100);  // Price and volume
 *   if (vpin.value() > 0.5) { // widen spread -- high toxicity }
 * @endcode
 */
class VPIN {
public:
    /**
     * @brief Construct VPIN estimator.
     * @param bucketVolume Volume per bucket (V). Typical: 1000-10000 shares.
     * @param numBuckets Number of buckets in rolling window (n). Typical: 50.
     */
    explicit VPIN(Quantity bucketVolume = 1000, size_t numBuckets = 50) noexcept
        : bucketVolume_(bucketVolume),
          numBuckets_(std::min(numBuckets, MAX_VPIN_BUCKETS)),
          currentBuyVolume_(0),
          currentBucketVolume_(0),
          lastPrice_(0.0),
          sumDeltaP_(0.0),
          sumDeltaP2_(0.0),
          priceCount_(0),
          head_(0),
          filledBuckets_(0),
          vpin_(0.0),
          sumAbsImbalance_(0.0) {
        buckets_.fill({0.0, 0.0});
    }

    /**
     * @brief Process a trade.
     * @param price Trade price
     * @param volume Trade volume (unsigned)
     */
    void onTrade(double price, Quantity volume) noexcept {
        // Track price changes for sigma estimation
        if (lastPrice_ > 0.0) {
            double dp = price - lastPrice_;
            sumDeltaP_ += dp;
            sumDeltaP2_ += dp * dp;
            priceCount_++;
        }
        lastPrice_ = price;

        // Accumulate volume into current bucket
        Quantity remaining = volume;

        while (remaining > 0) {
            Quantity spaceInBucket = bucketVolume_ - currentBucketVolume_;
            Quantity toAdd = std::min(remaining, spaceInBucket);

            // BVC: classify this portion as buy/sell
            // V_buy = V * Phi(deltaP / sigma_deltaP)
            double buyFraction = classifyBVC(price);
            currentBuyVolume_ += toAdd * buyFraction;
            currentBucketVolume_ += toAdd;
            remaining -= toAdd;

            // Bucket complete?
            if (currentBucketVolume_ >= bucketVolume_) {
                completeBucket();
            }
        }
    }

    /**
     * @brief Get current VPIN estimate.
     * @return VPIN in [0, 1]. Higher = more informed trading.
     */
    [[nodiscard]] double value() const noexcept { return vpin_; }

    /**
     * @brief Get number of completed buckets.
     */
    [[nodiscard]] size_t completedBuckets() const noexcept { return filledBuckets_; }

    /**
     * @brief Check if enough buckets have filled for a meaningful estimate.
     */
    [[nodiscard]] bool isValid() const noexcept {
        return filledBuckets_ >= numBuckets_;
    }

    /**
     * @brief Reset VPIN state.
     */
    void reset() noexcept {
        currentBuyVolume_ = 0;
        currentBucketVolume_ = 0;
        lastPrice_ = 0.0;
        sumDeltaP_ = sumDeltaP2_ = 0.0;
        priceCount_ = 0;
        head_ = 0;
        filledBuckets_ = 0;
        vpin_ = 0.0;
        sumAbsImbalance_ = 0.0;
        buckets_.fill({0.0, 0.0});
    }

private:
    struct Bucket {
        double buyVolume;
        double sellVolume;
    };

    Quantity bucketVolume_;
    size_t numBuckets_;

    // Current (incomplete) bucket state
    double currentBuyVolume_;
    Quantity currentBucketVolume_;

    // Price change statistics for BVC sigma estimation
    double lastPrice_;
    double sumDeltaP_;
    double sumDeltaP2_;
    size_t priceCount_;

    // Ring buffer of completed buckets
    std::array<Bucket, MAX_VPIN_BUCKETS> buckets_;
    size_t head_;           ///< Next write position
    size_t filledBuckets_;  ///< Total buckets ever completed

    // Running VPIN state
    double vpin_;
    double sumAbsImbalance_;

    /**
     * @brief Bulk Volume Classification.
     * Phi(deltaP / sigma) gives the probability that a trade was buyer-initiated.
     */
    [[nodiscard]] double classifyBVC(double price) const noexcept {
        if (priceCount_ < 2 || lastPrice_ <= 0.0) return 0.5;

        double dp = price - lastPrice_;
        double meanDp = sumDeltaP_ / priceCount_;
        double varDp = (sumDeltaP2_ / priceCount_) - (meanDp * meanDp);
        double sigma = std::sqrt(std::max(varDp, 1e-15));

        // Phi(z) approximation using the error function
        double z = dp / sigma;
        return normalCDF(z);
    }

    /**
     * @brief Complete the current bucket and update VPIN.
     */
    void completeBucket() noexcept {
        double sellVolume = static_cast<double>(bucketVolume_) - currentBuyVolume_;

        // Remove oldest bucket from sum if window is full
        size_t idx = head_ % numBuckets_;
        if (filledBuckets_ >= numBuckets_) {
            double oldImbalance = std::abs(buckets_[idx].buyVolume
                                           - buckets_[idx].sellVolume);
            sumAbsImbalance_ -= oldImbalance;
        }

        // Store new bucket
        buckets_[idx] = {currentBuyVolume_, sellVolume};
        double newImbalance = std::abs(currentBuyVolume_ - sellVolume);
        sumAbsImbalance_ += newImbalance;

        head_ = (head_ + 1) % numBuckets_;
        filledBuckets_++;

        // Update VPIN
        size_t n = std::min(filledBuckets_, numBuckets_);
        vpin_ = sumAbsImbalance_ / (n * static_cast<double>(bucketVolume_));

        // Reset current bucket
        currentBuyVolume_ = 0;
        currentBucketVolume_ = 0;
    }

    /**
     * @brief Approximate standard normal CDF.
     * Abramowitz & Stegun approximation, accurate to 1e-5.
     */
    [[nodiscard]] static double normalCDF(double x) noexcept {
        // Constants for the approximation
        constexpr double a1 = 0.254829592;
        constexpr double a2 = -0.284496736;
        constexpr double a3 = 1.421413741;
        constexpr double a4 = -1.453152027;
        constexpr double a5 = 1.061405429;
        constexpr double p  = 0.3275911;

        double sign = (x >= 0) ? 1.0 : -1.0;
        x = std::abs(x);
        double t = 1.0 / (1.0 + p * x);
        double y = 1.0 - (((((a5 * t + a4) * t) + a3) * t + a2) * t + a1) * t
                   * std::exp(-x * x / 2.0);

        return 0.5 * (1.0 + sign * y);
    }
};

} // namespace signals
