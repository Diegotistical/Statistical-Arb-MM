/**
 * @file latency_tests.cpp
 * @brief Latency verification tests using high-resolution timing.
 * 
 * Measures sub-microsecond latency for:
 *   - addOrder
 *   - cancelOrder
 *   - match (single trade)
 * 
 * Uses std::chrono::high_resolution_clock (rdtsc on x86).
 */

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <random>
#include <vector>

#include "../src/core/OrderBook.hpp"
#include "../src/core/MatchingStrategy.hpp"

using Clock = std::chrono::high_resolution_clock;
using Duration = std::chrono::nanoseconds;

// ============================================================================
// Timing utilities
// ============================================================================

struct LatencyStats {
    double min_ns;
    double max_ns;
    double avg_ns;
    double p50_ns;
    double p99_ns;
    double p999_ns;
};

LatencyStats computeStats(std::vector<int64_t>& samples) {
    std::sort(samples.begin(), samples.end());
    
    size_t n = samples.size();
    double sum = 0;
    for (auto s : samples) sum += s;
    
    return {
        static_cast<double>(samples.front()),
        static_cast<double>(samples.back()),
        sum / n,
        static_cast<double>(samples[n / 2]),
        static_cast<double>(samples[static_cast<size_t>(n * 0.99)]),
        static_cast<double>(samples[static_cast<size_t>(n * 0.999)])
    };
}

void printStats(const std::string& name, const LatencyStats& stats) {
    std::cout << "  " << name << ":\n"
              << "    Min:  " << std::fixed << std::setprecision(1) << stats.min_ns << " ns\n"
              << "    Avg:  " << stats.avg_ns << " ns\n"
              << "    p50:  " << stats.p50_ns << " ns\n"
              << "    p99:  " << stats.p99_ns << " ns\n"
              << "    p999: " << stats.p999_ns << " ns\n"
              << "    Max:  " << stats.max_ns << " ns\n\n";
}

// ============================================================================
// Latency tests
// ============================================================================

constexpr size_t NUM_SAMPLES = 100'000;
constexpr size_t WARMUP = 10'000;

void testAddOrderLatency() {
    std::cout << "=== Add Order Latency ===\n";
    
    std::vector<int64_t> samples;
    samples.reserve(NUM_SAMPLES);
    
    std::mt19937_64 rng(42);
    std::uniform_int_distribution<Price> priceDist(1, 10000);
    std::uniform_int_distribution<Quantity> qtyDist(1, 100);
    std::uniform_int_distribution<int> sideDist(0, 1);
    
    OrderBook book;
    OrderId nextId = 0;
    
    // Warmup
    for (size_t i = 0; i < WARMUP && nextId < OrderBook::MAX_ORDER_ID; ++i) {
        Order order(nextId++, i, 0, 
                    sideDist(rng) ? OrderSide::Buy : OrderSide::Sell,
                    OrderType::Limit, priceDist(rng), qtyDist(rng));
        book.addOrder(order);
    }
    
    book.reset();
    nextId = 0;
    
    // Measure
    for (size_t i = 0; i < NUM_SAMPLES && nextId < OrderBook::MAX_ORDER_ID; ++i) {
        Order order(nextId++, i, 0,
                    sideDist(rng) ? OrderSide::Buy : OrderSide::Sell,
                    OrderType::Limit, priceDist(rng), qtyDist(rng));
        
        auto start = Clock::now();
        book.addOrder(order);
        auto end = Clock::now();
        
        samples.push_back(std::chrono::duration_cast<Duration>(end - start).count());
    }
    
    auto stats = computeStats(samples);
    printStats("addOrder", stats);
    
    // Verify sub-microsecond average
    if (stats.avg_ns < 1000) {
        std::cout << "  ✓ Sub-microsecond avg latency verified\n\n";
    } else {
        std::cout << "  ✗ Average latency exceeds 1µs\n\n";
    }
}

void testCancelOrderLatency() {
    std::cout << "=== Cancel Order Latency ===\n";
    
    std::vector<int64_t> samples;
    samples.reserve(NUM_SAMPLES);
    
    std::mt19937_64 rng(42);
    std::uniform_int_distribution<Price> priceDist(1, 10000);
    std::uniform_int_distribution<Quantity> qtyDist(1, 100);
    std::uniform_int_distribution<int> sideDist(0, 1);
    
    OrderBook book;
    std::vector<OrderId> orderIds;
    
    // Add orders
    for (OrderId id = 0; id < NUM_SAMPLES && id < OrderBook::MAX_ORDER_ID; ++id) {
        Order order(id, id, 0,
                    sideDist(rng) ? OrderSide::Buy : OrderSide::Sell,
                    OrderType::Limit, priceDist(rng), qtyDist(rng));
        book.addOrder(order);
        orderIds.push_back(id);
    }
    
    // Shuffle for random cancel pattern
    std::shuffle(orderIds.begin(), orderIds.end(), rng);
    
    // Measure
    for (size_t i = 0; i < NUM_SAMPLES && i < orderIds.size(); ++i) {
        auto start = Clock::now();
        book.cancelOrder(orderIds[i]);
        auto end = Clock::now();
        
        samples.push_back(std::chrono::duration_cast<Duration>(end - start).count());
    }
    
    auto stats = computeStats(samples);
    printStats("cancelOrder", stats);
    
    if (stats.avg_ns < 1000) {
        std::cout << "  ✓ Sub-microsecond avg latency verified\n\n";
    } else {
        std::cout << "  ✗ Average latency exceeds 1µs\n\n";
    }
}

