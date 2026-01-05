#pragma once
/**
 * @file OFI.hpp
 * @brief Order Flow Imbalance indicator.
 * 
 * OFI = Σ ΔBidSize - Σ ΔAskSize
 * 
 * Interpretation:
 *   OFI > 0: Buy pressure (bids increasing)
 *   OFI < 0: Sell pressure (asks increasing)
 * 
 * Based on: Cont, Kukanov, Stoikov (2014)
 */

#include <cmath>
#include <deque>

namespace signals {

/**
 * @brief Order Flow Imbalance indicator.
 * 
 * Usage:
 * @code
 *   OFI ofi(100);  // 100-tick window
 *   ofi.update(bid_size, ask_size);
 *   if (ofi.normalized() > 0.5) { // Strong buy pressure }
 * @endcode
 */
class OFI {
public:
    /**
     * @brief Construct OFI with window size.
     * @param window Rolling window for normalization
     */
    explicit OFI(size_t window = 100)
        : window_(window), prevBidSize_(0), prevAskSize_(0),
          ofi_(0), sumOfi_(0), sumOfiSq_(0), initialized_(false) {}

    /**
     * @brief Update OFI with new book state.
     * @param bidSize Total size at best bid
     * @param askSize Total size at best ask
     * @return Current OFI value
     */
    double update(double bidSize, double askSize) {
        if (!initialized_) {
            prevBidSize_ = bidSize;
            prevAskSize_ = askSize;
            initialized_ = true;
            return 0;
        }

        // Compute changes
        double deltaBid = bidSize - prevBidSize_;
        double deltaAsk = askSize - prevAskSize_;
        
        // OFI = bid increase - ask increase
        double delta = deltaBid - deltaAsk;
        
        // Update cumulative OFI
        ofi_ += delta;
        
        // Track for normalization
        history_.push_back(delta);
        sumOfi_ += delta;
        sumOfiSq_ += delta * delta;
        
        if (history_.size() > window_) {
            double old = history_.front();
            history_.pop_front();
            sumOfi_ -= old;
            sumOfiSq_ -= old * old;
        }
        
        // Store for next update
        prevBidSize_ = bidSize;
        prevAskSize_ = askSize;
        
        return ofi_;
    }

    /**
     * @brief Get cumulative OFI value.
     */
    [[nodiscard]] double value() const noexcept { return ofi_; }

    /**
     * @brief Get normalized OFI (-1 to 1).
     * 
     * Normalized by rolling standard deviation.
     */
    [[nodiscard]] double normalized() const noexcept {
        if (history_.size() < 2) return 0;
        
        double n = static_cast<double>(history_.size());
        double mean = sumOfi_ / n;
        double var = (sumOfiSq_ / n) - (mean * mean);
        double std = std::sqrt(std::max(var, 1e-10));
        
        // Last delta normalized
        if (history_.empty()) return 0;
        return history_.back() / std;
    }

    /**
     * @brief Get rolling mean of OFI changes.
     */
    [[nodiscard]] double mean() const noexcept {
        if (history_.empty()) return 0;
        return sumOfi_ / history_.size();
    }

    /**
     * @brief Reset OFI state.
     */
    void reset() {
        history_.clear();
        ofi_ = sumOfi_ = sumOfiSq_ = 0;
        prevBidSize_ = prevAskSize_ = 0;
        initialized_ = false;
    }

private:
    size_t window_;
    double prevBidSize_;
    double prevAskSize_;
    double ofi_;
    double sumOfi_;
    double sumOfiSq_;
    bool initialized_;
    std::deque<double> history_;
};

} // namespace signals
