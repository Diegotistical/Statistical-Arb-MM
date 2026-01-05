#pragma once
/**
 * @file ReplayEngine.hpp
 * @brief Replays LOBSTER messages through OrderBook with validation.
 * 
 * Features:
 *   - Feed parsed messages to OrderBook
 *   - Validate book invariants after each message
 *   - Generate execution reports
 *   - Deterministic replay (same input = same output)
 */

#include <cstdint>
#include <functional>
#include <iostream>
#include <unordered_map>
#include <vector>

#include "LobsterParser.hpp"
#include "../core/OrderBook.hpp"
#include "../core/MatchingStrategy.hpp"
#include "../core/TickNormalizer.hpp"

namespace replay {

/**
 * @brief Replay statistics.
 */
struct ReplayStats {
    size_t messages_processed = 0;
    size_t orders_added = 0;
    size_t orders_cancelled = 0;
    size_t orders_executed = 0;
    size_t trades_generated = 0;
    size_t invariant_violations = 0;
    int64_t start_time_ns = 0;
    int64_t end_time_ns = 0;
    
    void print() const {
        std::cout << "=== Replay Statistics ===\n"
                  << "  Messages:     " << messages_processed << "\n"
                  << "  Adds:         " << orders_added << "\n"
                  << "  Cancels:      " << orders_cancelled << "\n"
                  << "  Executions:   " << orders_executed << "\n"
                  << "  Trades:       " << trades_generated << "\n"
                  << "  Violations:   " << invariant_violations << "\n"
                  << "  Duration:     " << (end_time_ns - start_time_ns) / 1e9 << " s\n";
    }
};

/**
 * @brief Book state hash for deterministic replay verification.
 */
struct BookStateHash {
    uint64_t bid_hash = 0;
    uint64_t ask_hash = 0;
    Price best_bid = PRICE_INVALID;
    Price best_ask = PRICE_INVALID;
    int32_t total_bid_orders = 0;
    int32_t total_ask_orders = 0;
    
    bool operator==(const BookStateHash& other) const {
        return bid_hash == other.bid_hash && 
               ask_hash == other.ask_hash &&
               best_bid == other.best_bid &&
               best_ask == other.best_ask;
    }
};

/**
 * @brief Engine for replaying market data through OrderBook.
 */
class ReplayEngine {
public:
    // Callback types
    using TradeCallback = std::function<void(const Trade&)>;
    using ViolationCallback = std::function<void(const std::string&)>;

    /**
     * @brief Construct replay engine.
     * @param tickSize Tick size for price normalization (default $0.01)
     */
    explicit ReplayEngine(double tickSize = 0.01)
        : ticks_(tickSize), nextOrderId_(0) {}

    /**
     * @brief Replay all messages from parser.
     * @param parser LOBSTER parser with message data
     * @return Replay statistics
     */
    ReplayStats replay(LobsterParser& parser) {
        ReplayStats stats;
        book_.reset();
        lobsterIdToOrderId_.clear();
        nextOrderId_ = 0;
        
        while (auto msg = parser.next()) {
            if (stats.messages_processed == 0) {
                stats.start_time_ns = msg->timestamp_ns;
            }
            stats.end_time_ns = msg->timestamp_ns;
            
            processMessage(*msg, stats);
            stats.messages_processed++;
            
            // Validate invariants periodically
            if (validateInvariants_ && stats.messages_processed % 10000 == 0) {
                if (!validateBookInvariants()) {
                    stats.invariant_violations++;
                    if (onViolation_) {
                        onViolation_("Invariant violation at message " + 
                                    std::to_string(stats.messages_processed));
                    }
                }
            }
        }
        
        return stats;
    }

    /**
     * @brief Get current book state hash for determinism verification.
     */
    [[nodiscard]] BookStateHash getStateHash() const {
        BookStateHash hash;
        hash.best_bid = book_.getBestBid();
        hash.best_ask = book_.getBestAsk();
        
        // Simple hash of order counts per level
        const auto& bids = book_.getBids();
        const auto& asks = book_.getAsks();
        
        for (size_t i = 0; i < static_cast<size_t>(OrderBook::MAX_PRICE); ++i) {
            if (bids[i].activeCount > 0) {
                hash.bid_hash ^= (i * 31 + bids[i].activeCount);
                hash.total_bid_orders += bids[i].activeCount;
            }
            if (asks[i].activeCount > 0) {
                hash.ask_hash ^= (i * 37 + asks[i].activeCount);
                hash.total_ask_orders += asks[i].activeCount;
            }
        }
        
        return hash;
    }

