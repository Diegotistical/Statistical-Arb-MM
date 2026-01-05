/**
 * @file main.cpp
 * @brief Statistical Arbitrage Market Making Simulator Demo.
 * 
 * Demonstrates:
 *   - Multi-asset order book
 *   - Spread z-score computation
 *   - OFI (Order Flow Imbalance)
 *   - Avellaneda-Stoikov market making
 *   - Backtest simulation
 */

#include <chrono>
#include <iomanip>
#include <iostream>
#include <random>
#include <vector>

#include "core/OrderBook.hpp"
#include "core/MatchingStrategy.hpp"
#include "core/TickNormalizer.hpp"
#include "core/Timestamp.hpp"
#include "signals/SpreadModel.hpp"
#include "signals/OFI.hpp"
#include "strategy/StatArbMM.hpp"
#include "execution/ExecutionSimulator.hpp"

using Clock = std::chrono::high_resolution_clock;

// ============================================================================
// Demo: Stat-Arb Market Making
// ============================================================================

void demoStatArbMM() {
    std::cout << "\n=== Statistical Arbitrage Market Making Demo ===\n\n";
    
    // Setup
    TickNormalizer ticks(0.01);  // $0.01 tick
    signals::SpreadModel spreadModel(100, 1.0);  // 100-tick lookback
    signals::OFI ofi(100);
    strategy::StatArbMM mm;
    
    std::mt19937_64 rng(42);
    std::normal_distribution<> priceDist(0, 0.02);
    std::normal_distribution<> spreadDist(0, 0.01);
    
    // Simulate two cointegrated assets
    double priceA = 150.0;
    double priceB = 150.0;
    double spreadState = 0;
    
    std::cout << "Simulating 1000 ticks of cointegrated pair trading...\n\n";
    
    // Metrics
    int trades = 0;
    double totalZ = 0;
    
    for (int t = 0; t < 1000; ++t) {
        // Evolve prices (cointegrated with mean-reverting spread)
        priceA *= std::exp(priceDist(rng));
        spreadState = 0.9 * spreadState + spreadDist(rng);  // OU process
        priceB = priceA * std::exp(spreadState);
        
        // Update signals
        double z = spreadModel.update(priceA, priceB);
        totalZ += std::abs(z);
        
        // Simulate book sizes
        double bidSize = 1000 + 500 * spreadState;
        double askSize = 1000 - 500 * spreadState;
        ofi.update(bidSize, askSize);
        
        // Generate quotes
        double fairPrice = ticks.toTicks((priceA + priceB) / 2.0);
        double vol = spreadModel.stdDev() * 100;  // Scale for ticks
        
        strategy::Quote quote = mm.computeQuotes(fairPrice, vol, z, ofi.normalized());
        
        // Log every 200 ticks
        if (t % 200 == 0) {
            std::cout << "Tick " << std::setw(4) << t 
                      << " | A=$" << std::fixed << std::setprecision(2) << priceA
                      << " B=$" << priceB
                      << " | z=" << std::setprecision(3) << std::setw(6) << z
                      << " | OFI=" << std::setw(6) << ofi.normalized()
                      << " | Inv=" << std::setw(4) << mm.inventory()
                      << "\n";
        }
        
        // Simulate random fills
        if (quote.valid && (rng() % 100 < 10)) {
            OrderSide side = (rng() % 2 == 0) ? OrderSide::Buy : OrderSide::Sell;
            mm.onFill(side, 100);
            trades++;
        }
    }
    
    std::cout << "\n--- Summary ---\n"
              << "  Ticks processed: 1000\n"
              << "  Trades:          " << trades << "\n"
              << "  Final inventory: " << mm.inventory() << "\n"
              << "  Avg |z-score|:   " << std::setprecision(3) << totalZ / 1000 << "\n"
              << "  Spread mean:     " << std::setprecision(6) << spreadModel.mean() << "\n"
              << "  Spread std:      " << spreadModel.stdDev() << "\n";
}

