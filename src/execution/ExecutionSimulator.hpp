#pragma once
/**
 * @file ExecutionSimulator.hpp
 * @brief Simulates realistic order execution with latency and slippage.
 * 
 * Features:
 *   - Fixed latency + random jitter
 *   - Queue position modeling
 *   - Partial fills based on queue depth
 *   - Adverse selection tied to OFI
 *   - Explicit timestamps
 */

#include <chrono>
#include <random>
#include <queue>
#include <functional>
#include <cmath>

#include "../core/Order.hpp"
#include "../core/OrderBook.hpp"
#include "../core/Timestamp.hpp"

namespace execution {

using core::Timestamp;

/**
 * @brief Pending order with submission timestamp.
 */
struct PendingOrder {
    Order order;
    Timestamp submitTime;     ///< Submission timestamp
    Timestamp arrivalTime;    ///< Expected arrival at exchange
    Quantity queueAhead = 0;  ///< Queue position at submission
};

/**
 * @brief Fill report from execution.
 */
struct FillReport {
    OrderId orderId;
    Price fillPrice;
    Quantity fillQty;
    Quantity remainingQty;
    Timestamp fillTime;
    bool complete;
    bool isMaker;             ///< True if passive fill
    double adverseSelectionCost = 0;  ///< Estimated AS cost
    Quantity queueAhead = 0;  ///< Queue position at fill
};

/**
 * @brief Simulates order execution with realistic latency.
 * 
 * Execution model:
 *   1. Order submitted → delay by latency + jitter
 *   2. On arrival, check if marketable
 *   3. If passive: track queue position, model partial fills
 *   4. Adverse selection probability tied to OFI
 */
class ExecutionSimulator {
public:
    /**
     * @brief Construct execution simulator.
     * @param baseLatencyNs Base latency in nanoseconds
     * @param jitterNs Random jitter (uniform distribution)
     * @param seed RNG seed for reproducibility
     */
    explicit ExecutionSimulator(int64_t baseLatencyNs = 5000,
                                 int64_t jitterNs = 1000,
                                 uint64_t seed = 42)
        : baseLatency_(baseLatencyNs),
          jitter_(jitterNs),
          rng_(seed),
          jitterDist_(-jitterNs, jitterNs),
          fillProbDist_(0.0, 1.0) {}

    /**
     * @brief Submit order for simulated execution.
     * @param order Order to submit
     * @param currentTime Current simulation time
     * @param queueAhead Shares ahead in queue (for passive orders)
     */
    void submit(const Order& order, Timestamp currentTime, Quantity queueAhead = 0) {
        int64_t latency = baseLatency_ + jitterDist_(rng_);
        PendingOrder pending;
        pending.order = order;
        pending.submitTime = currentTime;
        pending.arrivalTime = currentTime + latency;
        pending.queueAhead = queueAhead;
        pendingOrders_.push(pending);
    }

    /**
     * @brief Process pending orders against book.
     * @param book Order book to match against
     * @param currentTime Current simulation time
     * @param ofiNormalized Current normalized OFI (-1 to 1)
     * @return Vector of fill reports
     */
    std::vector<FillReport> process(OrderBook& book, Timestamp currentTime,
                                     double ofiNormalized = 0) {
        std::vector<FillReport> fills;
        
        while (!pendingOrders_.empty()) {
            auto& pending = pendingOrders_.front();
            
            // Check if order has arrived
            if (pending.arrivalTime > currentTime) {
                break;
            }
            
            // Simulate execution with queue position and OFI
            FillReport report = simulateFill(book, pending, currentTime, ofiNormalized);
            fills.push_back(report);
            
            pendingOrders_.pop();
        }
        
        return fills;
    }

    /**
     * @brief Compute adverse selection probability.
     * 
     * Higher OFI against your direction = higher AS probability.
     * 
     * @param side Order side
     * @param ofiNormalized Normalized OFI (-1 to 1)
     * @return Probability of adverse fill (0-1)
     */
    [[nodiscard]] double adverseSelectionProb(OrderSide side, 
                                               double ofiNormalized) const {
        // Buy order: adverse if OFI is negative (sell pressure)
        // Sell order: adverse if OFI is positive (buy pressure)
        if (side == OrderSide::Buy) {
            return std::max(0.0, -ofiNormalized * asMultiplier_);
        } else {
            return std::max(0.0, ofiNormalized * asMultiplier_);
        }
    }

