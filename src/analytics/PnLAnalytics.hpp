#pragma once
/**
 * @file PnLAnalytics.hpp
 * @brief PnL decomposition, performance attribution, and risk metrics.
 *
 * Decomposes PnL into:
 *   - Spread capture: profit from passive fills (bid-ask earned)
 *   - Inventory cost: holding risk (exposure * volatility * time)
 *   - Adverse selection: cost from trading against informed flow
 *   - Transaction costs: fees, commissions, market impact
 *
 * Performance metrics:
 *   - Annualized Sharpe ratio (with configurable risk-free rate)
 *   - Sortino ratio (downside deviation only)
 *   - Calmar ratio (return / max drawdown)
 *   - Maximum drawdown and recovery time
 *   - Fill rate and turnover
 *   - Average holding period
 */

#include <cmath>
#include <vector>
#include <numeric>
#include <algorithm>

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
    double transaction_costs = 0;  ///< Fees, commissions, impact
    double total = 0;              ///< Net PnL

    void compute() {
        total = realized + unrealized;
    }
};

/**
 * @brief Extended performance metrics.
 */
struct PerformanceMetrics {
    double sharpeRatio = 0;        ///< Annualized Sharpe
    double sortinoRatio = 0;       ///< Annualized Sortino
    double calmarRatio = 0;        ///< Return / MaxDrawdown
    double maxDrawdown = 0;        ///< Maximum peak-to-trough
    double maxDrawdownDuration = 0;///< Ticks to recover from max DD
    double totalReturn = 0;        ///< Total PnL
    double annualizedReturn = 0;   ///< Annualized return
    double fillRate = 0;           ///< Fills / Quotes submitted
    double turnover = 0;           ///< Total volume / Avg position
    double avgHoldingPeriod = 0;   ///< Average ticks between open/close
    int totalTrades = 0;
    int totalQuotes = 0;
};

/**
 * @brief Trade record for attribution.
 */
struct TradeRecord {
    int64_t timestamp;
    double price;
    double quantity;    ///< Signed: +buy, -sell
    double mid_at_fill; ///< Mid price at time of fill
    double spread_at_fill;
    bool is_maker;      ///< true if passive, false if aggressive
};

/**
 * @brief PnL analytics and decomposition engine.
 */
class PnLAnalytics {
public:
    /**
     * @brief Set annualization factor.
     * @param ticksPerYear Number of ticks in a trading year.
     *   For 1-second ticks: 252 * 6.5 * 3600 = 5,896,800
     *   For tick-by-tick: depends on data frequency
     */
    void setAnnualizationFactor(double ticksPerYear) noexcept {
        annualizationFactor_ = std::sqrt(ticksPerYear);
    }

    /**
     * @brief Set risk-free rate for Sharpe calculation.
     * @param rate Annual risk-free rate (e.g., 0.05 for 5%)
     */
    void setRiskFreeRate(double rate) noexcept { riskFreeRate_ = rate; }

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
            double realizedPnL = closingQty * (price - avgCost_);
            if (oldPosition < 0) realizedPnL = -realizedPnL;
            breakdown_.realized += realizedPnL;

