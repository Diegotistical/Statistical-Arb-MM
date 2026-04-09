/**
 * @file strategy_benchmark.cpp
 * @brief Google Benchmark tests for strategy hot-path latency.
 *
 * Benchmarks:
 *   - computeQuotes() latency
 *   - KalmanFilter::update() latency
 *   - RiskManager::preTradeCheck() latency
 *   - Full tick-to-quote pipeline latency
 *
 * Target: all hot-path operations under 1 microsecond.
 */

#include <benchmark/benchmark.h>
#include <cmath>
#include <random>

#include "../../src/strategy/StatArbMM.hpp"
#include "../../src/signals/KalmanFilter.hpp"
#include "../../src/signals/SpreadModel.hpp"
#include "../../src/signals/OFI.hpp"
#include "../../src/signals/VPIN.hpp"
#include "../../src/signals/KyleLambda.hpp"
#include "../../src/risk/RiskManager.hpp"

// ============================================================================
// StatArbMM::computeQuotes()
// ============================================================================

static void BM_ComputeQuotes(benchmark::State& state) {
    strategy::StatArbMM mm;
    mm.gamma = 0.01;
    mm.k = 1.5;
    mm.setInventory(50);
    mm.setSessionTimes(0, 1000000000LL);
    mm.updateTime(500000000LL);

    for (auto _ : state) {
        auto quote = mm.computeQuotes(15000, 0.02, 1.2, 0.3, 0.4, 0.1);
        benchmark::DoNotOptimize(quote);
    }
}
BENCHMARK(BM_ComputeQuotes);

// ============================================================================
// ComputeQuotes with RiskManager
// ============================================================================

static void BM_ComputeQuotesWithRisk(benchmark::State& state) {
    risk::RiskManager rm;
    strategy::StatArbMM mm;
    mm.gamma = 0.01;
    mm.k = 1.5;
    mm.setRiskManager(&rm);
    mm.setInventory(50);

    for (auto _ : state) {
        auto quote = mm.computeQuotes(15000, 0.02, 1.2, 0.3, 0.4, 0.1);
        benchmark::DoNotOptimize(quote);
    }
}
BENCHMARK(BM_ComputeQuotesWithRisk);

// ============================================================================
// KalmanFilter::update()
// ============================================================================

static void BM_KalmanUpdate(benchmark::State& state) {
    signals::KalmanHedgeRatio kf(0.9999, 1e-3);

    std::mt19937 rng(42);
    std::normal_distribution<double> noise(0, 0.001);

    double logA = 5.0, logB = 4.0;

    for (auto _ : state) {
        logA += noise(rng);
        logB += noise(rng);
        auto result = kf.update(logA, logB);
        benchmark::DoNotOptimize(result);
    }
}
BENCHMARK(BM_KalmanUpdate);

// ============================================================================
// RiskManager::preTradeCheck()
// ============================================================================

static void BM_PreTradeCheck(benchmark::State& state) {
    risk::RiskManager rm;
    rm.onFill(0, OrderSide::Buy, 100, 150.0);

    for (auto _ : state) {
        auto result = rm.preTradeCheck(0, OrderSide::Buy, 100, 15000);
        benchmark::DoNotOptimize(result);
    }
}
BENCHMARK(BM_PreTradeCheck);

// ============================================================================
// SpreadModel::update()
// ============================================================================

static void BM_SpreadModelUpdate(benchmark::State& state) {
    signals::SpreadModel model(100, 1.0);

    double priceA = 150.0, priceB = 100.0;
    std::mt19937 rng(42);
    std::normal_distribution<double> noise(0, 0.01);

    for (auto _ : state) {
        priceA += noise(rng);
        priceB += noise(rng);
        double z = model.update(priceA, priceB);
        benchmark::DoNotOptimize(z);
    }
}
BENCHMARK(BM_SpreadModelUpdate);

// ============================================================================
// SpreadModel with Kalman
// ============================================================================

