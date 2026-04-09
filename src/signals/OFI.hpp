#pragma once
/**
 * @file OFI.hpp
 * @brief Order Flow Imbalance with multi-level and trade classification.
 *
 * Implements two OFI variants:
 *
 *   1. Top-of-book OFI (original Cont, Kukanov, Stoikov 2014):
 *      OFI = sum(delta_BidSize - delta_AskSize)
 *      Simple, fast, captures the main signal.
 *
 *   2. Multi-level OFI (extension of CKS 2014):
 *      OFI_K = sum_{k=1}^{K} w_k * (delta_BidSize_k - delta_AskSize_k)
 *      where w_k = 1/k (decaying weight by level depth).
 *      Captures deeper book dynamics for better toxicity detection.
 *
 * Also includes Lee-Ready (1991) trade classification for determining
 * whether a trade was buyer- or seller-initiated:
 *   - Tick test: if trade price > previous trade price, it's a buy
 *   - Quote midpoint: if trade price > midpoint, it's a buy
 *   - Combined: midpoint test preferred, tick test as fallback
 *
 * Reference: Cont, Kukanov, Stoikov (2014) "The Price Impact of Order
 * Book Events"
 */

#include <cmath>
#include <deque>
#include <array>

namespace signals {

/// Maximum number of book levels for multi-level OFI
inline constexpr size_t MAX_OFI_LEVELS = 10;

/**
 * @brief Trade classification result.
 */
enum class TradeDirection : int8_t {
    Buy = 1,
    Sell = -1,
    Unknown = 0
};

/**
 * @brief Top-of-book Order Flow Imbalance indicator.
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
    explicit OFI(size_t window = 100)
        : window_(window), prevBidSize_(0), prevAskSize_(0),
          ofi_(0), sumOfi_(0), sumOfiSq_(0), initialized_(false) {}

    /**
     * @brief Update OFI with new book state.
     * @param bidSize Total size at best bid
     * @param askSize Total size at best ask
     * @return Current cumulative OFI value
     */
    double update(double bidSize, double askSize) {
        if (!initialized_) {
            prevBidSize_ = bidSize;
            prevAskSize_ = askSize;
            initialized_ = true;
            return 0;
        }

        double deltaBid = bidSize - prevBidSize_;
        double deltaAsk = askSize - prevAskSize_;
        double delta = deltaBid - deltaAsk;

        ofi_ += delta;

        history_.push_back(delta);
        sumOfi_ += delta;
        sumOfiSq_ += delta * delta;

        if (history_.size() > window_) {
            double old = history_.front();
            history_.pop_front();
            sumOfi_ -= old;
            sumOfiSq_ -= old * old;
        }

        prevBidSize_ = bidSize;
        prevAskSize_ = askSize;

        return ofi_;
    }

    [[nodiscard]] double value() const noexcept { return ofi_; }

    /**
     * @brief Get normalized OFI (z-score of cumulative OFI over window).
     */
    [[nodiscard]] double normalized() const noexcept {
        if (history_.size() < 2) return 0;

        double n = static_cast<double>(history_.size());
        double mean = sumOfi_ / n;
        double var = (sumOfiSq_ / n) - (mean * mean);
        double std = std::sqrt(std::max(var, 1e-10));

        // Normalize rolling sum (not just last delta)
        double rollingSum = sumOfi_;
        return rollingSum / (std * std::sqrt(n));
    }

    [[nodiscard]] double mean() const noexcept {
        if (history_.empty()) return 0;
        return sumOfi_ / static_cast<double>(history_.size());
    }

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

/**
 * @brief Multi-level Order Flow Imbalance (extended CKS 2014).
 *
 * Tracks OFI across K price levels with decaying weights.
 * Deeper levels capture institutional flow that doesn't appear
 * at the top of book.
 *
 * Usage:
 * @code
 *   MultiLevelOFI ofi(5, 100);  // 5 levels, 100-tick window
 *   std::array<double, 5> bidSizes = {1000, 800, 600, 400, 200};
 *   std::array<double, 5> askSizes = {900, 700, 500, 300, 100};
 *   ofi.update(bidSizes.data(), askSizes.data(), 5);
 * @endcode
 */
class MultiLevelOFI {
public:
    /**
     * @brief Construct multi-level OFI.
     * @param levels Number of book levels to track (up to MAX_OFI_LEVELS)
     * @param window Rolling window for normalization
     */
    explicit MultiLevelOFI(size_t levels = 5, size_t window = 100)
        : levels_(std::min(levels, MAX_OFI_LEVELS)), window_(window),
          ofi_(0), sumOfi_(0), sumOfiSq_(0), initialized_(false) {
        prevBidSizes_.fill(0);
        prevAskSizes_.fill(0);

        // Pre-compute level weights: w_k = 1/k (deeper levels matter less)
        weights_.fill(0);
        for (size_t k = 0; k < levels_; ++k) {
            weights_[k] = 1.0 / (1.0 + static_cast<double>(k));
        }
    }

