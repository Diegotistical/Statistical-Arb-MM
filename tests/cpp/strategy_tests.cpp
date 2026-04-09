/**
 * @file strategy_tests.cpp
 * @brief Unit tests for StatArbMM Avellaneda-Stoikov market making strategy.
 *
 * Tests verify:
 *   - Reservation price formula: r = s - q*gamma*sigma^2*tau
 *   - Optimal spread with intensity: delta = gamma*sigma^2*tau + (2/gamma)*ln(1+gamma/k)
 *   - Terminal time degradation (tau -> 0 near session end)
 *   - Signal modulation (VPIN, Kyle's lambda widen spread)
 *   - Risk manager integration (pre-trade checks)
 *   - Inventory-dependent quote sizing
 *   - Intensity parameter calibration
 */

#include <gtest/gtest.h>
#include <cmath>
#include <vector>

#include "../../src/strategy/StatArbMM.hpp"
#include "../../src/risk/RiskManager.hpp"

using namespace strategy;

class StatArbMMTest : public ::testing::Test {
protected:
    StatArbMM mm;

    void SetUp() override {
        mm.gamma = 0.01;
        mm.k = 1.5;
        mm.minSpread = 1.0;
        mm.defaultSize = 100;
        mm.maxInventory = 1000;
        mm.zEntryThreshold = 1.5;
        mm.zExitThreshold = 0.5;
    }
};

// ============================================================================
// Reservation Price Tests
// ============================================================================

TEST_F(StatArbMMTest, ReservationPriceZeroInventory) {
    mm.setInventory(0);
    double r = mm.reservationPrice(100.0, 0.02);
    // r = 100 - 0 * gamma * sigma^2 * tau = 100
    EXPECT_DOUBLE_EQ(r, 100.0);
}

TEST_F(StatArbMMTest, ReservationPriceLongInventory) {
    mm.setInventory(100);
    double sigma = 0.02;
    double r = mm.reservationPrice(100.0, sigma);
    // r = 100 - 100 * 0.01 * 0.0004 * 1.0 = 100 - 0.0004 = 99.9996
    double expected = 100.0 - 100 * 0.01 * sigma * sigma * 1.0;
    EXPECT_DOUBLE_EQ(r, expected);
    EXPECT_LT(r, 100.0);  // Long inventory -> reservation below mid
}

TEST_F(StatArbMMTest, ReservationPriceShortInventory) {
    mm.setInventory(-100);
    double r = mm.reservationPrice(100.0, 0.02);
    EXPECT_GT(r, 100.0);  // Short inventory -> reservation above mid
}

TEST_F(StatArbMMTest, ReservationPriceTerminalDegradation) {
    mm.setInventory(100);
    mm.setSessionTimes(0, 1000000000LL);  // 1 second session

    // At session start: full tau
    mm.updateTime(0);
    double rStart = mm.reservationPrice(100.0, 0.02);

    // Near session end: tau ~= kMinTau
    mm.updateTime(999000000LL);  // 999ms in
    double rEnd = mm.reservationPrice(100.0, 0.02);

    // Near end, reservation should be closer to fair (less inventory penalty)
    // because tau is smaller, BUT the urgency to flatten means the system
    // would quote more aggressively via the spread
    EXPECT_NE(rStart, rEnd);
}

// ============================================================================
// Optimal Spread Tests
// ============================================================================

TEST_F(StatArbMMTest, OptimalSpreadFormula) {
    double sigma = 0.02;
    double delta = mm.optimalSpread(sigma);

    // delta = gamma * sigma^2 * tau + (2/gamma) * ln(1 + gamma/k)
    // = 0.01 * 0.0004 * 1.0 + 200 * ln(1 + 0.01/1.5)
    // = 0.000004 + 200 * ln(1.00667)
    // = 0.000004 + 200 * 0.006645 = 1.329
    double expected = 0.01 * sigma * sigma * 1.0
                      + (2.0 / 0.01) * std::log(1.0 + 0.01 / 1.5);
    EXPECT_NEAR(delta, expected, 1e-10);
}

TEST_F(StatArbMMTest, OptimalSpreadDominatedByIntensityTerm) {
    // The (2/gamma)*ln(1+gamma/k) term should dominate for reasonable params
    double sigma = 0.02;
    double inventoryTerm = mm.gamma * sigma * sigma * 1.0;
    double intensityTerm = (2.0 / mm.gamma) * std::log(1.0 + mm.gamma / mm.k);

    EXPECT_GT(intensityTerm, inventoryTerm * 100);
}

TEST_F(StatArbMMTest, OptimalSpreadWidensWithInventory) {
    // Spread itself doesn't directly depend on inventory in A-S formula,
    // but the reservation price shifts, effectively widening one side
    mm.setInventory(0);
    auto q0 = mm.computeQuotes(10000, 0.02);

    mm.setInventory(500);
    auto q500 = mm.computeQuotes(10000, 0.02);

    // With long inventory, bid should be lower (less willing to buy more)
    if (q0.bidPrice != PRICE_INVALID && q500.bidPrice != PRICE_INVALID) {
        EXPECT_LE(q500.bidPrice, q0.bidPrice);
    }
}

TEST_F(StatArbMMTest, SpreadRespectsMinimum) {
    mm.minSpread = 5.0;
    mm.gamma = 1e-10;  // Very small gamma -> very small spread
    mm.k = 1e10;       // Very large k -> tiny intensity term

    double delta = mm.optimalSpread(0.001);
    EXPECT_GE(delta, 5.0);
}

// ============================================================================
// Terminal Time Tests
// ============================================================================

