#pragma once
/**
 * @file Simulator.hpp
 * @brief Event-driven backtest simulator with realistic execution modeling.
 *
 * Architecture: Event-driven (not vectorized) for two critical reasons:
 *   1. Lookahead bias: vectorized approaches can accidentally use future
 *      data in signal computation. Event-driven processes ticks sequentially,
 *      making it impossible to peek ahead.
 *   2. Fill model realism: realistic fill simulation requires maintaining
 *      queue position state, which depends on the full order lifecycle.
 *      Vectorized fill models are typically binary (fill or not), losing
 *      the partial fill and adverse selection dynamics.
 *
 * Main loop per tick:
 *   1. Update market data and signals (spread, OFI)
 *   2. Update strategy time (for A-S terminal degradation)
 *   3. Generate quotes via StatArbMM
 *   4. Submit orders through ExecutionSimulator (latency, queue)
 *   5. Process fills with proper side tracking
 *   6. Update PnL analytics and risk metrics
 */

#include <vector>
#include <cmath>
#include <algorithm>
#include <numeric>
#include <unordered_map>
#include <iostream>

#include "../core/OrderBook.hpp"
#include "../signals/SpreadModel.hpp"
#include "../signals/OFI.hpp"
#include "../signals/KalmanFilter.hpp"
#include "../signals/VPIN.hpp"
#include "../signals/KyleLambda.hpp"
#include "../strategy/StatArbMM.hpp"
#include "../execution/ExecutionSimulator.hpp"
#include "../execution/TransactionCosts.hpp"
#include "../risk/RiskManager.hpp"
#include "../analytics/PnLAnalytics.hpp"

namespace backtest {

/**
 * @brief Tick event from market data.
 */
struct TickEvent {
    int64_t timestamp;
    int32_t symbolId;
    Price bidPrice;
    Price askPrice;
    Quantity bidSize;
    Quantity askSize;
};

/**
 * @brief Simulation metrics (summary statistics).
 */
struct SimulationMetrics {
    double totalPnL = 0;
    double realizedPnL = 0;
    double unrealizedPnL = 0;
    double maxDrawdown = 0;
    double peakPnL = 0;
    int32_t numTrades = 0;
    int32_t numBuys = 0;
    int32_t numSells = 0;
    Quantity totalVolume = 0;
    double sharpeRatio = 0;
    double sortinoRatio = 0;
    double calmarRatio = 0;
    double fillRate = 0;
    double turnover = 0;
    int32_t finalInventory = 0;
    int32_t maxInventory = 0;

    void print() const {
        std::cout << "=== Simulation Metrics ===\n"
                  << "PnL:          " << totalPnL << "\n"
                  << "Realized:     " << realizedPnL << "\n"
                  << "Unrealized:   " << unrealizedPnL << "\n"
                  << "Max Drawdown: " << maxDrawdown << "\n"
                  << "Sharpe:       " << sharpeRatio << "\n"
                  << "Sortino:      " << sortinoRatio << "\n"
                  << "Calmar:       " << calmarRatio << "\n"
                  << "Trades:       " << numTrades << "\n"
                  << "Fill Rate:    " << fillRate << "\n"
                  << "Turnover:     " << turnover << "\n"
                  << "Volume:       " << totalVolume << "\n"
                  << "Final Inv:    " << finalInventory << "\n";
    }
};

/**
 * @brief Event-driven backtest simulator.
 *
 * Usage:
 * @code
 *   Simulator sim;
 *   sim.strategy().setSessionTimes(startNs, endNs);
 *   for (auto& tick : marketData) {
 *       sim.onTick(tick);
 *   }
 *   sim.metrics().print();
 * @endcode
 */
class Simulator {
public:
    explicit Simulator(double tickSize = 0.01)
        : tickSize_(tickSize), currentTime_(0), snapshotInterval_(100) {}

