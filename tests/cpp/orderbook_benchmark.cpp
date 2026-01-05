/**
 * @file orderbook_benchmark.cpp
 * @brief Throughput benchmark for order matching engine.
 * 
 * Measures:
 *   - Add order throughput (M ops/sec)
 *   - Cancel order throughput
 *   - Match order throughput
 *   - Mixed workload (80% add, 10% cancel, 10% match)
 */

#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <random>
#include <vector>

#include "../src/core/OrderBook.hpp"
#include "../src/core/MatchingStrategy.hpp"

// ============================================================================
// Configuration
// ============================================================================

constexpr size_t NUM_ORDERS = 5'000'000;       // 5M orders (within MAX_ORDER_ID)
constexpr size_t WARMUP_ORDERS = 100'000;
constexpr int PRICE_RANGE = 10'000;
constexpr int QTY_MIN = 1;
constexpr int QTY_MAX = 100;

using Clock = std::chrono::high_resolution_clock;

template <typename Duration>
double to_seconds(Duration d) {
    return std::chrono::duration<double>(d).count();
}

// ============================================================================
// Generate orders
// ============================================================================

std::vector<Order> generateOrders(size_t count, std::mt19937_64& rng) {
    std::vector<Order> orders;
    orders.reserve(count);
    
    std::uniform_int_distribution<Price> priceDist(1, PRICE_RANGE);
    std::uniform_int_distribution<Quantity> qtyDist(QTY_MIN, QTY_MAX);
    std::uniform_int_distribution<int> sideDist(0, 1);

    size_t maxId = std::min(count, static_cast<size_t>(OrderBook::MAX_ORDER_ID - 1));

    for (size_t i = 0; i < maxId; ++i) {
        orders.emplace_back(
            static_cast<OrderId>(i),
            static_cast<uint32_t>(i),
            0,
            sideDist(rng) ? OrderSide::Buy : OrderSide::Sell,
            OrderType::Limit,
            priceDist(rng),
            qtyDist(rng)
        );
    }

    return orders;
}

// ============================================================================
// Benchmarks
// ============================================================================

void benchmarkAddOrder() {
    std::cout << "\n=== Benchmark: Add Order ===\n";
    
    std::mt19937_64 rng(42);
    auto orders = generateOrders(NUM_ORDERS, rng);
    
    OrderBook book;
    
    // Warmup
    for (size_t i = 0; i < WARMUP_ORDERS && i < orders.size(); ++i) {
        book.addOrder(orders[i]);
    }
    book.reset();
    
    orders = generateOrders(NUM_ORDERS, rng);
    
    size_t added = 0;
    auto start = Clock::now();
    for (const auto& order : orders) {
        if (book.addOrder(order)) added++;
    }
    auto end = Clock::now();
    
    double elapsed = to_seconds(end - start);
    double opsPerSec = added / elapsed / 1'000'000.0;
    
    std::cout << "  Orders:     " << added << "\n";
    std::cout << "  Time:       " << std::fixed << std::setprecision(3) << elapsed << " s\n";
    std::cout << "  Throughput: " << std::setprecision(2) << opsPerSec << " M ops/sec\n";
}

void benchmarkCancelOrder() {
    std::cout << "\n=== Benchmark: Cancel Order ===\n";
    
    std::mt19937_64 rng(42);
    auto orders = generateOrders(NUM_ORDERS, rng);
    
    OrderBook book;
    
    for (const auto& order : orders) {
        book.addOrder(order);
    }
    
    std::vector<OrderId> cancelIds;
    cancelIds.reserve(orders.size());
    for (const auto& order : orders) {
        cancelIds.push_back(order.id);
    }
    std::shuffle(cancelIds.begin(), cancelIds.end(), rng);
    
    size_t cancelled = 0;
    auto start = Clock::now();
    for (OrderId id : cancelIds) {
        if (book.cancelOrder(id)) cancelled++;
    }
    auto end = Clock::now();
    
    double elapsed = to_seconds(end - start);
    double opsPerSec = cancelled / elapsed / 1'000'000.0;
    
    std::cout << "  Orders:     " << cancelled << "\n";
    std::cout << "  Time:       " << std::fixed << std::setprecision(3) << elapsed << " s\n";
    std::cout << "  Throughput: " << std::setprecision(2) << opsPerSec << " M ops/sec\n";
}