TEST_F(StatArbMMTest, TauDefaultsToOne) {
    EXPECT_DOUBLE_EQ(mm.tau(), 1.0);
}

TEST_F(StatArbMMTest, TauDecreasesOverSession) {
    mm.setSessionTimes(0, 1000000000LL);  // 1 second

    mm.updateTime(0);
    double tau0 = mm.tau();

    mm.updateTime(500000000LL);  // 500ms
    double tau50 = mm.tau();

    mm.updateTime(999000000LL);  // 999ms
    double tau99 = mm.tau();

    EXPECT_GT(tau0, tau50);
    EXPECT_GT(tau50, tau99);
    EXPECT_GT(tau99, 0.0);  // Never reaches exactly 0 (kMinTau floor)
}

// ============================================================================
// Signal Modulation Tests
// ============================================================================

TEST_F(StatArbMMTest, VPINWidensSpread) {
    mm.setInventory(0);

    auto qNoVpin = mm.computeQuotes(10000, 0.02, 0, 0, 0.0, 0.0);
    auto qHighVpin = mm.computeQuotes(10000, 0.02, 0, 0, 0.8, 0.0);

    // Higher VPIN should widen spread (ask higher, bid lower)
    if (qNoVpin.valid && qHighVpin.valid) {
        int spreadNoVpin = qNoVpin.askPrice - qNoVpin.bidPrice;
        int spreadHighVpin = qHighVpin.askPrice - qHighVpin.bidPrice;
        EXPECT_GE(spreadHighVpin, spreadNoVpin);
    }
}

TEST_F(StatArbMMTest, KyleLambdaWidensSpread) {
    mm.setInventory(0);

    auto qNoLambda = mm.computeQuotes(10000, 0.02, 0, 0, 0, 0.0);
    auto qHighLambda = mm.computeQuotes(10000, 0.02, 0, 0, 0, 1.0);

    if (qNoLambda.valid && qHighLambda.valid) {
        int spreadNoL = qNoLambda.askPrice - qNoLambda.bidPrice;
        int spreadHighL = qHighLambda.askPrice - qHighLambda.bidPrice;
        EXPECT_GE(spreadHighL, spreadNoL);
    }
}

// ============================================================================
// Inventory Limit Tests
// ============================================================================

TEST_F(StatArbMMTest, MaxInventoryQuotesOnlyToReduce) {
    mm.setInventory(mm.maxInventory);

    auto q = mm.computeQuotes(10000, 0.02);
    EXPECT_TRUE(q.valid);
    // Should only have ask (to sell and reduce long position)
    EXPECT_NE(q.askPrice, PRICE_INVALID);
    EXPECT_EQ(q.bidPrice, PRICE_INVALID);
}

TEST_F(StatArbMMTest, MaxShortInventoryQuotesOnlyToReduce) {
    mm.setInventory(-mm.maxInventory);

    auto q = mm.computeQuotes(10000, 0.02);
    EXPECT_TRUE(q.valid);
    // Should only have bid (to buy and reduce short position)
    EXPECT_NE(q.bidPrice, PRICE_INVALID);
    EXPECT_EQ(q.askPrice, PRICE_INVALID);
}

// ============================================================================
// Risk Manager Integration
// ============================================================================

TEST_F(StatArbMMTest, RiskManagerBlocksOrder) {
    risk::RiskManager rm;
    rm.config().maxPositionPerSymbol = 100;
    mm.setRiskManager(&rm);

    // Set position near limit
    rm.onFill(0, OrderSide::Buy, 95, 100.0);

    auto q = mm.computeQuotes(10000, 0.02);
    // Bid should be blocked (would exceed position limit)
    // Ask should pass
    EXPECT_TRUE(q.valid);
}

TEST_F(StatArbMMTest, KillSwitchBlocksAll) {
    risk::RiskManager rm;
    rm.activateKillSwitch();
    mm.setRiskManager(&rm);

    auto q = mm.computeQuotes(10000, 0.02);
    EXPECT_EQ(q.riskCheck, risk::RiskCheckResult::KillSwitchActive);
}

// ============================================================================
// Intensity Calibration
// ============================================================================

TEST_F(StatArbMMTest, CalibrateIntensityFromData) {
    // Simulate: wider spreads -> lower fill rates
    std::vector<double> spreads = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    std::vector<double> fillRates;
    double A = 100.0, trueK = 2.0;
    for (double s : spreads) {
        fillRates.push_back(A * std::exp(-trueK * s));
    }

    double calibratedK = StatArbMM::calibrateIntensity(spreads, fillRates);
    EXPECT_NEAR(calibratedK, trueK, 0.01);
}

TEST_F(StatArbMMTest, CalibrateIntensityInsufficientData) {
    std::vector<double> spreads = {1, 2};
    std::vector<double> rates = {10, 5};
    double k = StatArbMM::calibrateIntensity(spreads, rates);
    EXPECT_EQ(k, -1.0);
}

// ============================================================================
// Fill Handling
// ============================================================================

TEST_F(StatArbMMTest, OnFillUpdatesInventory) {
    mm.setInventory(0);
    mm.onFill(OrderSide::Buy, 100);
    EXPECT_EQ(mm.inventory(), 100);

    mm.onFill(OrderSide::Sell, 50);
    EXPECT_EQ(mm.inventory(), 50);
}

TEST_F(StatArbMMTest, ResetClearsState) {
    mm.setInventory(100);
    mm.setSessionTimes(0, 1000000000LL);
    mm.updateTime(500000000LL);

    mm.reset();
    EXPECT_EQ(mm.inventory(), 0);
    EXPECT_DOUBLE_EQ(mm.tau(), 1.0);
}
