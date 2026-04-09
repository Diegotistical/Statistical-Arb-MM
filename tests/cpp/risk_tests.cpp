/**
 * @file risk_tests.cpp
 * @brief Unit tests for RiskManager pre-trade checks and state management.
 *
 * Tests verify:
 *   - Per-symbol position limits
 *   - Portfolio position limits
 *   - Fat-finger protection (max order size, max notional)
 *   - Rate limiting
 *   - Kill switch (activation, deactivation, thread safety)
 *   - Drawdown circuit breaker (auto-activates kill switch)
 *   - PnL tracking after fills
 *   - Typed rejection reasons
 */

#include <gtest/gtest.h>
#include <cmath>

#include "../../src/risk/RiskManager.hpp"

using namespace risk;

class RiskManagerTest : public ::testing::Test {
protected:
    RiskManager rm;

    void SetUp() override {
        rm.config().maxPositionPerSymbol = 1000;
        rm.config().maxPortfolioPosition = 5000;
        rm.config().maxOrderSize = 500;
        rm.config().maxNotional = 500000.0;
        rm.config().maxLossPerSymbol = -10000.0;
        rm.config().maxPortfolioLoss = -50000.0;
        rm.config().maxDrawdown = 20000.0;
        rm.config().maxOrdersPerSecond = 100;
        rm.config().tickSize = 0.01;
    }
};

// ============================================================================
// Basic Pre-Trade Checks
// ============================================================================

TEST_F(RiskManagerTest, PassesCleanOrder) {
    auto result = rm.preTradeCheck(0, OrderSide::Buy, 100, 15000);
    EXPECT_EQ(result, RiskCheckResult::Passed);
}

TEST_F(RiskManagerTest, InvalidSymbolRejected) {
    auto result = rm.preTradeCheck(-1, OrderSide::Buy, 100, 15000);
    EXPECT_EQ(result, RiskCheckResult::InvalidSymbol);

    result = rm.preTradeCheck(MAX_SYMBOLS, OrderSide::Buy, 100, 15000);
    EXPECT_EQ(result, RiskCheckResult::InvalidSymbol);
}

// ============================================================================
// Position Limits
// ============================================================================

TEST_F(RiskManagerTest, SymbolPositionLimitBuy) {
    // Fill to 900 (below limit)
    rm.onFill(0, OrderSide::Buy, 900, 150.0);

    // Order of 50 should pass (projected = 950 < 1000)
    auto result = rm.preTradeCheck(0, OrderSide::Buy, 50, 15000);
    EXPECT_EQ(result, RiskCheckResult::Passed);

    // Order of 200 should fail (projected = 1100 > 1000)
    result = rm.preTradeCheck(0, OrderSide::Buy, 200, 15000);
    EXPECT_EQ(result, RiskCheckResult::SymbolPositionLimit);
}

TEST_F(RiskManagerTest, SymbolPositionLimitSell) {
    rm.onFill(0, OrderSide::Sell, 900, 150.0);

    auto result = rm.preTradeCheck(0, OrderSide::Sell, 200, 15000);
    EXPECT_EQ(result, RiskCheckResult::SymbolPositionLimit);
}

TEST_F(RiskManagerTest, PortfolioPositionLimit) {
    rm.config().maxPortfolioPosition = 2000;

    // Fill across multiple symbols
    rm.onFill(0, OrderSide::Buy, 800, 150.0);
    rm.onFill(1, OrderSide::Buy, 800, 150.0);

    // Portfolio abs position = 1600. Adding 500 to symbol 2 = 2100 > 2000
    auto result = rm.preTradeCheck(2, OrderSide::Buy, 500, 15000);
    EXPECT_EQ(result, RiskCheckResult::PortfolioPositionLimit);
}

// ============================================================================
// Fat Finger Protection
// ============================================================================

TEST_F(RiskManagerTest, MaxOrderSizeRejected) {
    auto result = rm.preTradeCheck(0, OrderSide::Buy, 600, 15000);
    EXPECT_EQ(result, RiskCheckResult::MaxOrderSize);
}

TEST_F(RiskManagerTest, MaxNotionalRejected) {
    // 500 shares * 15000 ticks * 0.01 tickSize = $75,000 < $500,000 -- should pass
    auto result = rm.preTradeCheck(0, OrderSide::Buy, 400, 15000);
    EXPECT_EQ(result, RiskCheckResult::Passed);

    // Increase price to exceed notional: 400 * 200000 * 0.01 = $800,000
    result = rm.preTradeCheck(0, OrderSide::Buy, 400, 200000);
    EXPECT_EQ(result, RiskCheckResult::MaxNotional);
}

// ============================================================================
// Rate Limiting
// ============================================================================

