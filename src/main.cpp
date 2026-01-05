/**
 * @file main.cpp
 * @brief Demo of single stock equity order matching engine.
 * 
 * Demonstrates:
 *   - Tick normalization (USD -> ticks)
 *   - Order book operations
 *   - Price-time priority matching
 *   - Self-trade prevention
 *   - IOC/FOK orders
 *   - Latency measurement
 */

#include <chrono>
#include <iomanip>
#include <iostream>
#include <vector>

#include "core/OrderBook.hpp"
#include "core/MatchingStrategy.hpp"
#include "core/TickNormalizer.hpp"

using Clock = std::chrono::high_resolution_clock;

// ============================================================================
// Demo: Single Stock Equity Matching
// ============================================================================

void demoEquityMatching() {
    std::cout << "\n=== Single Stock Equity Matching Demo ===\n\n";
    
    // Create order book and matching strategy
    OrderBook book;
    StandardMatchingStrategy matcher;
    std::vector<Trade> trades;
    
    // Tick normalizer for AAPL-like stock ($0.01 tick)
    TickNormalizer ticks(0.01);
    
    std::cout << "Tick size: $" << ticks.tickSize() << "\n\n";
    
    // ========================================================================
    // Step 1: Add resting orders (limit orders on book)
    // ========================================================================
    std::cout << "--- Adding Resting Orders ---\n";
    
    // Bids (buy orders)
    OrderId nextId = 0;
    
    struct OrderInput { double price; Quantity qty; const char* desc; };
    OrderInput bids[] = {
        {150.20, 100, "Client A bid"},
        {150.19, 200, "Client B bid"},
        {150.18, 150, "Client C bid"},
    };
    
    for (auto& b : bids) {
        Order order(nextId++, nextId, 0,
                    OrderSide::Buy, OrderType::Limit,
                    ticks.toTicks(b.price), b.qty);
        book.addOrder(order);
        std::cout << "  Added: " << b.desc << " @ $" << b.price 
                  << " x " << b.qty << " (tick=" << order.price << ")\n";
    }
    
    // Asks (sell orders)
    OrderInput asks[] = {
        {150.25, 100, "Client D ask"},
        {150.26, 200, "Client E ask"},
        {150.27, 150, "Client F ask"},
    };
    
    for (auto& a : asks) {
        Order order(nextId++, nextId, 0,
                    OrderSide::Sell, OrderType::Limit,
                    ticks.toTicks(a.price), a.qty);
        book.addOrder(order);
        std::cout << "  Added: " << a.desc << " @ $" << a.price 
                  << " x " << a.qty << " (tick=" << order.price << ")\n";
    }
    
    std::cout << "\nBook state:\n";
    std::cout << "  Best Bid: $" << ticks.toPrice(book.getBestBid()) 
              << " (tick=" << book.getBestBid() << ")\n";
    std::cout << "  Best Ask: $" << ticks.toPrice(book.getBestAsk()) 
              << " (tick=" << book.getBestAsk() << ")\n";
    std::cout << "  Spread:   $" << (ticks.toPrice(book.getBestAsk()) - ticks.toPrice(book.getBestBid())) << "\n";
    
    // ========================================================================
    // Step 2: Send aggressive order (crosses spread)
    // ========================================================================
    std::cout << "\n--- Aggressive Order (Cross Spread) ---\n";
    
    trades.clear();
    Order aggBuy(nextId++, nextId, 0,
                 OrderSide::Buy, OrderType::Limit,
                 ticks.toTicks(150.26), 150);  // Will take best ask + partial second
    
    std::cout << "  Sending: Buy 150 @ $150.26\n";
    
    auto start = Clock::now();
    matcher.match(book, aggBuy, trades);
    auto end = Clock::now();
    
    std::cout << "  Matched in " 
              << std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count() 
              << " ns\n";
    std::cout << "  Trades generated: " << trades.size() << "\n";
    
    for (const auto& t : trades) {
        std::cout << "    Trade: " << t.quantity << " shares @ $" 
                  << ticks.toPrice(t.price) << "\n";
    }
    
    std::cout << "\nBook after match:\n";
    std::cout << "  Best Bid: $" << ticks.toPrice(book.getBestBid()) << "\n";
    std::cout << "  Best Ask: $" << ticks.toPrice(book.getBestAsk()) << "\n";
    
    // ========================================================================
    // Step 3: IOC Order Demo
    // ========================================================================
    std::cout << "\n--- IOC Order Demo ---\n";
    
    trades.clear();
    Order ioc(nextId++, nextId, 0, 0,
              OrderSide::Buy, OrderType::Limit,
              TimeInForce::IOC, ticks.toTicks(150.30), 500);
    
    std::cout << "  Sending: IOC Buy 500 @ $150.30\n";
    matcher.match(book, ioc, trades);
    
    std::cout << "  Filled: " << (500 - ioc.quantity) << " shares\n";
    std::cout << "  Cancelled (unfilled): " << (ioc.isActive() ? ioc.quantity : 0) << " shares\n";
    std::cout << "  Trades: " << trades.size() << "\n";
    
    // ========================================================================
    // Step 4: Self-Trade Prevention Demo
    // ========================================================================
    std::cout << "\n--- Self-Trade Prevention Demo ---\n";
    
    book.reset();
    nextId = 0;
    
    // Add order from Client ID 100
    Order restingSell(nextId++, 1, 100, 0,  // clientId = 100
                      OrderSide::Sell, OrderType::Limit,
                      TimeInForce::GTC, ticks.toTicks(150.00), 100);
    book.addOrder(restingSell);
    std::cout << "  Added: Client 100 sell @ $150.00 x 100\n";
    
    // Try to buy from same client
    trades.clear();
    Order selfBuy(nextId++, 2, 100, 0,  // clientId = 100 (same!)
                  OrderSide::Buy, OrderType::Limit,
                  TimeInForce::GTC, ticks.toTicks(150.00), 100);
    
    std::cout << "  Sending: Client 100 buy @ $150.00 x 100 (same client)\n";
    matcher.match(book, selfBuy, trades);
    
    std::cout << "  Trades generated: " << trades.size() << " (STP prevented)\n";
    std::cout << "  Incoming order active: " << (selfBuy.isActive() ? "yes" : "NO (cancelled by STP)") << "\n";
    
    // ========================================================================
    // Step 5: Latency Summary
    // ========================================================================
    std::cout << "\n--- Quick Latency Check ---\n";
    
    book.reset();
    
    constexpr int NUM_OPS = 100000;
    
    start = Clock::now();
    for (int i = 0; i < NUM_OPS; ++i) {
        Order order(i, i, 0, OrderSide::Buy, OrderType::Limit, 5000 + (i % 1000), 100);
        book.addOrder(order);
    }
    end = Clock::now();
    
    double ns_per_op = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count() / static_cast<double>(NUM_OPS);
    double ops_per_sec = 1e9 / ns_per_op / 1e6;
    
    std::cout << "  " << NUM_OPS << " addOrder ops\n";
    std::cout << "  Avg latency: " << std::fixed << std::setprecision(1) << ns_per_op << " ns\n";
    std::cout << "  Throughput:  " << std::setprecision(2) << ops_per_sec << " M ops/sec\n";
}

// ============================================================================
// Main
// ============================================================================

int main() {
    std::cout << "========================================\n";
    std::cout << "   Single Stock Equity Matching Engine\n";
    std::cout << "========================================\n";
    std::cout << "Order size:    " << sizeof(Order) << " bytes\n";
    std::cout << "Max orders:    " << OrderBook::MAX_ORDER_ID << "\n";
    std::cout << "Max price:     " << OrderBook::MAX_PRICE << " ticks\n";
    std::cout << "POD verified:  " << (std::is_trivially_copyable_v<Order> ? "YES" : "NO") << "\n";
    
    demoEquityMatching();
    
    std::cout << "\n========================================\n";
    std::cout << "   Demo Complete\n";
    std::cout << "========================================\n";
    
    return 0;
}