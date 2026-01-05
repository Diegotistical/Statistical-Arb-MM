#pragma once
/**
 * @file Simulator.hpp
 * @brief Backtest simulator with PnL and Sharpe calculation.
 * 
 * Main loop:
 *   1. Update market data
 *   2. Compute signals (spread, OFI)
 *   3. Generate quotes (StatArbMM)
 *   4. Execute orders
 *   5. Track metrics
 */

#include <vector>
#include <cmath>
#include <algorithm>
#include <numeric>

#include "../core/OrderBook.hpp"
#include "../signals/SpreadModel.hpp"
#include "../signals/OFI.hpp"
#include "../strategy/StatArbMM.hpp"
#include "../execution/ExecutionSimulator.hpp"

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
 * @brief Simulation metrics.
 */
struct SimulationMetrics {
    // PnL
    double totalPnL = 0;
    double realizedPnL = 0;
    double unrealizedPnL = 0;
    
    // Risk
    double maxDrawdown = 0;
    double peakPnL = 0;
    
    // Trades
    int32_t numTrades = 0;
    int32_t numBuys = 0;
    int32_t numSells = 0;
    Quantity totalVolume = 0;
    
    // Sharpe (annualized)
    double sharpeRatio = 0;
    
    // Inventory
    int32_t finalInventory = 0;
    int32_t maxInventory = 0;
    
    void print() const {
        std::cout << "=== Simulation Metrics ===\n"
                  << "PnL:          " << totalPnL << "\n"
                  << "Realized:     " << realizedPnL << "\n"
                  << "Unrealized:   " << unrealizedPnL << "\n"
                  << "Max Drawdown: " << maxDrawdown << "\n"
                  << "Sharpe:       " << sharpeRatio << "\n"
                  << "Trades:       " << numTrades << "\n"
                  << "Volume:       " << totalVolume << "\n"
                  << "Final Inv:    " << finalInventory << "\n";
    }
};

/**
 * @brief Backtest simulator.
 * 
 * Usage:
 * @code
 *   Simulator sim;
 *   for (auto& tick : marketData) {
 *       sim.onTick(tick);
 *   }
 *   sim.metrics().print();
 * @endcode
 */
class Simulator {
public:
    explicit Simulator(double tickSize = 0.01)
        : tickSize_(tickSize), currentTime_(0) {}

    /**
     * @brief Process a tick event.
     */
    void onTick(const TickEvent& tick) {
        currentTime_ = tick.timestamp;
        
        // Update signals
        double mid = (tick.bidPrice + tick.askPrice) / 2.0;
        
        if (lastMid_.count(tick.symbolId) > 0) {
            // Compute spread if we have 2 symbols
            if (tick.symbolId == 0 && lastMid_.count(1) > 0) {
                spreadModel_.update(mid, lastMid_[1]);
            }
            
            // Update OFI
            ofi_.update(tick.bidSize, tick.askSize);
        }
        
        lastMid_[tick.symbolId] = mid;
        lastTick_[tick.symbolId] = tick;
        
        // Generate quotes
        double vol = estimateVolatility();
        strategy::Quote quote = strategy_.computeQuotes(
            mid, vol, spreadModel_.zScore(), ofi_.normalized());
        
        if (quote.valid) {
            // Submit orders
            submitQuotes(quote, tick.symbolId);
        }
        
        // Process fills
        auto fills = executor_.process(book_, currentTime_);
        for (const auto& fill : fills) {
            onFill(fill);
        }
        
        // Update unrealized PnL
        updateUnrealizedPnL(mid);
    }

    /**
     * @brief Get simulation metrics.
     */
    [[nodiscard]] SimulationMetrics metrics() const {
        SimulationMetrics m = metrics_;
        m.finalInventory = strategy_.inventory();
        m.sharpeRatio = computeSharpe();
        return m;
    }

    /**
     * @brief Reset simulator state.
     */
    void reset() {
        book_.reset();
        spreadModel_.reset();
        ofi_.reset();
        strategy_.reset();
        executor_.clear();
        metrics_ = SimulationMetrics{};
        pnlHistory_.clear();
        lastMid_.clear();
        lastTick_.clear();
        avgCost_ = 0;
    }

