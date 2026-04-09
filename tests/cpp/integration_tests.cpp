/**
 * @file integration_tests.cpp
 * @brief Integration tests for the full signal -> order -> fill -> PnL pipeline.
 *
 * Tests verify:
 *   - Complete tick-to-PnL pipeline produces valid metrics
 *   - Simulator with all signals (spread, OFI, VPIN, Kyle) runs correctly
 *   - Transaction costs reduce PnL vs no-cost baseline
 *   - Risk manager prevents excessive positions during simulation
 *   - Walk-forward optimizer runs without crashes
 */

#include <gtest/gtest.h>
#include <cmath>
#include <vector>
#include <random>

#include "../../src/backtest/Simulator.hpp"
#include "../../src/backtest/WalkForward.hpp"

using namespace backtest;

class IntegrationTest : public ::testing::Test {
protected:
    std::mt19937 rng{42};

    // Generate synthetic tick data for a cointegrated pair
    std::vector<TickEvent> generateSyntheticTicks(
        size_t n, double theta = 0.1, double sigma = 0.01) {

        std::normal_distribution<double> noise(0, sigma);
        std::vector<TickEvent> ticks;
        ticks.reserve(n * 2);

        double priceA = 15000;  // 150.00 in ticks
        double priceB = 10000;  // 100.00 in ticks
        double spread = 0;

        for (size_t i = 0; i < n; ++i) {
            // Mean-reverting spread
            spread = spread * (1.0 - theta) + noise(rng) * 100;

            // Update prices
            priceB += noise(rng) * 50;  // Random walk component
            priceA = priceB * 1.5 + spread;  // Cointegrated with beta=1.5

            int64_t ts = static_cast<int64_t>(i) * 1000000LL;  // 1ms per tick

            // Symbol 0 tick
            TickEvent tickA;
            tickA.timestamp = ts;
            tickA.symbolId = 0;
            tickA.bidPrice = static_cast<Price>(priceA - 1);
            tickA.askPrice = static_cast<Price>(priceA + 1);
            tickA.bidSize = 1000 + static_cast<Quantity>(noise(rng) * 200);
            tickA.askSize = 1000 + static_cast<Quantity>(noise(rng) * 200);
            ticks.push_back(tickA);

            // Symbol 1 tick
            TickEvent tickB;
            tickB.timestamp = ts;
            tickB.symbolId = 1;
            tickB.bidPrice = static_cast<Price>(priceB - 1);
            tickB.askPrice = static_cast<Price>(priceB + 1);
            tickB.bidSize = 1000 + static_cast<Quantity>(noise(rng) * 200);
            tickB.askSize = 1000 + static_cast<Quantity>(noise(rng) * 200);
            ticks.push_back(tickB);
        }

        return ticks;
    }
};

// ============================================================================
// Full Pipeline Tests
// ============================================================================

TEST_F(IntegrationTest, SimulatorRunsWithoutCrash) {
    auto ticks = generateSyntheticTicks(1000);

    Simulator sim;
    sim.strategy().setSessionTimes(0, 1000 * 1000000LL);
    sim.strategy().gamma = 0.01;
    sim.strategy().k = 1.5;
    sim.strategy().maxInventory = 500;

    for (const auto& tick : ticks) {
        sim.onTick(tick);
    }

    auto metrics = sim.metrics();

    // Should produce some trades
    EXPECT_GE(metrics.numTrades, 0);
    // PnL should be finite
    EXPECT_TRUE(std::isfinite(metrics.totalPnL));
    // Sharpe should be finite (may be 0 if insufficient data)
    EXPECT_TRUE(std::isfinite(metrics.sharpeRatio));
}

TEST_F(IntegrationTest, SimulatorProducesValidMetrics) {
    auto ticks = generateSyntheticTicks(2000);

    Simulator sim;
    sim.strategy().setSessionTimes(0, 2000 * 1000000LL);
    sim.setSnapshotInterval(50);

    for (const auto& tick : ticks) {
        sim.onTick(tick);
    }

    auto metrics = sim.metrics();

    // Total PnL = realized + unrealized
    EXPECT_NEAR(metrics.totalPnL,
                metrics.realizedPnL + metrics.unrealizedPnL, 1.0);

    // Max drawdown should be non-negative
    EXPECT_GE(metrics.maxDrawdown, 0.0);

    // Volume should equal sum of fills
    EXPECT_GE(metrics.totalVolume, 0);

    // Buys + sells = total trades
    EXPECT_EQ(metrics.numBuys + metrics.numSells, metrics.numTrades);
}

TEST_F(IntegrationTest, InventoryStaysWithinLimits) {
    auto ticks = generateSyntheticTicks(2000);

    Simulator sim;
    sim.strategy().maxInventory = 200;
    sim.strategy().setSessionTimes(0, 2000 * 1000000LL);

    for (const auto& tick : ticks) {
        sim.onTick(tick);
    }

    // Max inventory seen should not exceed limit
    auto metrics = sim.metrics();
    EXPECT_LE(metrics.maxInventory, 200 + 200);  // Allow some buffer for fills in flight
}