    /**
     * @brief Process a tick event.
     */
    void onTick(const TickEvent& tick) {
        currentTime_ = tick.timestamp;
        tickCount_++;

        // Update market data
        double mid = (tick.bidPrice + tick.askPrice) / 2.0;
        double spread = (tick.askPrice - tick.bidPrice) * tickSize_;

        // Update signals
        if (lastMid_.count(tick.symbolId) > 0) {
            if (tick.symbolId == 0 && lastMid_.count(1) > 0) {
                spreadModel_.update(mid, lastMid_[1]);
            }
            ofi_.update(tick.bidSize, tick.askSize);

            // Update VPIN with trade-like events
            double priceChange = mid - lastMid_[tick.symbolId];
            if (std::abs(priceChange) > 1e-10) {
                vpin_.onTrade(mid, tick.bidSize + tick.askSize);
            }

            // Update Kyle's lambda
            if (std::abs(priceChange) > 1e-10) {
                kyle_.update(priceChange, ofi_.value());
            }
        }

        lastMid_[tick.symbolId] = mid;
        lastTick_[tick.symbolId] = tick;

        // Update strategy time for terminal degradation
        strategy_.updateTime(currentTime_);

        // Update risk manager mark
        riskManager_.updateMark(tick.symbolId, mid);

        // Estimate volatility from price returns
        double vol = estimateVolatility(tick.symbolId, mid);

        // Generate quotes
        double vpinValue = vpin_.isValid() ? vpin_.value() : 0.0;
        auto kyleResult = kyle_.estimate();
        double kyleLambdaNorm = kyleResult.isSignificant() ? kyleResult.lambda : 0.0;

        strategy::Quote quote = strategy_.computeQuotes(
            mid, vol, spreadModel_.zScore(), ofi_.normalized(),
            vpinValue, kyleLambdaNorm);

        pnlAnalytics_.onQuote();

        if (quote.valid && quote.riskCheck == risk::RiskCheckResult::Passed) {
            submitQuotes(quote, tick.symbolId, mid, spread);
        }

        // Process fills from execution simulator
        auto fills = executor_.process(book_, core::Timestamp(currentTime_));
        for (const auto& fill : fills) {
            onFill(fill, mid, spread);
        }

        // Update unrealized PnL
        pnlAnalytics_.updateMark(currentTime_, mid);

        // Periodic PnL snapshot for Sharpe calculation
        if (tickCount_ % snapshotInterval_ == 0) {
            pnlAnalytics_.snapshot();
        }
    }

    /**
     * @brief Get simulation metrics.
     */
    [[nodiscard]] SimulationMetrics metrics() const {
        SimulationMetrics m;

        auto perfMetrics = pnlAnalytics_.computeMetrics();
        auto riskSnap = riskManager_.snapshot();

        m.totalPnL = pnlAnalytics_.breakdown().total;
        m.realizedPnL = pnlAnalytics_.breakdown().realized;
        m.unrealizedPnL = pnlAnalytics_.breakdown().unrealized;
        m.maxDrawdown = pnlAnalytics_.maxDrawdown();
        m.sharpeRatio = perfMetrics.sharpeRatio;
        m.sortinoRatio = perfMetrics.sortinoRatio;
        m.calmarRatio = perfMetrics.calmarRatio;
        m.fillRate = perfMetrics.fillRate;
        m.turnover = perfMetrics.turnover;
        m.numTrades = perfMetrics.totalTrades;
        m.finalInventory = strategy_.inventory();
        m.maxInventory = maxInventory_;
        m.totalVolume = totalVolume_;
        m.numBuys = numBuys_;
        m.numSells = numSells_;

        return m;
    }

    /**
     * @brief Reset simulator state.
     */
    void reset() {
        book_.reset();
        spreadModel_.reset();
        ofi_.reset();
        vpin_.reset();
        kyle_.reset();
        strategy_.reset();
        executor_.clear();
        riskManager_.reset();
        pnlAnalytics_.reset();
        lastMid_.clear();
        lastTick_.clear();
        priceHistory_.clear();
        pendingOrders_.clear();
        tickCount_ = 0;
        totalVolume_ = 0;
        numBuys_ = numSells_ = 0;
        maxInventory_ = 0;
    }

    // Accessors
    [[nodiscard]] const OrderBook& book() const { return book_; }
    [[nodiscard]] signals::SpreadModel& spreadModel() { return spreadModel_; }
    [[nodiscard]] signals::OFI& ofi() { return ofi_; }
    [[nodiscard]] signals::VPIN& vpin() { return vpin_; }
    [[nodiscard]] signals::KyleLambda& kyle() { return kyle_; }
    [[nodiscard]] strategy::StatArbMM& strategy() { return strategy_; }
    [[nodiscard]] risk::RiskManager& riskManager() { return riskManager_; }
    [[nodiscard]] analytics::PnLAnalytics& pnlAnalytics() { return pnlAnalytics_; }
    [[nodiscard]] execution::TransactionCostModel& txCostModel() { return txCostModel_; }

    void setSnapshotInterval(int interval) { snapshotInterval_ = interval; }

private:
    double tickSize_;
    int64_t currentTime_;
    int snapshotInterval_;
    int64_t tickCount_ = 0;

    OrderBook book_;
    signals::SpreadModel spreadModel_{100, 1.0};
    signals::OFI ofi_{100};
    signals::VPIN vpin_{1000, 50};
    signals::KyleLambda kyle_{200};
    strategy::StatArbMM strategy_;
    execution::ExecutionSimulator executor_{5000, 1000};
    execution::TransactionCostModel txCostModel_;
    risk::RiskManager riskManager_;
    analytics::PnLAnalytics pnlAnalytics_;