void benchmarkMatchOrder() {
    std::cout << "\n=== Benchmark: Match Order ===\n";
    
    std::mt19937_64 rng(42);
    
    OrderBook book;
    StandardMatchingStrategy strategy;
    std::vector<Trade> trades;
    trades.reserve(100);

    std::uniform_int_distribution<Price> priceDist(PRICE_RANGE / 4, PRICE_RANGE * 3 / 4);
    std::uniform_int_distribution<Quantity> qtyDist(QTY_MIN, QTY_MAX);

    OrderId nextId = 0;
    for (size_t i = 0; i < 100'000 && nextId < OrderBook::MAX_ORDER_ID - 2; ++i) {
        Order bid(nextId++, static_cast<uint32_t>(i), 0, 
                  OrderSide::Buy, OrderType::Limit, priceDist(rng), qtyDist(rng));
        book.addOrder(bid);
        
        Order ask(nextId++, static_cast<uint32_t>(i + 100'000), 0,
                  OrderSide::Sell, OrderType::Limit, priceDist(rng), qtyDist(rng));
        book.addOrder(ask);
    }

    size_t numAggressive = std::min(NUM_ORDERS, 
                                    static_cast<size_t>(OrderBook::MAX_ORDER_ID) - nextId - 1);
    std::vector<Order> aggressiveOrders;
    aggressiveOrders.reserve(numAggressive);
    
    for (size_t i = 0; i < numAggressive; ++i) {
        bool isBuy = (i % 2 == 0);
        Price price = isBuy ? PRICE_RANGE : 1;
        aggressiveOrders.emplace_back(
            nextId++, static_cast<uint32_t>(200'000 + i), 0,
            isBuy ? OrderSide::Buy : OrderSide::Sell,
            OrderType::Limit, price, qtyDist(rng)
        );
    }

    size_t totalTrades = 0;
    auto start = Clock::now();
    
    for (auto& order : aggressiveOrders) {
        trades.clear();
        strategy.match(book, order, trades);
        totalTrades += trades.size();
    }
    
    auto end = Clock::now();
    
    double elapsed = to_seconds(end - start);
    double opsPerSec = aggressiveOrders.size() / elapsed / 1'000'000.0;
    
    std::cout << "  Orders:     " << aggressiveOrders.size() << "\n";
    std::cout << "  Trades:     " << totalTrades << "\n";
    std::cout << "  Time:       " << std::fixed << std::setprecision(3) << elapsed << " s\n";
    std::cout << "  Throughput: " << std::setprecision(2) << opsPerSec << " M ops/sec\n";
}

void benchmarkMixed() {
    std::cout << "\n=== Benchmark: Mixed Workload (80/10/10) ===\n";
    
    std::mt19937_64 rng(42);
    
    OrderBook book;
    StandardMatchingStrategy strategy;
    std::vector<Trade> trades;
    trades.reserve(100);

    std::uniform_int_distribution<Price> priceDist(1, PRICE_RANGE);
    std::uniform_int_distribution<Quantity> qtyDist(QTY_MIN, QTY_MAX);
    std::uniform_int_distribution<int> sideDist(0, 1);
    std::uniform_int_distribution<int> opDist(0, 99);

    OrderId nextId = 0;
    std::vector<OrderId> activeIds;
    activeIds.reserve(1'000'000);

    size_t addCount = 0, cancelCount = 0, matchCount = 0;
    size_t maxOps = std::min(NUM_ORDERS, static_cast<size_t>(OrderBook::MAX_ORDER_ID - 1));

    auto start = Clock::now();
    
    for (size_t i = 0; i < maxOps && nextId < OrderBook::MAX_ORDER_ID - 1; ++i) {
        int op = opDist(rng);
        
        if (op < 80) {
            Order order(nextId, static_cast<uint32_t>(nextId), 0,
                       sideDist(rng) ? OrderSide::Buy : OrderSide::Sell,
                       OrderType::Limit, priceDist(rng), qtyDist(rng));
            if (book.addOrder(order)) {
                activeIds.push_back(nextId);
                addCount++;
            }
            nextId++;
        } else if (op < 90 && !activeIds.empty()) {
            std::uniform_int_distribution<size_t> idxDist(0, activeIds.size() - 1);
            size_t idx = idxDist(rng);
            if (book.cancelOrder(activeIds[idx])) {
                cancelCount++;
            }
            activeIds[idx] = activeIds.back();
            activeIds.pop_back();
        } else {
            trades.clear();
            bool isBuy = sideDist(rng);
            Price price = isBuy ? PRICE_RANGE : 1;
            Order order(nextId, static_cast<uint32_t>(nextId), 0,
                       isBuy ? OrderSide::Buy : OrderSide::Sell,
                       OrderType::Limit, price, qtyDist(rng));
            strategy.match(book, order, trades);
            nextId++;
            matchCount++;
        }
    }
    
    auto end = Clock::now();
    
    double elapsed = to_seconds(end - start);
    size_t totalOps = addCount + cancelCount + matchCount;
    double opsPerSec = totalOps / elapsed / 1'000'000.0;
    
    std::cout << "  Total Ops:  " << totalOps << "\n";
    std::cout << "  Adds:       " << addCount << "\n";
    std::cout << "  Cancels:    " << cancelCount << "\n";
    std::cout << "  Matches:    " << matchCount << "\n";
    std::cout << "  Time:       " << std::fixed << std::setprecision(3) << elapsed << " s\n";
    std::cout << "  Throughput: " << std::setprecision(2) << opsPerSec << " M ops/sec\n";
}

// ============================================================================
// Main
// ============================================================================

int main() {
    std::cout << "============================================\n";
    std::cout << "   Order Matching Engine Benchmark\n";
    std::cout << "============================================\n";
    std::cout << "Order size:   " << sizeof(Order) << " bytes\n";
    std::cout << "Max orders:   " << OrderBook::MAX_ORDER_ID << "\n";

    benchmarkAddOrder();
    benchmarkCancelOrder();
    benchmarkMatchOrder();
    benchmarkMixed();

    std::cout << "\n============================================\n";
    std::cout << "   Benchmark Complete\n";
    std::cout << "============================================\n";

    return 0;
}
