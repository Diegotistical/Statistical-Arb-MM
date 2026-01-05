#pragma once
/**
 * @file PnLAnalytics.hpp
 * @brief PnL decomposition and performance attribution.
 * 
 * Decomposes PnL into:
 *   - Spread capture (bid-ask capture from MM)
 *   - Inventory drift (unrealized mark-to-market)
 *   - Adverse selection (cost from toxic flow)
 */

#include <cmath>
#include <vector>
#include <numeric>

namespace analytics {

/**
 * @brief PnL breakdown by source.
 */
struct PnLBreakdown {
    double realized = 0;           ///< Realized PnL (closed positions)
    double unrealized = 0;         ///< Unrealized PnL (open inventory)
    double spread_capture = 0;     ///< Profit from bid-ask spread
    double inventory_cost = 0;     ///< Cost from inventory risk
    double adverse_selection = 0;  ///< Cost from toxic flow
    double total = 0;              ///< Total PnL
    
    void compute() {
        total = realized + unrealized;
    }
};

/**
 * @brief Trade record for attribution.
 */
struct TradeRecord {
    int64_t timestamp;
    double price;
    double quantity;    // Signed: +buy, -sell
    double mid_at_fill; // Mid price at time of fill
    double spread_at_fill;
    bool is_maker;      // true if passive, false if aggressive
};

/**
 * @brief PnL analytics and decomposition engine.
 * 
 * Tracks:
 *   - Mark-to-market PnL
 *   - Spread capture from passive fills
 *   - Adverse selection cost
 *   - Inventory holding cost
 */
class PnLAnalytics {
public:
    /**
     * @brief Record a trade.
     */
    void onTrade(int64_t timestamp, double price, double quantity,
                 double midPrice, double spread, bool isMaker) {
        TradeRecord trade{timestamp, price, quantity, midPrice, spread, isMaker};
        trades_.push_back(trade);
        
        // Update position
        double oldPosition = position_;
        position_ += quantity;
        
        // Calculate realized PnL if closing
        if ((oldPosition > 0 && quantity < 0) || (oldPosition < 0 && quantity > 0)) {
            double closingQty = std::min(std::abs(oldPosition), std::abs(quantity));
            double realizedPnL = closingQty * (price - avgCost_) * (quantity > 0 ? -1 : 1);
            breakdown_.realized += realizedPnL;
        }
        
        // Update average cost
        if (std::abs(position_) > 1e-10) {
            if ((oldPosition >= 0 && quantity > 0) || (oldPosition <= 0 && quantity < 0)) {
                // Adding to position
                avgCost_ = (avgCost_ * std::abs(oldPosition) + price * std::abs(quantity)) /
                           std::abs(position_);
            }
        } else {
            avgCost_ = 0;
        }
        
        // Spread capture (maker only)
        if (isMaker) {
            double spreadCapture = std::abs(quantity) * (spread / 2.0);
            breakdown_.spread_capture += spreadCapture;
        }
        
        // Adverse selection: price moved against us after fill
        // Will be computed in updateMark()
    }

    /**
     * @brief Update mark-to-market prices.
     */
    void updateMark(int64_t timestamp, double midPrice) {
        // Unrealized PnL
        breakdown_.unrealized = position_ * (midPrice - avgCost_);
        
        // Adverse selection: compute from recent trades
        computeAdverseSelection(midPrice);
        
        // Inventory cost: opportunity cost of holding
        // Simplified: |position| * volatility * sqrt(time)
        double vol = estimateVolatility();
        breakdown_.inventory_cost = std::abs(position_) * vol * 0.01;  // Scale factor
        
        lastMid_ = midPrice;
        lastTimestamp_ = timestamp;
        
        breakdown_.compute();
    }

    /**
     * @brief Get current PnL breakdown.
     */
    [[nodiscard]] const PnLBreakdown& breakdown() const { return breakdown_; }
    
    /**
     * @brief Get current position.
     */
    [[nodiscard]] double position() const { return position_; }
    
    /**
     * @brief Get average cost.
     */
    [[nodiscard]] double avgCost() const { return avgCost_; }

    /**
     * @brief Compute Sharpe ratio.
     */
    [[nodiscard]] double sharpe() const {
        if (pnlHistory_.size() < 20) return 0;
        
        std::vector<double> returns;
        for (size_t i = 1; i < pnlHistory_.size(); ++i) {
            returns.push_back(pnlHistory_[i] - pnlHistory_[i-1]);
        }
        
        double mean = std::accumulate(returns.begin(), returns.end(), 0.0) / returns.size();
        double sq_sum = 0;
        for (double r : returns) sq_sum += (r - mean) * (r - mean);
        double std = std::sqrt(sq_sum / returns.size());
        
        if (std < 1e-10) return 0;
        return (mean / std) * std::sqrt(252 * 6.5 * 3600);  // Annualized
    }

    /**
     * @brief Record PnL snapshot for Sharpe calculation.
     */
    void snapshot() {
        pnlHistory_.push_back(breakdown_.total);
    }

    /**
     * @brief Reset analytics.
     */
    void reset() {
        trades_.clear();
        pnlHistory_.clear();
        breakdown_ = PnLBreakdown{};
        position_ = avgCost_ = lastMid_ = 0;
        lastTimestamp_ = 0;
    }

private:
    std::vector<TradeRecord> trades_;
    std::vector<double> pnlHistory_;
    PnLBreakdown breakdown_;
    double position_ = 0;
    double avgCost_ = 0;
    double lastMid_ = 0;
    int64_t lastTimestamp_ = 0;

    void computeAdverseSelection(double currentMid) {
        // Adverse selection: for each fill, how much did price move against us?
        // Lookback: last 100 trades
        size_t start = trades_.size() > 100 ? trades_.size() - 100 : 0;
        double totalAS = 0;
        
        for (size_t i = start; i < trades_.size(); ++i) {
            const auto& trade = trades_[i];
            double priceMove = currentMid - trade.mid_at_fill;
            
            // Adverse if price moved against our trade direction
            if ((trade.quantity > 0 && priceMove < 0) ||  // Bought, price dropped
                (trade.quantity < 0 && priceMove > 0)) {  // Sold, price rose
                totalAS += std::abs(trade.quantity * priceMove);
            }
        }
        
        breakdown_.adverse_selection = totalAS;
    }

    [[nodiscard]] double estimateVolatility() const {
        if (pnlHistory_.size() < 20) return 0.01;
        
        double sum = 0, sumSq = 0;
        size_t n = std::min(pnlHistory_.size(), size_t(100));
        size_t start = pnlHistory_.size() - n;
        
        for (size_t i = start + 1; i < pnlHistory_.size(); ++i) {
            double ret = pnlHistory_[i] - pnlHistory_[i-1];
            sum += ret;
            sumSq += ret * ret;
        }
        
        double mean = sum / (n - 1);
        double var = sumSq / (n - 1) - mean * mean;
        return std::sqrt(std::max(var, 0.0));
    }
};

} // namespace analytics