void testMatchLatency() {
    std::cout << "=== Match Order Latency ===\n";
    
    std::vector<int64_t> samples;
    samples.reserve(NUM_SAMPLES);
    
    std::mt19937_64 rng(42);
    std::uniform_int_distribution<Price> priceDist(4000, 6000);
    std::uniform_int_distribution<Quantity> qtyDist(1, 100);
    
    OrderBook book;
    StandardMatchingStrategy strategy;
    std::vector<Trade> trades;
    trades.reserve(10);
    
    OrderId nextId = 0;
    
    // Pre-populate with liquidity
    for (size_t i = 0; i < 10000 && nextId < OrderBook::MAX_ORDER_ID; ++i) {
        Order bid(nextId++, i, 0, OrderSide::Buy, OrderType::Limit, 
                  priceDist(rng), qtyDist(rng));
        book.addOrder(bid);
        
        Order ask(nextId++, i, 0, OrderSide::Sell, OrderType::Limit,
                  priceDist(rng), qtyDist(rng));
        book.addOrder(ask);
    }
    
    // Measure matching latency
    for (size_t i = 0; i < NUM_SAMPLES && nextId < OrderBook::MAX_ORDER_ID; ++i) {
        trades.clear();
        
        bool isBuy = (i % 2 == 0);
        Price price = isBuy ? 10000 : 1;  // Aggressive price
        Order aggressive(nextId++, i, 0,
                        isBuy ? OrderSide::Buy : OrderSide::Sell,
                        OrderType::Limit, price, 1);  // Small qty for fast match
        
        auto start = Clock::now();
        strategy.match(book, aggressive, trades);
        auto end = Clock::now();
        
        samples.push_back(std::chrono::duration_cast<Duration>(end - start).count());
    }
    
    auto stats = computeStats(samples);
    printStats("match (single trade)", stats);
    
    if (stats.avg_ns < 2000) {
        std::cout << "  ✓ Sub-2µs avg match latency verified\n\n";
    } else {
        std::cout << "  ✗ Average match latency exceeds 2µs\n\n";
    }
}

void testThroughput() {
    std::cout << "=== Throughput Test ===\n";
    
    constexpr size_t NUM_OPS = 1'000'000;
    
    std::mt19937_64 rng(42);
    std::uniform_int_distribution<Price> priceDist(1, 10000);
    std::uniform_int_distribution<Quantity> qtyDist(1, 100);
    std::uniform_int_distribution<int> sideDist(0, 1);
    
    OrderBook book;
    OrderId nextId = 0;
    
    auto start = Clock::now();
    
    for (size_t i = 0; i < NUM_OPS && nextId < OrderBook::MAX_ORDER_ID; ++i) {
        Order order(nextId++, i, 0,
                    sideDist(rng) ? OrderSide::Buy : OrderSide::Sell,
                    OrderType::Limit, priceDist(rng), qtyDist(rng));
        book.addOrder(order);
    }
    
    auto end = Clock::now();
    
    double elapsedSec = std::chrono::duration<double>(end - start).count();
    double opsPerSec = nextId / elapsedSec / 1'000'000.0;
    
    std::cout << "  Operations: " << nextId << "\n"
              << "  Time:       " << std::fixed << std::setprecision(3) << elapsedSec << " s\n"
              << "  Throughput: " << std::setprecision(2) << opsPerSec << " M ops/sec\n\n";
    
    if (opsPerSec > 10.0) {
        std::cout << "  ✓ >10M ops/sec verified\n\n";
    }
}

// ============================================================================
// Main
// ============================================================================

int main() {
    std::cout << "============================================\n";
    std::cout << "   Latency Verification Tests\n";
    std::cout << "============================================\n";
    std::cout << "Samples per test: " << NUM_SAMPLES << "\n\n";

    testAddOrderLatency();
    testCancelOrderLatency();
    testMatchLatency();
    testThroughput();

    std::cout << "============================================\n";
    std::cout << "   Latency Tests Complete\n";
    std::cout << "============================================\n";

    return 0;
}