static void BM_SpreadModelKalman(benchmark::State& state) {
    signals::KalmanHedgeRatio kf(0.9999, 1e-3);
    signals::SpreadModel model(100);
    model.setKalmanFilter(&kf);

    double priceA = 150.0, priceB = 100.0;
    std::mt19937 rng(42);
    std::normal_distribution<double> noise(0, 0.01);

    for (auto _ : state) {
        priceA += noise(rng);
        priceB += noise(rng);
        double z = model.update(priceA, priceB);
        benchmark::DoNotOptimize(z);
    }
}
BENCHMARK(BM_SpreadModelKalman);

// ============================================================================
// OFI::update()
// ============================================================================

static void BM_OFIUpdate(benchmark::State& state) {
    signals::OFI ofi(100);

    double bidSize = 1000, askSize = 1000;
    std::mt19937 rng(42);
    std::normal_distribution<double> noise(0, 50);

    for (auto _ : state) {
        bidSize += noise(rng);
        askSize += noise(rng);
        double val = ofi.update(bidSize, askSize);
        benchmark::DoNotOptimize(val);
    }
}
BENCHMARK(BM_OFIUpdate);

// ============================================================================
// VPIN::onTrade()
// ============================================================================

static void BM_VPINUpdate(benchmark::State& state) {
    signals::VPIN vpin(1000, 50);

    double price = 150.0;
    std::mt19937 rng(42);
    std::normal_distribution<double> noise(0, 0.01);

    for (auto _ : state) {
        price += noise(rng);
        vpin.onTrade(price, 100);
        benchmark::DoNotOptimize(vpin.value());
    }
}
BENCHMARK(BM_VPINUpdate);

// ============================================================================
// Kyle Lambda update
// ============================================================================

static void BM_KyleLambdaUpdate(benchmark::State& state) {
    signals::KyleLambda kyle(200);

    std::mt19937 rng(42);
    std::normal_distribution<double> noise(0, 0.01);

    for (auto _ : state) {
        kyle.update(noise(rng), noise(rng) * 100);
        benchmark::DoNotOptimize(kyle.lambda());
    }
}
BENCHMARK(BM_KyleLambdaUpdate);

// ============================================================================
// Full tick-to-quote pipeline
// ============================================================================

static void BM_FullTickToQuote(benchmark::State& state) {
    signals::KalmanHedgeRatio kf(0.9999, 1e-3);
    signals::SpreadModel spread(100);
    spread.setKalmanFilter(&kf);
    signals::OFI ofi(100);
    signals::VPIN vpin(1000, 50);
    signals::KyleLambda kyle(200);
    risk::RiskManager rm;
    strategy::StatArbMM mm;
    mm.gamma = 0.01;
    mm.k = 1.5;
    mm.setRiskManager(&rm);
    mm.setSessionTimes(0, 1000000000LL);

    double priceA = 150.0, priceB = 100.0;
    double bidSize = 1000, askSize = 1000;
    std::mt19937 rng(42);
    std::normal_distribution<double> noise(0, 0.01);
    int64_t ts = 0;

    for (auto _ : state) {
        priceA += noise(rng);
        priceB += noise(rng);
        bidSize += noise(rng) * 100;
        askSize += noise(rng) * 100;
        ts += 1000000;

        // Signal updates
        double z = spread.update(priceA, priceB);
        double ofiVal = ofi.update(bidSize, askSize);
        double ofiNorm = ofi.normalized();
        vpin.onTrade(priceA, 100);
        kyle.update(priceA - priceB, ofiVal);

        // Strategy
        mm.updateTime(ts);
        auto kyleResult = kyle.estimate();
        auto quote = mm.computeQuotes(
            priceA * 100, 0.02, z, ofiNorm,
            vpin.value(), kyleResult.lambda);

        benchmark::DoNotOptimize(quote);
    }
}
BENCHMARK(BM_FullTickToQuote);