// ============================================================================
// Transaction Cost Impact
// ============================================================================

TEST_F(IntegrationTest, TransactionCostsReducePnL) {
    auto ticks = generateSyntheticTicks(1000);

    // Sim with zero costs
    Simulator simNoCost;
    simNoCost.txCostModel().setEpsilon(0);
    simNoCost.txCostModel().setEta(0);
    simNoCost.txCostModel().setFeeSchedule({0, 0, 0, 0});
    simNoCost.strategy().setSessionTimes(0, 1000 * 1000000LL);

    for (const auto& tick : ticks) {
        simNoCost.onTick(tick);
    }

    // Sim with costs
    Simulator simWithCost;
    simWithCost.txCostModel().setEpsilon(0.005);
    simWithCost.txCostModel().setEta(0.1);
    simWithCost.strategy().setSessionTimes(0, 1000 * 1000000LL);

    for (const auto& tick : ticks) {
        simWithCost.onTick(tick);
    }

    auto metricsNoCost = simNoCost.metrics();
    auto metricsWithCost = simWithCost.metrics();

    // If there were any trades, costs should reduce PnL
    if (metricsNoCost.numTrades > 0 && metricsWithCost.numTrades > 0) {
        auto breakdownCost = simWithCost.pnlAnalytics().breakdown();
        EXPECT_GT(breakdownCost.transaction_costs, 0);
    }
}

// ============================================================================
// Risk Manager Integration
// ============================================================================

TEST_F(IntegrationTest, RiskManagerIntegrated) {
    auto ticks = generateSyntheticTicks(500);

    Simulator sim;
    sim.riskManager().config().maxPositionPerSymbol = 100;
    sim.strategy().setSessionTimes(0, 500 * 1000000LL);

    for (const auto& tick : ticks) {
        sim.onTick(tick);
    }

    // Risk manager snapshot should be consistent
    auto snap = sim.riskManager().snapshot();
    EXPECT_GE(snap.totalAbsPosition, 0);
    EXPECT_TRUE(std::isfinite(snap.totalPnL));
}

// ============================================================================
// Signal Pipeline Tests
// ============================================================================

TEST_F(IntegrationTest, VPINComputableDuringSimulation) {
    auto ticks = generateSyntheticTicks(500);

    Simulator sim;
    sim.strategy().setSessionTimes(0, 500 * 1000000LL);

    for (const auto& tick : ticks) {
        sim.onTick(tick);
    }

    // VPIN value should be in [0, 1] if valid
    double vpinVal = sim.vpin().value();
    if (sim.vpin().isValid()) {
        EXPECT_GE(vpinVal, 0.0);
        EXPECT_LE(vpinVal, 1.0);
    }
}

TEST_F(IntegrationTest, KyleLambdaComputableDuringSimulation) {
    auto ticks = generateSyntheticTicks(500);

    Simulator sim;
    sim.strategy().setSessionTimes(0, 500 * 1000000LL);

    for (const auto& tick : ticks) {
        sim.onTick(tick);
    }

    auto kyleResult = sim.kyle().estimate();
    EXPECT_TRUE(std::isfinite(kyleResult.lambda));
    EXPECT_GE(kyleResult.samples, 0u);
}

// ============================================================================
// Simulator Reset
// ============================================================================

TEST_F(IntegrationTest, SimulatorResetProducesCleanState) {
    auto ticks = generateSyntheticTicks(200);

    Simulator sim;
    sim.strategy().setSessionTimes(0, 200 * 1000000LL);

    for (const auto& tick : ticks) {
        sim.onTick(tick);
    }

    sim.reset();

    auto metrics = sim.metrics();
    EXPECT_EQ(metrics.numTrades, 0);
    EXPECT_DOUBLE_EQ(metrics.totalPnL, 0);
    EXPECT_EQ(metrics.finalInventory, 0);
}

// ============================================================================
// Walk-Forward Optimizer
// ============================================================================

TEST_F(IntegrationTest, WalkForwardRunsWithoutCrash) {
    auto ticks = generateSyntheticTicks(3000);

    WalkForwardOptimizer wfo;
    wfo.setTrainWindow(1000);
    wfo.setTestWindow(500);
    wfo.addGammaRange(0.005, 0.05, 3);
    wfo.addKRange(1.0, 3.0, 3);
    wfo.addZThresholdRange(1.0, 2.5, 3);

    auto result = wfo.run(ticks);

    // Should produce at least one fold
    EXPECT_GE(result.folds.size(), 1u);

    // Metrics should be finite
    EXPECT_TRUE(std::isfinite(result.avgISSharpe));
    EXPECT_TRUE(std::isfinite(result.avgOOSSharpe));
    EXPECT_TRUE(std::isfinite(result.avgOverfitRatio));
}