            // Track holding period
            totalHoldingTicks_ += (timestamp - lastPositionChangeTs_);
            positionChanges_++;
        }

        // Update average cost
        if (std::abs(position_) > 1e-10) {
            bool isAdding = (oldPosition >= 0 && quantity > 0)
                            || (oldPosition <= 0 && quantity < 0);
            if (isAdding) {
                avgCost_ = (avgCost_ * std::abs(oldPosition) + price * std::abs(quantity))
                           / std::abs(position_);
            }
            // If flipping sign, reset avg cost
            if ((oldPosition > 0 && position_ < 0) || (oldPosition < 0 && position_ > 0)) {
                avgCost_ = price;
            }
        } else {
            avgCost_ = 0;
        }

        lastPositionChangeTs_ = timestamp;

        // Spread capture (maker only)
        if (isMaker) {
            breakdown_.spread_capture += std::abs(quantity) * (spread / 2.0);
        }

        // Track total volume for turnover
        totalVolume_ += std::abs(quantity);
        totalAbsPosition_ += std::abs(position_);
        tickCount_++;
    }

    /**
     * @brief Record a quote submission (for fill rate calculation).
     */
    void onQuote() { quotesSubmitted_++; }

    /**
     * @brief Record transaction costs for a trade.
     */
    void addTransactionCost(double cost) {
        breakdown_.transaction_costs += cost;
    }

    /**
     * @brief Update mark-to-market prices.
     */
    void updateMark(int64_t timestamp, double midPrice) {
        breakdown_.unrealized = position_ * (midPrice - avgCost_);

        // Adverse selection from recent trades
        computeAdverseSelection(midPrice);

        // Inventory cost: simplified as |position| * vol * sqrt(dt)
        double vol = estimateVolatility();
        if (tickCount_ > 0) {
            breakdown_.inventory_cost = std::abs(position_) * vol
                                        * std::sqrt(static_cast<double>(tickCount_));
        }

        lastMid_ = midPrice;
        lastTimestamp_ = timestamp;

        breakdown_.compute();
    }

    /**
     * @brief Record PnL snapshot for return calculations.
     * Should be called at regular intervals (e.g., every second or every N ticks).
     */
    void snapshot() {
        double currentPnL = breakdown_.total - breakdown_.transaction_costs;
        pnlHistory_.push_back(currentPnL);

        // Track drawdown
        peakPnL_ = std::max(peakPnL_, currentPnL);
        double dd = peakPnL_ - currentPnL;
        if (dd > maxDrawdown_) {
            maxDrawdown_ = dd;
            drawdownStartIdx_ = peakIdx_;
            drawdownEndIdx_ = pnlHistory_.size() - 1;
        }
        if (currentPnL >= peakPnL_) {
            peakIdx_ = pnlHistory_.size() - 1;
        }
    }

    // ========================================================================
    // Performance Metrics
    // ========================================================================

    /**
     * @brief Compute all performance metrics.
     */
    [[nodiscard]] PerformanceMetrics computeMetrics() const {
        PerformanceMetrics m;

        m.totalReturn = breakdown_.total - breakdown_.transaction_costs;
        m.maxDrawdown = maxDrawdown_;
        m.totalTrades = static_cast<int>(trades_.size());
        m.totalQuotes = quotesSubmitted_;

        if (pnlHistory_.size() < 20) return m;

        // Compute returns
        std::vector<double> returns;
        returns.reserve(pnlHistory_.size() - 1);
        for (size_t i = 1; i < pnlHistory_.size(); ++i) {
            returns.push_back(pnlHistory_[i] - pnlHistory_[i - 1]);
        }

        double n = static_cast<double>(returns.size());

        // Mean and std of returns
        double meanRet = std::accumulate(returns.begin(), returns.end(), 0.0) / n;
        double sqSum = 0;
        for (double r : returns) sqSum += (r - meanRet) * (r - meanRet);
        double stdRet = std::sqrt(sqSum / n);

        // Sharpe ratio (annualized)
        if (stdRet > 1e-10) {
            double rfPerTick = riskFreeRate_ / (annualizationFactor_ * annualizationFactor_);
            m.sharpeRatio = ((meanRet - rfPerTick) / stdRet) * annualizationFactor_;
        }

        // Sortino ratio (downside deviation only)
        double downSum = 0;
        int downCount = 0;
        for (double r : returns) {
            if (r < 0) {
                downSum += r * r;
                downCount++;
            }
        }
        if (downCount > 0) {
            double downDev = std::sqrt(downSum / n);  // Use full N for comparability
            if (downDev > 1e-10) {
                m.sortinoRatio = (meanRet / downDev) * annualizationFactor_;
            }
        }

        // Calmar ratio
        if (maxDrawdown_ > 1e-10) {
            m.annualizedReturn = meanRet * annualizationFactor_ * annualizationFactor_;
            m.calmarRatio = m.annualizedReturn / maxDrawdown_;
        }

        // Drawdown duration
        if (drawdownEndIdx_ > drawdownStartIdx_) {
            m.maxDrawdownDuration = static_cast<double>(drawdownEndIdx_ - drawdownStartIdx_);
        }

        // Fill rate
        if (quotesSubmitted_ > 0) {
            m.fillRate = static_cast<double>(trades_.size())
                         / static_cast<double>(quotesSubmitted_);
        }

        // Turnover
        if (tickCount_ > 0) {
            double avgPos = totalAbsPosition_ / static_cast<double>(tickCount_);
            if (avgPos > 1e-10) {
                m.turnover = totalVolume_ / avgPos;
            }
        }

        // Average holding period
        if (positionChanges_ > 0) {
            m.avgHoldingPeriod = static_cast<double>(totalHoldingTicks_)
                                 / static_cast<double>(positionChanges_);
        }

        return m;
    }

    // ========================================================================
    // Accessors
    // ========================================================================

    [[nodiscard]] const PnLBreakdown& breakdown() const { return breakdown_; }
    [[nodiscard]] double position() const { return position_; }
    [[nodiscard]] double avgCost() const { return avgCost_; }
    [[nodiscard]] double maxDrawdown() const { return maxDrawdown_; }

    /**
     * @brief Compute Sharpe ratio (convenience method).
     */
    [[nodiscard]] double sharpe() const {
        return computeMetrics().sharpeRatio;
    }

    void reset() {
        trades_.clear();
        pnlHistory_.clear();
        breakdown_ = PnLBreakdown{};
        position_ = avgCost_ = lastMid_ = 0;
        lastTimestamp_ = 0;
        peakPnL_ = maxDrawdown_ = 0;
        peakIdx_ = drawdownStartIdx_ = drawdownEndIdx_ = 0;
        totalVolume_ = totalAbsPosition_ = 0;
        tickCount_ = 0;
        quotesSubmitted_ = 0;
        positionChanges_ = 0;
        totalHoldingTicks_ = 0;
        lastPositionChangeTs_ = 0;
    }