    // Accessors
    [[nodiscard]] const OrderBook& book() const { return book_; }
    [[nodiscard]] signals::SpreadModel& spreadModel() { return spreadModel_; }
    [[nodiscard]] signals::OFI& ofi() { return ofi_; }
    [[nodiscard]] strategy::StatArbMM& strategy() { return strategy_; }

private:
    double tickSize_;
    int64_t currentTime_;
    
    OrderBook book_;
    signals::SpreadModel spreadModel_{100, 1.0};
    signals::OFI ofi_{100};
    strategy::StatArbMM strategy_;
    execution::ExecutionSimulator executor_{5000, 1000};
    
    SimulationMetrics metrics_;
    std::vector<double> pnlHistory_;
    std::unordered_map<int32_t, double> lastMid_;
    std::unordered_map<int32_t, TickEvent> lastTick_;
    
    double avgCost_ = 0;
    OrderId nextOrderId_ = 0;

    void submitQuotes(const strategy::Quote& quote, int32_t symbolId) {
        if (quote.bidPrice != PRICE_INVALID && quote.bidSize > 0) {
            Order bid(nextOrderId_++, 0, symbolId, 
                      OrderSide::Buy, OrderType::Limit, 
                      quote.bidPrice, quote.bidSize);
            executor_.submit(bid, currentTime_);
        }
        
        if (quote.askPrice != PRICE_INVALID && quote.askSize > 0) {
            Order ask(nextOrderId_++, 0, symbolId,
                      OrderSide::Sell, OrderType::Limit,
                      quote.askPrice, quote.askSize);
            executor_.submit(ask, currentTime_);
        }
    }

    void onFill(const execution::FillReport& fill) {
        if (fill.fillQty == 0) return;
        
        metrics_.numTrades++;
        metrics_.totalVolume += fill.fillQty;
        
        // Determine side from fill (simplified)
        bool isBuy = (strategy_.inventory() >= 0);  // Approximate
        
        if (isBuy) {
            metrics_.numBuys++;
            // Update average cost
            double newCost = fill.fillPrice * fill.fillQty;
            avgCost_ = (avgCost_ * std::abs(strategy_.inventory()) + newCost) /
                       (std::abs(strategy_.inventory()) + fill.fillQty);
            strategy_.onFill(OrderSide::Buy, fill.fillQty);
        } else {
            metrics_.numSells++;
            // Realize PnL
            double realizePnL = (fill.fillPrice - avgCost_) * fill.fillQty;
            metrics_.realizedPnL += realizePnL;
            strategy_.onFill(OrderSide::Sell, fill.fillQty);
        }
        
        metrics_.maxInventory = std::max(metrics_.maxInventory, 
                                          std::abs(strategy_.inventory()));
    }

    void updateUnrealizedPnL(double currentMid) {
        metrics_.unrealizedPnL = (currentMid - avgCost_) * strategy_.inventory();
        metrics_.totalPnL = metrics_.realizedPnL + metrics_.unrealizedPnL;
        
        // Track drawdown
        metrics_.peakPnL = std::max(metrics_.peakPnL, metrics_.totalPnL);
        double drawdown = metrics_.peakPnL - metrics_.totalPnL;
        metrics_.maxDrawdown = std::max(metrics_.maxDrawdown, drawdown);
        
        // Store for Sharpe
        pnlHistory_.push_back(metrics_.totalPnL);
    }

    [[nodiscard]] double estimateVolatility() const {
        // Simple realized vol from price changes
        if (pnlHistory_.size() < 20) return 1.0;
        
        std::vector<double> returns;
        for (size_t i = 1; i < pnlHistory_.size(); ++i) {
            double ret = pnlHistory_[i] - pnlHistory_[i-1];
            returns.push_back(ret);
        }
        
        double mean = std::accumulate(returns.begin(), returns.end(), 0.0) / returns.size();
        double sq_sum = 0;
        for (double r : returns) sq_sum += (r - mean) * (r - mean);
        
        return std::sqrt(sq_sum / returns.size());
    }

    [[nodiscard]] double computeSharpe() const {
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
        
        // Annualize (assume 1 tick = 1 second, 252 trading days, 6.5 hours)
        double annualizationFactor = std::sqrt(252 * 6.5 * 3600);
        return (mean / std) * annualizationFactor;
    }
};

} // namespace backtest