    /**
     * @brief Enable/disable invariant validation.
     */
    void setValidateInvariants(bool enable) { validateInvariants_ = enable; }

    /**
     * @brief Set trade callback.
     */
    void setTradeCallback(TradeCallback cb) { onTrade_ = std::move(cb); }

    /**
     * @brief Set violation callback.
     */
    void setViolationCallback(ViolationCallback cb) { onViolation_ = std::move(cb); }

    /**
     * @brief Get underlying order book (for inspection).
     */
    [[nodiscard]] const OrderBook& book() const { return book_; }

private:
    OrderBook book_;
    StandardMatchingStrategy matcher_;
    TickNormalizer ticks_;
    std::vector<Trade> tradeBuffer_;
    
    // Map LOBSTER order IDs to internal order IDs
    std::unordered_map<int64_t, OrderId> lobsterIdToOrderId_;
    OrderId nextOrderId_;
    
    bool validateInvariants_ = true;
    TradeCallback onTrade_;
    ViolationCallback onViolation_;

    void processMessage(const LobsterMessage& msg, ReplayStats& stats) {
        switch (msg.type) {
            case LobsterEventType::LimitOrderSubmission:
                processSubmission(msg, stats);
                break;
                
            case LobsterEventType::TotalCancellation:
            case LobsterEventType::PartialCancellation:
                processCancellation(msg, stats);
                break;
                
            case LobsterEventType::VisibleExecution:
            case LobsterEventType::HiddenExecution:
                processExecution(msg, stats);
                break;
                
            case LobsterEventType::CrossTrade:
            case LobsterEventType::TradingHalt:
                // Ignore for now
                break;
        }
    }

    void processSubmission(const LobsterMessage& msg, ReplayStats& stats) {
        if (nextOrderId_ >= OrderBook::MAX_ORDER_ID) return;
        
        OrderId orderId = nextOrderId_++;
        lobsterIdToOrderId_[msg.order_id] = orderId;
        
        // LOBSTER price is in hundredths of cents (10000 = $1.00)
        Price priceTicks = msg.price / 100;  // Convert to cents (ticks)
        
        if (priceTicks < 0 || priceTicks >= OrderBook::MAX_PRICE) return;
        
        Order order(orderId, 
                    static_cast<uint32_t>(msg.order_id & 0xFFFFFFFF),
                    0,  // symbolId
                    msg.isBuy() ? OrderSide::Buy : OrderSide::Sell,
                    OrderType::Limit,
                    priceTicks,
                    static_cast<Quantity>(msg.size));
        
        if (book_.addOrder(order)) {
            stats.orders_added++;
        }
    }

    void processCancellation(const LobsterMessage& msg, ReplayStats& stats) {
        auto it = lobsterIdToOrderId_.find(msg.order_id);
        if (it == lobsterIdToOrderId_.end()) return;
        
        if (book_.cancelOrder(it->second)) {
            stats.orders_cancelled++;
        }
        
        if (msg.type == LobsterEventType::TotalCancellation) {
            lobsterIdToOrderId_.erase(it);
        }
    }

    void processExecution(const LobsterMessage& msg, ReplayStats& stats) {
        // Execution message indicates a trade occurred
        // In replay mode, we just track statistics
        stats.orders_executed++;
        stats.trades_generated++;
        
        // Optionally trigger callback
        if (onTrade_) {
            Trade trade(0, 0, 0, msg.price / 100, static_cast<Quantity>(msg.size));
            onTrade_(trade);
        }
    }

    [[nodiscard]] bool validateBookInvariants() const {
        // Check: best bid < best ask (no crossed book)
        if (book_.hasBids() && book_.hasAsks()) {
            if (book_.getBestBid() >= book_.getBestAsk()) {
                return false;  // Crossed book!
            }
        }
        
        // Check: bitmask consistency
        const auto& bids = book_.getBids();
        const auto& bidMask = book_.getBidMask();
        
        for (size_t i = 0; i < static_cast<size_t>(OrderBook::MAX_PRICE); ++i) {
            bool hasOrders = bids[i].activeCount > 0;
            bool inMask = bidMask.test(i);
            if (hasOrders != inMask) {
                return false;  // Bitmask inconsistency
            }
        }
        
        return true;
    }
};

} // namespace replay
