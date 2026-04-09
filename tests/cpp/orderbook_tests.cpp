/**
 * @file orderbook_tests.cpp
 * @brief Unit tests for OrderBook correctness and invariants.
 * 
 * Tests:
 *   1. Add/cancel basic operations
 *   2. Generation counter safety
 *   3. Self-trade prevention
 *   4. IOC/FOK order handling
 *   5. Best bid/ask tracking
 *   6. Invariant verification
 */

#include <cassert>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include "../../src/core/OrderBook.hpp"
#include "../../src/core/MatchingStrategy.hpp"
#include "../../src/core/TickNormalizer.hpp"

// ============================================================================
// Test Framework (minimal)
// ============================================================================

static int testsRun = 0;
static int testsPassed = 0;

#define TEST(name) void name()
#define RUN_TEST(name) do { \
    testsRun++; \
    std::cout << "  Running: " << #name << "... "; \
    try { name(); testsPassed++; std::cout << "PASS\n"; } \
    catch (const std::exception& e) { std::cout << "FAIL: " << e.what() << "\n"; } \
} while(0)

#define ASSERT(cond) do { if (!(cond)) throw std::runtime_error("Assertion failed: " #cond); } while(0)
#define ASSERT_EQ(a, b) do { if ((a) != (b)) throw std::runtime_error("Assertion failed: " #a " == " #b); } while(0)

// ============================================================================
// Tests
// ============================================================================

TEST(test_add_order) {
    OrderBook book;
    
    Order order(0, 12345, 0, OrderSide::Buy, OrderType::Limit, 5000, 100);
    ASSERT(book.addOrder(order));
    ASSERT(book.hasBids());
    ASSERT_EQ(book.getBestBid(), 5000);
}

TEST(test_cancel_order) {
    OrderBook book;
    
    Order order(0, 12345, 0, OrderSide::Buy, OrderType::Limit, 5000, 100);
    book.addOrder(order);
    
    ASSERT(book.cancelOrder(0));
    ASSERT(!book.hasBids());
    ASSERT_EQ(book.getBestBid(), PRICE_INVALID);
    
    // Double cancel should fail
    ASSERT(!book.cancelOrder(0));
}

TEST(test_generation_safety) {
    OrderBook book;
    
    // Add order in generation 0
    Order order(0, 12345, 0, OrderSide::Buy, OrderType::Limit, 5000, 100);
    book.addOrder(order);
    uint32_t gen0 = book.generation();
    
    // Reset (increments generation)
    book.reset();
    uint32_t gen1 = book.generation();
    ASSERT(gen1 > gen0);
    
    // Cancel with old generation should fail
    ASSERT(!book.cancelOrder(0));
    
    // Add same ID in new generation
    Order order2(0, 12346, 0, OrderSide::Sell, OrderType::Limit, 6000, 50);
    ASSERT(book.addOrder(order2));
    
    // New cancel should work
    ASSERT(book.cancelOrder(0));
}

TEST(test_best_bid_ask_tracking) {
    OrderBook book;
    
    // Add multiple bids
    book.addOrder(Order(0, 1, 0, OrderSide::Buy, OrderType::Limit, 5000, 100));
    book.addOrder(Order(1, 2, 0, OrderSide::Buy, OrderType::Limit, 5100, 100));
    book.addOrder(Order(2, 3, 0, OrderSide::Buy, OrderType::Limit, 4900, 100));
    
    ASSERT_EQ(book.getBestBid(), 5100);
    
    // Cancel best bid
    book.cancelOrder(1);
    ASSERT_EQ(book.getBestBid(), 5000);
    
    // Add asks
    book.addOrder(Order(3, 4, 0, OrderSide::Sell, OrderType::Limit, 5200, 100));
    book.addOrder(Order(4, 5, 0, OrderSide::Sell, OrderType::Limit, 5300, 100));
    
    ASSERT_EQ(book.getBestAsk(), 5200);
    
    // Cancel best ask
    book.cancelOrder(3);
    ASSERT_EQ(book.getBestAsk(), 5300);
}

TEST(test_duplicate_order_rejection) {
    OrderBook book;
    
    Order order(0, 12345, 0, OrderSide::Buy, OrderType::Limit, 5000, 100);
    ASSERT(book.addOrder(order));
    
    // Same ID should be rejected
    RejectReason reason;
    Order order2(0, 12346, 0, OrderSide::Sell, OrderType::Limit, 6000, 50);
    ASSERT(!book.addOrder(order2, reason));
    ASSERT_EQ(static_cast<int>(reason), static_cast<int>(RejectReason::DuplicateOrderId));
}

TEST(test_modify_order) {
    OrderBook book;
    
    Order order(0, 12345, 0, OrderSide::Buy, OrderType::Limit, 5000, 100);
    book.addOrder(order);
    
    // Reduce quantity
    ASSERT(book.modifyOrder(0, 50));
    
    // Cannot increase
    ASSERT(!book.modifyOrder(0, 150));
    
    // Reduce to zero = cancel
    ASSERT(book.modifyOrder(0, 0));
    ASSERT(!book.hasBids());
}