    /**
     * @brief Update with new multi-level book state.
     * @param bidSizes Array of bid sizes at each level (best to worst)
     * @param askSizes Array of ask sizes at each level (best to worst)
     * @param numLevels Number of valid levels in the arrays
     * @return Current weighted OFI value
     */
    double update(const double* bidSizes, const double* askSizes, size_t numLevels) {
        numLevels = std::min(numLevels, levels_);

        if (!initialized_) {
            for (size_t k = 0; k < numLevels; ++k) {
                prevBidSizes_[k] = bidSizes[k];
                prevAskSizes_[k] = askSizes[k];
            }
            initialized_ = true;
            return 0;
        }

        // Weighted OFI across levels
        double delta = 0;
        for (size_t k = 0; k < numLevels; ++k) {
            double dBid = bidSizes[k] - prevBidSizes_[k];
            double dAsk = askSizes[k] - prevAskSizes_[k];
            delta += weights_[k] * (dBid - dAsk);

            prevBidSizes_[k] = bidSizes[k];
            prevAskSizes_[k] = askSizes[k];
        }

        ofi_ += delta;

        // Rolling window for normalization
        history_.push_back(delta);
        sumOfi_ += delta;
        sumOfiSq_ += delta * delta;

        if (history_.size() > window_) {
            double old = history_.front();
            history_.pop_front();
            sumOfi_ -= old;
            sumOfiSq_ -= old * old;
        }

        return ofi_;
    }

    [[nodiscard]] double value() const noexcept { return ofi_; }

    [[nodiscard]] double normalized() const noexcept {
        if (history_.size() < 2) return 0;
        double n = static_cast<double>(history_.size());
        double mean = sumOfi_ / n;
        double var = (sumOfiSq_ / n) - (mean * mean);
        double std = std::sqrt(std::max(var, 1e-10));
        return sumOfi_ / (std * std::sqrt(n));
    }

    void reset() {
        history_.clear();
        ofi_ = sumOfi_ = sumOfiSq_ = 0;
        prevBidSizes_.fill(0);
        prevAskSizes_.fill(0);
        initialized_ = false;
    }

private:
    size_t levels_;
    size_t window_;
    std::array<double, MAX_OFI_LEVELS> prevBidSizes_;
    std::array<double, MAX_OFI_LEVELS> prevAskSizes_;
    std::array<double, MAX_OFI_LEVELS> weights_;
    double ofi_;
    double sumOfi_;
    double sumOfiSq_;
    bool initialized_;
    std::deque<double> history_;
};

/**
 * @brief Lee-Ready (1991) trade classification algorithm.
 *
 * Determines whether a trade was buyer- or seller-initiated using:
 *   1. Quote midpoint test: trade > mid => buy, trade < mid => sell
 *   2. Tick test (fallback): price up => buy, price down => sell
 *
 * Combined approach (standard industry practice):
 *   - Use midpoint test when trade is away from mid
 *   - Use tick test when trade is exactly at mid
 *
 * Reference: Lee & Ready (1991) "Inferring Trade Direction from
 * Intraday Data"
 */
class LeeReadyClassifier {
public:
    /**
     * @brief Classify a trade as buyer- or seller-initiated.
     *
     * @param tradePrice  Execution price
     * @param bidPrice    Best bid at time of trade
     * @param askPrice    Best ask at time of trade
     * @return TradeDirection (Buy, Sell, or Unknown)
     */
    TradeDirection classify(double tradePrice, double bidPrice, double askPrice) noexcept {
        double midPrice = (bidPrice + askPrice) / 2.0;

        TradeDirection result;

        // Quote midpoint test (primary)
        if (tradePrice > midPrice + 1e-10) {
            result = TradeDirection::Buy;
        } else if (tradePrice < midPrice - 1e-10) {
            result = TradeDirection::Sell;
        } else {
            // At midpoint: use tick test (fallback)
            result = tickTest(tradePrice);
        }

        lastPrice_ = tradePrice;
        return result;
    }

    /**
     * @brief Get signed trade volume.
     * @param volume Unsigned trade volume
     * @param direction Classification result
     * @return Positive for buys, negative for sells
     */
    [[nodiscard]] static double signedVolume(double volume, TradeDirection direction) noexcept {
        return volume * static_cast<double>(static_cast<int8_t>(direction));
    }

    void reset() noexcept { lastPrice_ = 0; }

private:
    double lastPrice_ = 0;

    /**
     * @brief Tick test: classify based on price movement.
     */
    [[nodiscard]] TradeDirection tickTest(double currentPrice) const noexcept {
        if (lastPrice_ <= 0) return TradeDirection::Unknown;

        if (currentPrice > lastPrice_ + 1e-10) {
            return TradeDirection::Buy;   // Uptick
        } else if (currentPrice < lastPrice_ - 1e-10) {
            return TradeDirection::Sell;  // Downtick
        }
        return TradeDirection::Unknown;   // Zero tick
    }
};

} // namespace signals