// ============================================================================
// Demo: Execution Simulation
// ============================================================================

void demoExecutionSim() {
    std::cout << "\n=== Execution Simulation Demo ===\n\n";
    
    OrderBook book;
    execution::ExecutionSimulator executor(5000, 1000);  // 5µs + 1µs jitter
    TickNormalizer ticks(0.01);
    
    // Build initial book
    for (int i = 0; i < 10; ++i) {
        Order bid(i, i, 0, OrderSide::Buy, OrderType::Limit,
                  ticks.toTicks(150.00 - i * 0.01), 100);
        Order ask(i + 10, i + 10, 0, OrderSide::Sell, OrderType::Limit,
                  ticks.toTicks(150.10 + i * 0.01), 100);
        (void)book.addOrder(bid);
        (void)book.addOrder(ask);
    }
    
    std::cout << "Initial book:\n"
              << "  Best Bid: $" << ticks.toPrice(book.getBestBid()) << "\n"
              << "  Best Ask: $" << ticks.toPrice(book.getBestAsk()) << "\n\n";
    
    // Submit aggressive order using Timestamp
    core::Timestamp t0(0);
    Order aggressor(100, 100, 0, OrderSide::Buy, OrderType::Limit,
                    ticks.toTicks(150.10), 50);  // Marketable
    
    std::cout << "Submitting marketable buy order...\n";
    executor.submit(aggressor, t0);
    
    // Process after latency
    core::Timestamp t1(10000);  // 10µs later
    auto fills = executor.process(book, t1);
    
    std::cout << "Fills after " << (t1 - t0) / 1000 << "µs:\n";
    for (const auto& fill : fills) {
        std::cout << "  Order " << fill.orderId
                  << " filled " << fill.fillQty
                  << " @ $" << ticks.toPrice(fill.fillPrice)
                  << (fill.complete ? " (complete)" : " (partial)")
                  << (fill.isMaker ? " [maker]" : " [taker]")
                  << " AS=" << fill.adverseSelectionCost << "\n";
    }
}

// ============================================================================
// Performance Benchmark
// ============================================================================

void runBenchmark() {
    std::cout << "\n=== Performance Benchmark ===\n\n";
    
    constexpr int NUM_OPS = 1'000'000;
    
    signals::SpreadModel spreadModel(100);
    signals::OFI ofi(100);
    
    std::mt19937_64 rng(42);
    std::normal_distribution<> dist(100, 1);
    
    // Benchmark SpreadModel
    auto start = Clock::now();
    for (int i = 0; i < NUM_OPS; ++i) {
        spreadModel.update(dist(rng), dist(rng));
    }
    auto end = Clock::now();
    
    double ns_spread = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count() / static_cast<double>(NUM_OPS);
    
    // Benchmark OFI
    start = Clock::now();
    for (int i = 0; i < NUM_OPS; ++i) {
        ofi.update(dist(rng), dist(rng));
    }
    end = Clock::now();
    
    double ns_ofi = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count() / static_cast<double>(NUM_OPS);
    
    std::cout << "SpreadModel.update:  " << std::fixed << std::setprecision(1) << ns_spread << " ns/op\n"
              << "OFI.update:          " << ns_ofi << " ns/op\n"
              << "Operations:          " << NUM_OPS / 1'000'000 << "M\n";
}

// ============================================================================
// Main
// ============================================================================

int main() {
    std::cout << "========================================\n";
    std::cout << "  Stat-Arb Market Making Simulator\n";
    std::cout << "========================================\n";
    std::cout << "Order size:    " << sizeof(Order) << " bytes\n";
    std::cout << "Max orders:    " << OrderBook::MAX_ORDER_ID << "\n";
    
    demoStatArbMM();
    demoExecutionSim();
    runBenchmark();
    
    std::cout << "\n========================================\n";
    std::cout << "  Demo Complete\n";
    std::cout << "========================================\n";
    
    return 0;
}