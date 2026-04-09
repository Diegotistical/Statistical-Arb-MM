#pragma once
/**
 * @file MatchingStrategy.hpp
 * @brief Price-time priority matching with CRTP for hot-path dispatch.
 *
 * Design decision: CRTP vs virtual dispatch
 *
 * The matching engine is on the critical path (every order passes through it).
 * Virtual dispatch adds ~5-10ns per call from:
 *   1. vtable pointer dereference
 *   2. Indirect branch prediction penalty
 *   3. Inlining prevention (compiler can't see through virtual)
 *
 * CRTP (Curiously Recurring Template Pattern) eliminates all three:
 *   - match() is resolved at compile time
 *   - The compiler can inline matchImpl() into the caller
 *   - Branch predictor never misses (direct call)
 *
 * At 16M ops/sec (61ns per op), saving 5-10ns is an 8-16% improvement.
 * This is the textbook use case for CRTP in HFT systems.
 *
 * The old virtual interface is preserved as MatchingStrategy for
 * backward compatibility with code that doesn't need hot-path performance.
 */

#include <algorithm>
#include <vector>

#include "OrderBook.hpp"

// ============================================================================
// CRTP Base (Compile-Time Polymorphism for Hot Path)
// ============================================================================

/**
 * @brief CRTP base for matching strategies on the hot path.
 *
 * Usage:
 * @code
 *   class MyStrategy : public MatchingStrategyBase<MyStrategy> {
 *   public:
 *       void matchImpl(OrderBook& book, Order& incoming, std::vector<Trade>& trades) {
 *           // Your matching logic here
 *       }
 *   };
 *
 *   MyStrategy strategy;
 *   strategy.match(book, order, trades);  // Compile-time dispatch, inlinable
 * @endcode
 */
template <typename Derived>
class MatchingStrategyBase {
public:
    /**
     * @brief Match incoming order against book (CRTP dispatch).
     * Resolved at compile time. The compiler can inline Derived::matchImpl().
     */
    void match(OrderBook& book, Order& incoming, std::vector<Trade>& trades) {
        static_cast<Derived*>(this)->matchImpl(book, incoming, trades);
    }
};

// ============================================================================
// Virtual Base (Runtime Polymorphism for Flexibility)
// ============================================================================

/**
 * @brief Virtual matching strategy interface (for when runtime polymorphism is needed).
 *
 * Use this when the strategy type isn't known at compile time (e.g., config-driven).
 * Prefer MatchingStrategyBase<Derived> on the hot path.
 */
class MatchingStrategy {
public:
    MatchingStrategy() = default;
    virtual ~MatchingStrategy() = default;

    MatchingStrategy(const MatchingStrategy&) = default;
    MatchingStrategy& operator=(const MatchingStrategy&) = default;
    MatchingStrategy(MatchingStrategy&&) = default;
    MatchingStrategy& operator=(MatchingStrategy&&) = default;

    virtual void match(OrderBook& book, Order& incoming,
                       std::vector<Trade>& trades) = 0;
};

// ============================================================================
// Standard Price-Time Priority (Both CRTP and Virtual)
// ============================================================================

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
 *
 * Inherits from both CRTP base (for hot path) and virtual base
 * (for backward compatibility). Use through CRTP when possible.
 */
class StandardMatchingStrategy
    : public MatchingStrategyBase<StandardMatchingStrategy>
    , public MatchingStrategy {
public:
    STPAction defaultSTPAction = STPAction::CancelNewest;

    // Virtual dispatch entry point
    void match(OrderBook& book, Order& incoming,
               std::vector<Trade>& trades) override {
        matchImpl(book, incoming, trades);
    }

    // CRTP dispatch entry point (called by MatchingStrategyBase::match)
    void matchImpl(OrderBook& book, Order& incoming,
                   std::vector<Trade>& trades) {

        // FOK: Check if can fully fill before starting
        if (incoming.isFOK()) {
            Quantity available = getAvailableQuantity(book, incoming);
            if (available < incoming.quantity) {
                incoming.deactivate();
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

    void matchBuyOrder(OrderBook& book, Order& incoming,
                       std::vector<Trade>& trades) {
        if (!book.hasAsks()) return;

        Price p = book.getBestAsk();
        auto& askMask = book.getAskMask();
        auto& asks = book.getAsks();

        while (p < OrderBook::MAX_PRICE && p != PRICE_INVALID) {
            if (!askMask.test(static_cast<size_t>(p))) [[unlikely]] {
                size_t next = askMask.findFirstSet(static_cast<size_t>(p));
                if (next >= static_cast<size_t>(OrderBook::MAX_PRICE)) break;
                p = static_cast<Price>(next);
            }

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
    }

    void matchSellOrder(OrderBook& book, Order& incoming,
                        std::vector<Trade>& trades) {
        if (!book.hasBids()) return;

        Price p = book.getBestBid();
        auto& bidMask = book.getBidMask();
        auto& bids = book.getBids();

        while (p >= 0 && p != PRICE_INVALID) {
            if (!bidMask.test(static_cast<size_t>(p))) [[unlikely]] {
                if (p == 0) break;
                size_t next = bidMask.findFirstSetDown(static_cast<size_t>(p) - 1);
                if (next >= static_cast<size_t>(OrderBook::MAX_PRICE)) break;
                p = static_cast<Price>(next);
                if (!bidMask.test(static_cast<size_t>(p))) break;
            }

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

    void matchAtLevel(OrderBook& book, PriceLevel& level, Order& incoming,
                      std::vector<Trade>& trades, bool isAsk, Price p) {

        const size_t size = level.orders.size();

        for (size_t i = level.headIndex; i < size; ++i) {
            Order& resting = level.orders[i];

            if (!resting.isActive()) [[unlikely]] {
                if (i == level.headIndex) level.headIndex++;
                continue;
            }

            if (resting.clientId != 0 && resting.clientId == incoming.clientId) {
                handleSelfTrade(book, resting, incoming, level, i);
                if (!incoming.isActive() || incoming.quantity == 0) break;
                continue;
            }

            Quantity qty = std::min(incoming.quantity, resting.quantity);

            trades.emplace_back(
                resting.id, incoming.id, incoming.symbolId,
                resting.price, qty,
                resting.clientId, incoming.clientId
            );

            resting.quantity -= qty;
            incoming.quantity -= qty;

            if (resting.quantity == 0) {
                resting.deactivate();
                level.activeCount--;
                if (i == level.headIndex) level.headIndex++;
            }

            if (incoming.quantity == 0) break;
        }
    }

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
