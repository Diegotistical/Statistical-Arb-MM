#pragma once
/**
 * @file MatchingStrategy.hpp
 * @brief Price-time priority matching with self-trade prevention.
 * 
 * Features:
 *   - Price-time priority (FIFO at each price level)
 *   - Self-trade prevention (STP) based on clientId
 *   - IOC/FOK order handling
 *   - Efficient bitmask-based level skipping
 * 
 * Self-Trade Prevention:
 *   When an incoming order would match against a resting order from
 *   the same client (clientId), the STP action determines behavior:
 *   - CancelNewest: Cancel incoming order (default)
 *   - CancelOldest: Cancel resting order
 *   - CancelBoth: Cancel both orders
 * 
 * @note For production, STP should be configurable per client.
 */

#include <algorithm>
#include <vector>

#include "OrderBook.hpp"

/**
 * @brief Abstract matching strategy interface.
 */
class MatchingStrategy {
public:
    MatchingStrategy() = default;
    virtual ~MatchingStrategy() = default;

    MatchingStrategy(const MatchingStrategy&) = default;
    MatchingStrategy& operator=(const MatchingStrategy&) = default;
    MatchingStrategy(MatchingStrategy&&) = default;
    MatchingStrategy& operator=(MatchingStrategy&&) = default;

    /**
     * @brief Match incoming order against book.
     * 
     * @param book Order book to match against
     * @param incoming Incoming order (modified: quantity reduced)
     * @param trades Output vector for generated trades
     */
    virtual void match(OrderBook& book, Order& incoming, 
                       std::vector<Trade>& trades) = 0;
};

/**
 * @brief Standard price-time priority matching with STP.
 * 
 * Matching rules:
 *   1. Buy orders match against asks from lowest to highest price
 *   2. Sell orders match against bids from highest to lowest price
 *   3. At each price level, orders match in FIFO order
 *   4. Self-trades are prevented based on STPAction
 *   5. IOC orders cancel unfilled quantity after matching
 *   6. FOK orders are rejected if cannot fully fill
 */
class StandardMatchingStrategy : public MatchingStrategy {
public:
    /** @brief Default STP action for this strategy */
    STPAction defaultSTPAction = STPAction::CancelNewest;

    void match(OrderBook& book, Order& incoming, 
               std::vector<Trade>& trades) override {
        
        // FOK: Check if can fully fill before starting
        if (incoming.isFOK()) {
            Quantity available = getAvailableQuantity(book, incoming);
            if (available < incoming.quantity) {
                incoming.deactivate();  // Reject FOK
                return;
            }
        }

        // Handle market orders - set aggressive price
        if (incoming.type == OrderType::Market) {
            incoming.price = (incoming.side == OrderSide::Buy) 
                ? OrderBook::MAX_PRICE 
                : 0;
        }

        // Match against opposite side
        if (incoming.side == OrderSide::Buy) {
            matchBuyOrder(book, incoming, trades);
        } else {
            matchSellOrder(book, incoming, trades);
        }

        // IOC: Cancel any remaining quantity
        if (incoming.isIOC() && incoming.quantity > 0) {
            incoming.quantity = 0;
            incoming.deactivate();
        }

        // Add remaining to book if GTC limit order
        if (incoming.quantity > 0 && 
            incoming.type == OrderType::Limit &&
            incoming.tif == TimeInForce::GTC) {
            book.addOrder(incoming);
        }
    }

private:
    /**
     * @brief Get total available quantity on opposite side.
     */
    Quantity getAvailableQuantity(OrderBook& book, const Order& incoming) {
        Quantity total = 0;
        
        if (incoming.side == OrderSide::Buy) {
            if (!book.hasAsks()) return 0;
            
            auto& askMask = book.getAskMask();
            auto& asks = book.getAsks();
            
            size_t p = askMask.findFirstSet(0);
            while (p < static_cast<size_t>(OrderBook::MAX_PRICE)) {
                if (static_cast<Price>(p) > incoming.price && 
                    incoming.type == OrderType::Limit) break;
                    
                total += asks[p].totalVolume();
                if (total >= incoming.quantity) return total;
                
                p = askMask.findFirstSet(p + 1);
            }
        } else {
            if (!book.hasBids()) return 0;
            
            auto& bidMask = book.getBidMask();
            auto& bids = book.getBids();
            
            size_t p = bidMask.findFirstSetDown(OrderBook::MAX_PRICE - 1);
            while (p < static_cast<size_t>(OrderBook::MAX_PRICE)) {
                if (static_cast<Price>(p) < incoming.price && 
                    incoming.type == OrderType::Limit) break;
                    
                total += bids[p].totalVolume();
                if (total >= incoming.quantity) return total;
                
                if (p == 0) break;
                p = bidMask.findFirstSetDown(p - 1);
            }
        }
        
        return total;
    }

