/**
 * @file main.cpp
 * @brief Statistical Arbitrage Market Making Engine Demo.
 *
 * Demonstrates the full production pipeline:
 *   1. Kalman-filtered hedge ratio tracking
 *   2. Multi-signal generation (spread z-score, OFI, VPIN, Kyle's lambda)
 *   3. Avellaneda-Stoikov optimal quoting with terminal time degradation
 *   4. Real-time risk management (position limits, drawdown, kill switch)
 *   5. Realistic execution simulation (latency, queue, adverse selection)
 *   6. PnL decomposition and performance attribution
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
#include "signals/KalmanFilter.hpp"
#include "signals/OFI.hpp"
#include "signals/VPIN.hpp"
#include "signals/KyleLambda.hpp"
#include "strategy/StatArbMM.hpp"
#include "risk/RiskManager.hpp"
#include "execution/ExecutionSimulator.hpp"
#include "execution/TransactionCosts.hpp"
#include "analytics/PnLAnalytics.hpp"
#include "analytics/CointegrationTests.hpp"

using Clock = std::chrono::high_resolution_clock;

// ============================================================================
// Demo 1: Full Pipeline with All Components
// ============================================================================

void demoFullPipeline() {
    std::cout << "\n=== Full Pipeline Demo ===\n\n";

    // --- Setup components ---
    TickNormalizer ticks(0.01);
    signals::KalmanHedgeRatio kalman(0.999, 1e-3);
    signals::SpreadModel spreadModel(100);
    spreadModel.setKalmanFilter(&kalman);
    signals::OFI ofi(100);
    signals::VPIN vpin(500, 50);
    signals::KyleLambda kyle(200);

    strategy::StatArbMM mm;
    mm.gamma = 0.01;
    mm.k = 1.5;
    mm.maxInventory = 500;
    mm.alphaVpin = 1.0;
    mm.alphaLambda = 0.5;

    risk::RiskManager riskMgr;
    riskMgr.config().maxPositionPerSymbol = 500;
    riskMgr.config().maxDrawdown = 5000.0;
    mm.setRiskManager(&riskMgr);

    analytics::PnLAnalytics pnl;
    execution::TransactionCostModel txCost;
    txCost.setDailyVolume(1e6);

    // Session: 1000 ticks = 1 second
    int64_t sessionStartNs = 0;
    int64_t sessionEndNs = 1000 * 1000000LL;
    mm.setSessionTimes(sessionStartNs, sessionEndNs);

    std::mt19937_64 rng(42);
    std::normal_distribution<> priceDist(0, 0.01);
    std::normal_distribution<> spreadDist(0, 0.005);

    // Simulate cointegrated pair
    double priceA = 150.0;
    double priceB = 100.0;
    double spreadState = 0;
    int fills = 0;

    std::cout << "Running 1000 ticks with full signal suite...\n\n";
    std::cout << std::fixed << std::setprecision(3);

    for (int t = 0; t < 1000; ++t) {
        int64_t currentNs = t * 1000000LL;

        // Evolve prices (cointegrated with mean-reverting spread)
        priceB *= std::exp(priceDist(rng));
        spreadState = 0.9 * spreadState + spreadDist(rng);
        priceA = priceB * 1.5 * std::exp(spreadState);

        // --- Signal updates ---
        double z = spreadModel.update(priceA, priceB);
        double bidSize = 1000 + 500 * spreadState;
        double askSize = 1000 - 500 * spreadState;
        ofi.update(bidSize, askSize);

        double priceChange = (t > 0) ? (priceA - priceB) * 0.01 : 0;
        vpin.onTrade(priceA, 100);
        kyle.update(priceChange, ofi.value());

        // --- Strategy ---
        mm.updateTime(currentNs);
        double fairPrice = ticks.toTicks(priceA);
        double vol = std::max(spreadModel.stdDev() * 100, 0.5);

        double vpinVal = vpin.isValid() ? vpin.value() : 0.0;
        auto kyleResult = kyle.estimate();
        double kyleLambdaNorm = kyleResult.isSignificant() ? kyleResult.lambda : 0.0;

        strategy::Quote quote = mm.computeQuotes(
            fairPrice, vol, z, ofi.normalized(), vpinVal, kyleLambdaNorm);

        // --- Simulated fills (simplified) ---
        if (quote.valid && quote.riskCheck == risk::RiskCheckResult::Passed
            && (rng() % 100 < 8)) {
            OrderSide side = (z < -1.0) ? OrderSide::Buy : OrderSide::Sell;
            Quantity qty = 100;
            double fillPrice = (side == OrderSide::Buy)
                ? ticks.toPrice(quote.bidPrice)
                : ticks.toPrice(quote.askPrice);

            mm.onFill(side, qty);
            riskMgr.onFill(0, side, qty, fillPrice);
            riskMgr.updateMark(0, priceA);

            double signedQty = (side == OrderSide::Buy) ? 100.0 : -100.0;
            double spread = (quote.askPrice - quote.bidPrice) * ticks.tickSize();
            pnl.onTrade(currentNs, fillPrice, signedQty, priceA, spread, true);

            auto cost = txCost.estimateCost(qty, fillPrice, spread, true,
                                             side == OrderSide::Buy);
            pnl.addTransactionCost(cost.totalCost);
            fills++;
        }

        pnl.updateMark(currentNs, priceA);
        if (t % 50 == 0) pnl.snapshot();

        // Log every 200 ticks
        if (t % 200 == 0) {
            std::cout << "t=" << std::setw(4) << t
                      << " | A=$" << std::setprecision(2) << priceA
                      << " B=$" << priceB
                      << " | beta=" << std::setprecision(3) << spreadModel.beta()
                      << " z=" << std::setw(6) << z
                      << " | OFI=" << std::setw(6) << ofi.normalized()
                      << " | VPIN=" << std::setprecision(2) << vpinVal
                      << " | tau=" << mm.tau()
                      << " | inv=" << std::setw(4) << mm.inventory()
                      << "\n";
        }
    }

    // --- Results ---
    auto snap = riskMgr.snapshot();
    auto metrics = pnl.computeMetrics();
    auto breakdown = pnl.breakdown();

    std::cout << "\n--- Results ---\n"
              << "  Fills:              " << fills << "\n"
              << "  Final inventory:    " << mm.inventory() << "\n"
              << "  Kalman beta:        " << std::setprecision(4) << spreadModel.beta() << "\n"
              << "  Total PnL:          $" << std::setprecision(2) << breakdown.total << "\n"
              << "  Realized PnL:       $" << breakdown.realized << "\n"
              << "  Spread capture:     $" << breakdown.spread_capture << "\n"
              << "  Adverse selection:  $" << breakdown.adverse_selection << "\n"
              << "  Transaction costs:  $" << breakdown.transaction_costs << "\n"
              << "  Sharpe ratio:       " << std::setprecision(3) << metrics.sharpeRatio << "\n"
              << "  Sortino ratio:      " << metrics.sortinoRatio << "\n"
              << "  Max drawdown:       $" << std::setprecision(2) << snap.maxDrawdownSeen << "\n"
              << "  Kill switch:        " << (snap.killSwitchActive ? "ACTIVE" : "inactive") << "\n";
}

// ============================================================================
// Demo 2: Cointegration Analysis
// ============================================================================

void demoCointAnalysis() {
    std::cout << "\n=== Cointegration Analysis Demo ===\n\n";

    std::mt19937_64 rng(42);
    std::normal_distribution<> noise(0, 0.01);

    // Generate cointegrated pair
    std::vector<double> pricesA(500), pricesB(500);
    pricesB[0] = 100.0;
    double spread = 0;
    pricesA[0] = std::exp(std::log(pricesB[0]) * 1.5 + spread);

    for (int i = 1; i < 500; ++i) {
        pricesB[i] = pricesB[i-1] * std::exp(noise(rng));
        spread = spread * 0.9 + noise(rng);
        pricesA[i] = std::exp(std::log(pricesB[i]) * 1.5 + spread);
    }

    // Engle-Granger
    auto eg = analytics::CointegrationAnalyzer::engleGranger(pricesA, pricesB);
    std::cout << "Engle-Granger Test:\n"
              << "  Hedge ratio (beta): " << std::setprecision(4) << eg.beta << "\n"
              << "  ADF statistic:      " << eg.adf_stat << "\n"
              << "  p-value:            " << eg.p_value << "\n"
              << "  Cointegrated:       " << (eg.is_cointegrated ? "YES" : "NO") << "\n"
              << "  ADF lags (BIC):     " << eg.adf_lags << "\n"
              << "  OU half-life (MLE): " << std::setprecision(1) << eg.half_life << " periods\n"
              << "  OU theta:           " << std::setprecision(4) << eg.mean_reversion_speed << "\n"
              << "  Log-likelihood:     " << std::setprecision(1) << eg.log_likelihood << "\n\n";

    // Johansen
    auto joh = analytics::CointegrationAnalyzer::johansenTest(pricesA, pricesB);
    std::cout << "Johansen Test:\n"
              << "  Rank:               " << joh.rank << "\n"
              << "  Trace stat (r=0):   " << std::setprecision(3) << joh.traceStatR0 << "\n"
              << "  Max-eig stat (r=0): " << joh.maxEigStatR0 << "\n"
              << "  Rejects r=0:        " << (joh.traceRejectsR0 ? "YES" : "NO") << "\n"
              << "  Eigenvalues:        [" << std::setprecision(4) << joh.eigenvalue1
              << ", " << joh.eigenvalue2 << "]\n"
              << "  Coint. vector:      [" << joh.beta1 << ", " << joh.beta2 << "]\n";
}

// ============================================================================
// Demo 3: Risk Manager
// ============================================================================

void demoRiskManager() {
    std::cout << "\n=== Risk Manager Demo ===\n\n";

    risk::RiskManager rm;
    rm.config().maxPositionPerSymbol = 500;
    rm.config().maxOrderSize = 200;
    rm.config().maxDrawdown = 1000.0;

    auto check = rm.preTradeCheck(0, OrderSide::Buy, 100, 15000);
    std::cout << "Order(Buy 100 @ 150.00): " << risk::RiskManager::rejectionString(check) << "\n";

    check = rm.preTradeCheck(0, OrderSide::Buy, 300, 15000);
    std::cout << "Order(Buy 300 @ 150.00): " << risk::RiskManager::rejectionString(check) << "\n";

    rm.onFill(0, OrderSide::Buy, 450, 150.0);
    check = rm.preTradeCheck(0, OrderSide::Buy, 100, 15000);
    std::cout << "Order(Buy 100, pos=450): " << risk::RiskManager::rejectionString(check) << "\n";

    rm.updateMark(0, 155.0);
    rm.updateMark(0, 145.0);
    auto snap = rm.snapshot();
    std::cout << "After price drop:        DD=$" << std::fixed << std::setprecision(0)
              << snap.maxDrawdownSeen
              << " kill=" << (snap.killSwitchActive ? "ACTIVE" : "off") << "\n";
}

// ============================================================================
// Demo 4: Performance Benchmark
// ============================================================================

void runBenchmark() {
    std::cout << "\n=== Performance Benchmark ===\n\n";

    constexpr int NUM_OPS = 1'000'000;
    std::mt19937_64 rng(42);
    std::normal_distribution<> dist(0, 0.01);

    // Kalman update
    signals::KalmanHedgeRatio kf(0.9999, 1e-3);
    auto start = Clock::now();
    for (int i = 0; i < NUM_OPS; ++i) {
        kf.update(5.0 + dist(rng), 4.0 + dist(rng));
    }
    auto end = Clock::now();
    double ns_kalman = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count()
                       / static_cast<double>(NUM_OPS);

    // computeQuotes with risk
    strategy::StatArbMM mm;
    mm.gamma = 0.01; mm.k = 1.5; mm.setInventory(50);
    risk::RiskManager rm;
    mm.setRiskManager(&rm);

    start = Clock::now();
    for (int i = 0; i < NUM_OPS; ++i) {
        auto q = mm.computeQuotes(15000, 0.02, 1.2, 0.3, 0.4, 0.1);
        (void)q;
    }
    end = Clock::now();
    double ns_quotes = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count()
                       / static_cast<double>(NUM_OPS);

    // preTradeCheck
    start = Clock::now();
    for (int i = 0; i < NUM_OPS; ++i) {
        auto r = rm.preTradeCheck(0, OrderSide::Buy, 100, 15000);
        (void)r;
    }
    end = Clock::now();
    double ns_risk = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count()
                     / static_cast<double>(NUM_OPS);

    std::cout << std::fixed << std::setprecision(1);
    std::cout << "KalmanFilter.update:     " << ns_kalman << " ns/op\n"
              << "StatArbMM.computeQuotes: " << ns_quotes << " ns/op  (with risk check)\n"
              << "RiskManager.preCheck:    " << ns_risk << " ns/op\n"
              << "Operations:              " << NUM_OPS / 1'000'000 << "M each\n";
}

// ============================================================================
// Main
// ============================================================================

int main() {
    std::cout << "============================================================\n";
    std::cout << "  Statistical Arbitrage Market Making Engine\n";
    std::cout << "  C++20 | Avellaneda-Stoikov | Kalman | VPIN | Risk Mgmt\n";
    std::cout << "============================================================\n";
    std::cout << "Order size:    " << sizeof(Order) << " bytes\n";
    std::cout << "Max orders:    " << OrderBook::MAX_ORDER_ID << "\n";

    demoFullPipeline();
    demoCointAnalysis();
    demoRiskManager();
    runBenchmark();

    std::cout << "\n============================================================\n";
    std::cout << "  Demo Complete\n";
    std::cout << "============================================================\n";

    return 0;
}