TEST(test_price_validation) {
    OrderBook book;
    RejectReason reason;
    
    // Invalid price (negative)
    Order badPrice1(0, 1, 0, OrderSide::Buy, OrderType::Limit, -1, 100);
    ASSERT(!book.addOrder(badPrice1, reason));
    ASSERT_EQ(static_cast<int>(reason), static_cast<int>(RejectReason::InvalidPrice));
    
    // Invalid price (too high)
    Order badPrice2(1, 2, 0, OrderSide::Buy, OrderType::Limit, OrderBook::MAX_PRICE, 100);
    ASSERT(!book.addOrder(badPrice2, reason));
    ASSERT_EQ(static_cast<int>(reason), static_cast<int>(RejectReason::InvalidPrice));
    
    // Invalid quantity (zero)
    Order badQty(2, 3, 0, OrderSide::Buy, OrderType::Limit, 5000, 0);
    ASSERT(!book.addOrder(badQty, reason));
    ASSERT_EQ(static_cast<int>(reason), static_cast<int>(RejectReason::InvalidQuantity));
}

TEST(test_self_trade_prevention) {
    OrderBook book;
    StandardMatchingStrategy strategy;
    std::vector<Trade> trades;
    
    // Add resting order from client 1
    Order resting(0, 1, 1, 0, OrderSide::Sell, OrderType::Limit, 
                  TimeInForce::GTC, 5000, 100);
    book.addOrder(resting);
    
    // Incoming order from same client should trigger STP
    Order incoming(1, 2, 1, 0, OrderSide::Buy, OrderType::Limit,
                   TimeInForce::GTC, 5000, 100);
    
    strategy.match(book, incoming, trades);
    
    // Default STP = CancelNewest, so no trades should occur
    ASSERT_EQ(trades.size(), 0u);
    ASSERT(!incoming.isActive());  // Incoming was cancelled
}

TEST(test_ioc_order) {
    OrderBook book;
    StandardMatchingStrategy strategy;
    std::vector<Trade> trades;
    
    // Add partial liquidity
    Order resting(0, 1, 0, OrderSide::Sell, OrderType::Limit, 5000, 50);
    book.addOrder(resting);
    
    // IOC order for more than available
    Order ioc(1, 2, 0, 0, OrderSide::Buy, OrderType::Limit,
              TimeInForce::IOC, 5000, 100);
    
    strategy.match(book, ioc, trades);
    
    // Should have filled 50, remaining 50 cancelled
    ASSERT_EQ(trades.size(), 1u);
    ASSERT_EQ(trades[0].quantity, 50u);
    ASSERT(!ioc.isActive());  // IOC cancelled remainder
}

TEST(test_fok_order) {
    OrderBook book;
    StandardMatchingStrategy strategy;
    std::vector<Trade> trades;
    
    // Add partial liquidity
    Order resting(0, 1, 0, OrderSide::Sell, OrderType::Limit, 5000, 50);
    book.addOrder(resting);
    
    // FOK order for more than available - should reject
    Order fok(1, 2, 0, 0, OrderSide::Buy, OrderType::Limit,
              TimeInForce::FOK, 5000, 100);
    
    strategy.match(book, fok, trades);
    
    // Should have no trades (FOK rejected)
    ASSERT_EQ(trades.size(), 0u);
    ASSERT(!fok.isActive());
    
    // Resting order should still be there
    ASSERT(book.hasAsks());
}

TEST(test_tick_normalizer) {
    TickNormalizer norm(0.01);  // $0.01 tick
    
    ASSERT_EQ(norm.toTicks(150.25), 15025);
    ASSERT_EQ(norm.toTicks(0.01), 1);
    ASSERT_EQ(norm.toTicks(999.99), 99999);
    
    // Round-trip
    double price = 150.25;
    Price ticks = norm.toTicks(price);
    double recovered = norm.toPrice(ticks);
    ASSERT(std::abs(recovered - price) < 0.0001);
}

// ============================================================================
// Main
// ============================================================================

int main() {
    std::cout << "============================================\n";
    std::cout << "   OrderBook Unit Tests\n";
    std::cout << "============================================\n\n";

    RUN_TEST(test_add_order);
    RUN_TEST(test_cancel_order);
    RUN_TEST(test_generation_safety);
    RUN_TEST(test_best_bid_ask_tracking);
    RUN_TEST(test_duplicate_order_rejection);
    RUN_TEST(test_modify_order);
    RUN_TEST(test_price_validation);
    RUN_TEST(test_self_trade_prevention);
    RUN_TEST(test_ioc_order);
    RUN_TEST(test_fok_order);
    RUN_TEST(test_tick_normalizer);

    std::cout << "\n============================================\n";
    std::cout << "   Results: " << testsPassed << "/" << testsRun << " passed\n";
    std::cout << "============================================\n";

    return (testsPassed == testsRun) ? 0 : 1;
}