    std::unordered_map<int32_t, double> lastMid_;
    std::unordered_map<int32_t, TickEvent> lastTick_;

    // Track pending orders for proper side identification on fill
    struct PendingOrderInfo {
        OrderSide side;
        Price price;
        Quantity originalQty;
    };
    std::unordered_map<OrderId, PendingOrderInfo> pendingOrders_;
    OrderId nextOrderId_ = 0;

    // Price history for volatility estimation
    std::unordered_map<int32_t, std::deque<double>> priceHistory_;
    static constexpr size_t VOL_WINDOW = 100;

    // Running statistics
    Quantity totalVolume_ = 0;
    int32_t numBuys_ = 0;
    int32_t numSells_ = 0;
    int32_t maxInventory_ = 0;

    void submitQuotes(const strategy::Quote& quote, int32_t symbolId,
                      double /*mid*/, double /*spread*/) {
        if (quote.bidPrice != PRICE_INVALID && quote.bidSize > 0) {
            OrderId id = nextOrderId_++;
            Order bid(id, 0, symbolId,
                      OrderSide::Buy, OrderType::Limit,
                      quote.bidPrice, quote.bidSize);

            // Store order info for fill processing
            pendingOrders_[id] = {OrderSide::Buy, quote.bidPrice, quote.bidSize};

            // Compute queue position from book
            Quantity queueAhead = 0;
            if (book_.hasBids() && quote.bidPrice <= book_.getBestBid()) {
                auto [vol, count] = book_.getQueuePosition(id);
                queueAhead = vol;
            }

            executor_.submit(bid, core::Timestamp(currentTime_), queueAhead);
            riskManager_.recordOrder(symbolId, currentTime_);
        }

        if (quote.askPrice != PRICE_INVALID && quote.askSize > 0) {
            OrderId id = nextOrderId_++;
            Order ask(id, 0, symbolId,
                      OrderSide::Sell, OrderType::Limit,
                      quote.askPrice, quote.askSize);

            pendingOrders_[id] = {OrderSide::Sell, quote.askPrice, quote.askSize};

            Quantity queueAhead = 0;
            if (book_.hasAsks() && quote.askPrice >= book_.getBestAsk()) {
                auto [vol, count] = book_.getQueuePosition(id);
                queueAhead = vol;
            }

            executor_.submit(ask, core::Timestamp(currentTime_), queueAhead);
            riskManager_.recordOrder(symbolId, currentTime_);
        }
    }

    void onFill(const execution::FillReport& fill, double currentMid, double spread) {
        if (fill.fillQty == 0) return;

        // Look up the original order to get the correct side
        auto it = pendingOrders_.find(fill.orderId);
        if (it == pendingOrders_.end()) return;

        OrderSide side = it->second.side;
        double fillPrice = fill.fillPrice * tickSize_;

        // Update strategy inventory
        strategy_.onFill(side, fill.fillQty);

        // Update risk manager
        riskManager_.onFill(0, side, fill.fillQty, fillPrice);

        // Update PnL analytics with proper signed quantity
        double signedQty = (side == OrderSide::Buy)
                           ? static_cast<double>(fill.fillQty)
                           : -static_cast<double>(fill.fillQty);
        pnlAnalytics_.onTrade(currentTime_, fillPrice, signedQty,
                               currentMid * tickSize_, spread, fill.isMaker);

        // Compute transaction costs
        auto cost = txCostModel_.estimateCost(
            fill.fillQty, fillPrice, spread,
            fill.isMaker, side == OrderSide::Buy);
        pnlAnalytics_.addTransactionCost(cost.totalCost);

        // Update statistics
        totalVolume_ += fill.fillQty;
        if (side == OrderSide::Buy) numBuys_++;
        else numSells_++;

        maxInventory_ = std::max(maxInventory_,
                                  static_cast<int32_t>(std::abs(strategy_.inventory())));

        // Clean up completed orders
        if (fill.complete) {
            pendingOrders_.erase(it);
        }
    }

    [[nodiscard]] double estimateVolatility(int32_t symbolId, double mid) {
        auto& history = priceHistory_[symbolId];
        history.push_back(mid);
        if (history.size() > VOL_WINDOW) {
            history.pop_front();
        }

        if (history.size() < 20) return 1.0;

        // Realized volatility from log returns
        double sumR = 0, sumR2 = 0;
        size_t n = history.size() - 1;
        for (size_t i = 1; i <= n; ++i) {
            double ret = std::log(history[i] / history[i - 1]);
            sumR += ret;
            sumR2 += ret * ret;
        }

        double mean = sumR / static_cast<double>(n);
        double var = sumR2 / static_cast<double>(n) - mean * mean;
        return std::sqrt(std::max(var, 1e-10));
    }
};

} // namespace backtest