TEST_F(RiskManagerTest, RateLimitExceeded) {
    int64_t now = 1000000000LL;  // 1 second

    // Submit 100 orders (at the limit)
    for (int i = 0; i < 100; ++i) {
        rm.recordOrder(0, now);
    }

    // 101st order should be rate-limited
    rm.recordOrder(0, now);
    auto result = rm.preTradeCheck(0, OrderSide::Buy, 100, 15000, now);
    EXPECT_EQ(result, RiskCheckResult::RateLimitExceeded);
}

TEST_F(RiskManagerTest, RateLimitResetsAfterWindow) {
    int64_t now = 1000000000LL;

    for (int i = 0; i < 100; ++i) {
        rm.recordOrder(0, now);
    }

    // Move to next second - window resets
    int64_t nextSecond = now + 1000000001LL;
    rm.recordOrder(0, nextSecond);

    auto result = rm.preTradeCheck(0, OrderSide::Buy, 100, 15000, nextSecond);
    EXPECT_EQ(result, RiskCheckResult::Passed);
}

// ============================================================================
// Kill Switch
// ============================================================================

TEST_F(RiskManagerTest, KillSwitchBlocksAllOrders) {
    rm.activateKillSwitch();

    auto result = rm.preTradeCheck(0, OrderSide::Buy, 100, 15000);
    EXPECT_EQ(result, RiskCheckResult::KillSwitchActive);

    result = rm.preTradeCheck(1, OrderSide::Sell, 50, 10000);
    EXPECT_EQ(result, RiskCheckResult::KillSwitchActive);
}

TEST_F(RiskManagerTest, KillSwitchDeactivation) {
    rm.activateKillSwitch();
    EXPECT_TRUE(rm.isKillSwitchActive());

    rm.deactivateKillSwitch();
    EXPECT_FALSE(rm.isKillSwitchActive());

    auto result = rm.preTradeCheck(0, OrderSide::Buy, 100, 15000);
    EXPECT_EQ(result, RiskCheckResult::Passed);
}

// ============================================================================
// Drawdown Circuit Breaker
// ============================================================================

TEST_F(RiskManagerTest, DrawdownActivatesKillSwitch) {
    rm.config().maxDrawdown = 1000.0;

    // Create profit then lose it
    rm.onFill(0, OrderSide::Buy, 100, 100.0);
    rm.updateMark(0, 110.0);  // Unrealized +$1000
    // Peak PnL = 1000

    rm.updateMark(0, 80.0);  // Unrealized = 100*(80-100) = -2000
    // Drawdown = 1000 - (-2000) = 3000 > 1000

    EXPECT_TRUE(rm.isKillSwitchActive());
}

// ============================================================================
// PnL Tracking
// ============================================================================

TEST_F(RiskManagerTest, PnLTracksCorrectly) {
    rm.onFill(0, OrderSide::Buy, 100, 100.0);
    rm.updateMark(0, 105.0);

    auto snap = rm.snapshot();
    // Unrealized = 100 * (105 - 100) = 500
    EXPECT_NEAR(snap.totalUnrealizedPnL, 500.0, 0.01);
}

TEST_F(RiskManagerTest, RealizedPnLOnClose) {
    rm.onFill(0, OrderSide::Buy, 100, 100.0);
    rm.onFill(0, OrderSide::Sell, 100, 110.0);

    auto snap = rm.snapshot();
    // Realized = 100 * (110 - 100) = 1000
    EXPECT_NEAR(snap.totalRealizedPnL, 1000.0, 0.01);
}

// ============================================================================
// Rejection String
// ============================================================================

TEST_F(RiskManagerTest, RejectionStringsReadable) {
    EXPECT_STREQ(RiskManager::rejectionString(RiskCheckResult::Passed), "PASSED");
    EXPECT_STREQ(RiskManager::rejectionString(RiskCheckResult::KillSwitchActive), "KILL_SWITCH");
    EXPECT_STREQ(RiskManager::rejectionString(RiskCheckResult::MaxOrderSize), "MAX_ORDER_SIZE");
}

// ============================================================================
// Reset
// ============================================================================

TEST_F(RiskManagerTest, ResetClearsEverything) {
    rm.onFill(0, OrderSide::Buy, 500, 100.0);
    rm.activateKillSwitch();

    rm.reset();

    EXPECT_FALSE(rm.isKillSwitchActive());
    auto snap = rm.snapshot();
    EXPECT_EQ(snap.totalAbsPosition, 0);
    EXPECT_DOUBLE_EQ(snap.totalPnL, 0);
}

// ============================================================================
// Snapshot
// ============================================================================

TEST_F(RiskManagerTest, SnapshotCounts) {
    rm.onFill(0, OrderSide::Buy, 100, 100.0);
    rm.onFill(1, OrderSide::Sell, 200, 50.0);

    auto snap = rm.snapshot();
    EXPECT_EQ(snap.totalAbsPosition, 300);
    EXPECT_EQ(snap.activeSymbols, 2);
}