private:
    std::vector<TradeRecord> trades_;
    std::vector<double> pnlHistory_;
    PnLBreakdown breakdown_;
    double position_ = 0;
    double avgCost_ = 0;
    double lastMid_ = 0;
    int64_t lastTimestamp_ = 0;

    // Drawdown tracking
    double peakPnL_ = 0;
    double maxDrawdown_ = 0;
    size_t peakIdx_ = 0;
    size_t drawdownStartIdx_ = 0;
    size_t drawdownEndIdx_ = 0;

    // Turnover and fill rate tracking
    double totalVolume_ = 0;
    double totalAbsPosition_ = 0;
    int64_t tickCount_ = 0;
    int quotesSubmitted_ = 0;

    // Holding period tracking
    int positionChanges_ = 0;
    int64_t totalHoldingTicks_ = 0;
    int64_t lastPositionChangeTs_ = 0;

    // Configuration
    double annualizationFactor_ = std::sqrt(252.0 * 6.5 * 3600.0);
    double riskFreeRate_ = 0.0;

    void computeAdverseSelection(double currentMid) {
        size_t start = trades_.size() > 100 ? trades_.size() - 100 : 0;
        double totalAS = 0;

        for (size_t i = start; i < trades_.size(); ++i) {
            const auto& trade = trades_[i];
            double priceMove = currentMid - trade.mid_at_fill;

            if ((trade.quantity > 0 && priceMove < 0) ||
                (trade.quantity < 0 && priceMove > 0)) {
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
            double ret = pnlHistory_[i] - pnlHistory_[i - 1];
            sum += ret;
            sumSq += ret * ret;
        }

        double mean = sum / static_cast<double>(n - 1);
        double var = sumSq / static_cast<double>(n - 1) - mean * mean;
        return std::sqrt(std::max(var, 0.0));
    }
};

} // namespace analytics