    /**
     * @brief Compute fill probability based on queue position.
     * 
     * Model: P(fill) decreases with queue depth.
     * 
     * @param queueAhead Shares ahead in queue
     * @param elapsedNs Time since order placement (ns)
     * @param volume Trading volume in period
     * @return Fill probability (0-1)
     */
    [[nodiscard]] double fillProbability(Quantity queueAhead, 
                                          int64_t elapsedNs,
                                          Quantity volume) const {
        if (volume == 0) return 0;
        
        // Queue decay: position improves over time as orders get filled
        double decayFactor = std::exp(-decayRate_ * elapsedNs);
        Quantity effectiveQueue = static_cast<Quantity>(queueAhead * decayFactor);
        
        // Fill probability: inversely proportional to queue position
        // P(fill) = volume / (volume + queue_ahead)
        double prob = static_cast<double>(volume) / (volume + effectiveQueue);
        
        return std::min(1.0, prob);
    }

    /**
     * @brief Simulate partial fill based on available liquidity.
     * 
     * @param requestedQty Requested fill quantity
     * @param availableLiquidity Available liquidity at price level
     * @param queueAhead Queue position
     * @return Actual fill quantity
     */
    [[nodiscard]] Quantity partialFillQty(Quantity requestedQty,
                                           Quantity availableLiquidity,
                                           Quantity queueAhead) const {
        // Can only fill what's available after queue
        if (queueAhead >= availableLiquidity) {
            return 0;
        }
        
        Quantity available = availableLiquidity - queueAhead;
        return std::min(requestedQty, available);
    }

    /**
     * @brief Clear all pending orders.
     */
    void clear() {
        while (!pendingOrders_.empty()) pendingOrders_.pop();
    }

    /**
     * @brief Get count of pending orders.
     */
    [[nodiscard]] size_t pendingCount() const { return pendingOrders_.size(); }

    // Configuration
    void setBaseLatency(int64_t ns) { baseLatency_ = ns; }
    void setJitter(int64_t ns) { 
        jitter_ = ns; 
        jitterDist_ = std::uniform_int_distribution<int64_t>(-ns, ns); 
    }
    void setDecayRate(double rate) { decayRate_ = rate; }
    void setASMultiplier(double mult) { asMultiplier_ = mult; }

private:
    int64_t baseLatency_;
    int64_t jitter_;
    double decayRate_ = 0.0000007;  // Queue decay per nanosecond
    double asMultiplier_ = 0.5;     // Adverse selection sensitivity
    
    std::mt19937_64 rng_;
    std::uniform_int_distribution<int64_t> jitterDist_;
    std::uniform_real_distribution<double> fillProbDist_;
    std::queue<PendingOrder> pendingOrders_;

    FillReport simulateFill(OrderBook& book, PendingOrder& pending, 
                            Timestamp currentTime, double ofiNormalized) {
        FillReport report;
        report.orderId = pending.order.id;
        report.fillTime = currentTime;
        report.remainingQty = pending.order.quantity;
        report.fillQty = 0;
        report.complete = false;
        report.isMaker = false;
        report.queueAhead = pending.queueAhead;
        
        const Order& order = pending.order;
        
        // Check marketability
        if (order.side == OrderSide::Buy) {
            if (!book.hasAsks()) {
                // No liquidity, order rests as maker
                (void)book.addOrder(order);
                report.isMaker = true;
                return report;
            }
            
            Price bestAsk = book.getBestAsk();
            if (order.price >= bestAsk) {
                // Marketable - taker
                const auto& askLevel = book.getLevel(bestAsk, OrderSide::Sell);
                Quantity available = askLevel.totalVolume();
                
                // Partial fill based on queue and liquidity
                report.fillQty = partialFillQty(order.quantity, available, 0);
                report.fillPrice = bestAsk;
                report.remainingQty = order.quantity - report.fillQty;
                report.complete = (report.remainingQty == 0);
                report.isMaker = false;
                
                // Adverse selection cost
                double asProb = adverseSelectionProb(order.side, ofiNormalized);
                report.adverseSelectionCost = report.fillQty * asProb * 0.01;  // Scale factor
                
            } else {
                // Passive - add to book as maker
                (void)book.addOrder(order);
                report.isMaker = true;
            }
        } else {  // Sell
            if (!book.hasBids()) {
                (void)book.addOrder(order);
                report.isMaker = true;
                return report;
            }
            
            Price bestBid = book.getBestBid();
            if (order.price <= bestBid) {
                // Marketable - taker
                const auto& bidLevel = book.getLevel(bestBid, OrderSide::Buy);
                Quantity available = bidLevel.totalVolume();
                
                report.fillQty = partialFillQty(order.quantity, available, 0);
                report.fillPrice = bestBid;
                report.remainingQty = order.quantity - report.fillQty;
                report.complete = (report.remainingQty == 0);
                report.isMaker = false;
                
                double asProb = adverseSelectionProb(order.side, ofiNormalized);
                report.adverseSelectionCost = report.fillQty * asProb * 0.01;
                
            } else {
                (void)book.addOrder(order);
                report.isMaker = true;
            }
        }
        
        return report;
    }
};

} // namespace execution