    /**
     * @brief Match buy order against asks.
     */
    void matchBuyOrder(OrderBook& book, Order& incoming, 
                       std::vector<Trade>& trades) {
        if (!book.hasAsks()) return;

        Price p = book.getBestAsk();
        auto& askMask = book.getAskMask();
        auto& asks = book.getAsks();

        while (p < OrderBook::MAX_PRICE && p != PRICE_INVALID) {
            // Skip empty levels
            if (!askMask.test(static_cast<size_t>(p))) [[unlikely]] {
                size_t next = askMask.findFirstSet(static_cast<size_t>(p));
                if (next >= static_cast<size_t>(OrderBook::MAX_PRICE)) break;
                p = static_cast<Price>(next);
            }

            // Price limit check
            if (p > incoming.price && incoming.type == OrderType::Limit) [[unlikely]] {
                break;
            }

            auto& level = asks[p];
            if (level.activeCount > 0) {
                matchAtLevel(book, level, incoming, trades, true, p);
                
                if (level.activeCount == 0) [[unlikely]] {
                    askMask.clear(static_cast<size_t>(p));
                }
            }

            if (incoming.quantity == 0) break;
            p++;
        }

        // Update best ask
        if (book.hasAsks()) {
            size_t newBest = askMask.findFirstSet(0);
            // Best ask update will be reflected via askMask
        }
    }

    /**
     * @brief Match sell order against bids.
     */
    void matchSellOrder(OrderBook& book, Order& incoming, 
                        std::vector<Trade>& trades) {
        if (!book.hasBids()) return;

        Price p = book.getBestBid();
        auto& bidMask = book.getBidMask();
        auto& bids = book.getBids();

        while (p >= 0 && p != PRICE_INVALID) {
            // Skip empty levels
            if (!bidMask.test(static_cast<size_t>(p))) [[unlikely]] {
                if (p == 0) break;
                size_t next = bidMask.findFirstSetDown(static_cast<size_t>(p) - 1);
                if (next >= static_cast<size_t>(OrderBook::MAX_PRICE)) break;
                p = static_cast<Price>(next);
                if (!bidMask.test(static_cast<size_t>(p))) break;
            }

            // Price limit check
            if (p < incoming.price && incoming.type == OrderType::Limit) [[unlikely]] {
                break;
            }

            auto& level = bids[p];
            if (level.activeCount > 0) {
                matchAtLevel(book, level, incoming, trades, false, p);
                
                if (level.activeCount == 0) [[unlikely]] {
                    bidMask.clear(static_cast<size_t>(p));
                }
            }

            if (incoming.quantity == 0) break;
            if (p == 0) break;
            p--;
        }
    }

    /**
     * @brief Match at a single price level (FIFO).
     */
    void matchAtLevel(OrderBook& book, PriceLevel& level, Order& incoming,
                      std::vector<Trade>& trades, bool isAsk, Price p) {
        
        const size_t size = level.orders.size();
        
        for (size_t i = level.headIndex; i < size; ++i) {
            Order& resting = level.orders[i];
            
            // Skip inactive
            if (!resting.isActive()) [[unlikely]] {
                if (i == level.headIndex) level.headIndex++;
                continue;
            }

            // Self-trade prevention
            if (resting.clientId != 0 && resting.clientId == incoming.clientId) {
                handleSelfTrade(book, resting, incoming, level, i);
                if (!incoming.isActive() || incoming.quantity == 0) break;
                continue;
            }

            // Calculate match quantity
            Quantity qty = std::min(incoming.quantity, resting.quantity);

            // Record trade
            trades.emplace_back(
                resting.id, incoming.id, incoming.symbolId,
                resting.price, qty,
                resting.clientId, incoming.clientId
            );

            // Update quantities
            resting.quantity -= qty;
            incoming.quantity -= qty;

            // Handle fully filled resting order
            if (resting.quantity == 0) {
                resting.deactivate();
                level.activeCount--;
                if (i == level.headIndex) level.headIndex++;
            }

            if (incoming.quantity == 0) break;
        }
    }

    /**
     * @brief Handle self-trade based on STP action.
     */
    void handleSelfTrade(OrderBook& book, Order& resting, Order& incoming,
                         PriceLevel& level, size_t restingIndex) {
        switch (defaultSTPAction) {
            case STPAction::CancelNewest:
                incoming.deactivate();
                break;
                
            case STPAction::CancelOldest:
                resting.deactivate();
                level.activeCount--;
                if (restingIndex == level.headIndex) level.headIndex++;
                break;
                
            case STPAction::CancelBoth:
                incoming.deactivate();
                resting.deactivate();
                level.activeCount--;
                if (restingIndex == level.headIndex) level.headIndex++;
                break;
        }
    }
};
