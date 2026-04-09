#pragma once
/**
 * @file WalkForward.hpp
 * @brief Walk-forward optimization framework for strategy parameter selection.
 *
 * Walk-forward analysis is the gold standard for backtesting parameter
 * optimization because it prevents overfitting:
 *
 *   1. Divide data into rolling train/test windows
 *   2. On each training window, sweep a parameter grid
 *   3. Select best parameters by in-sample Sharpe ratio
 *   4. Evaluate on the out-of-sample test window
 *   5. Report overfit ratio: OOS Sharpe / IS Sharpe
 *
 * An overfit ratio > 0.5 suggests parameters are robust.
 * An overfit ratio < 0.3 indicates significant overfitting.
 *
 * Why event-driven (not vectorized)?
 *   - Vectorized backtests assume instantaneous fills at mid
 *   - This produces unrealistically high Sharpe ratios
 *   - Event-driven with queue position and latency gives realistic PnL
 *
 * Usage:
 * @code
 *   WalkForwardOptimizer wfo;
 *   wfo.setTrainWindow(5000);  // 5000 ticks training
 *   wfo.setTestWindow(1000);   // 1000 ticks test
 *   wfo.addGammaRange(0.001, 0.1, 10);
 *   wfo.addKRange(0.5, 5.0, 10);
 *   auto results = wfo.run(allTicks);
 * @endcode
 */

#include <vector>
#include <functional>
#include <algorithm>
#include <cmath>
#include <iostream>

#include "Simulator.hpp"

namespace backtest {

/**
 * @brief Result of a single walk-forward fold.
 */
struct WalkForwardFold {
    int foldIndex;
    size_t trainStart;
    size_t trainEnd;
    size_t testStart;
    size_t testEnd;

    // Best in-sample parameters
    double bestGamma;
    double bestK;
    double bestZThreshold;

    // Performance
    double inSampleSharpe;
    double outOfSampleSharpe;
    double inSamplePnL;
    double outOfSamplePnL;
    double outOfSampleDrawdown;

    /**
     * @brief Overfit ratio: OOS Sharpe / IS Sharpe.
     * > 0.5 = robust, 0.3-0.5 = moderate overfit, < 0.3 = severe overfit
     */
    [[nodiscard]] double overfitRatio() const {
        if (std::abs(inSampleSharpe) < 1e-10) return 0;
        return outOfSampleSharpe / inSampleSharpe;
    }
};

/**
 * @brief Aggregate walk-forward optimization results.
 */
struct WalkForwardResult {
    std::vector<WalkForwardFold> folds;

    // Aggregate metrics
    double avgISSharpe = 0;
    double avgOOSSharpe = 0;
    double avgOverfitRatio = 0;
    double totalOOSPnL = 0;
    double maxOOSDrawdown = 0;

    void compute() {
        if (folds.empty()) return;

        double sumIS = 0, sumOOS = 0, sumOR = 0;
        for (const auto& f : folds) {
            sumIS += f.inSampleSharpe;
            sumOOS += f.outOfSampleSharpe;
            sumOR += f.overfitRatio();
            totalOOSPnL += f.outOfSamplePnL;
            maxOOSDrawdown = std::max(maxOOSDrawdown, f.outOfSampleDrawdown);
        }

        double n = static_cast<double>(folds.size());
        avgISSharpe = sumIS / n;
        avgOOSSharpe = sumOOS / n;
        avgOverfitRatio = sumOR / n;
    }

    void print() const {
        std::cout << "=== Walk-Forward Results ===\n"
                  << "Folds:            " << folds.size() << "\n"
                  << "Avg IS Sharpe:    " << avgISSharpe << "\n"
                  << "Avg OOS Sharpe:   " << avgOOSSharpe << "\n"
                  << "Avg Overfit Ratio:" << avgOverfitRatio << "\n"
                  << "Total OOS PnL:    " << totalOOSPnL << "\n"
                  << "Max OOS Drawdown: " << maxOOSDrawdown << "\n\n";

        for (const auto& f : folds) {
            std::cout << "  Fold " << f.foldIndex
                      << ": IS=" << f.inSampleSharpe
                      << " OOS=" << f.outOfSampleSharpe
                      << " OR=" << f.overfitRatio()
                      << " gamma=" << f.bestGamma
                      << " k=" << f.bestK
                      << " z=" << f.bestZThreshold << "\n";
        }
    }
};

/**
 * @brief Walk-forward optimizer for strategy parameters.
 */
class WalkForwardOptimizer {
public:
    /**
     * @brief Set training window size in ticks.
     */
    void setTrainWindow(size_t ticks) { trainWindow_ = ticks; }

    /**
     * @brief Set test window size in ticks.
     */
    void setTestWindow(size_t ticks) { testWindow_ = ticks; }

    /**
     * @brief Set step size between folds (default: testWindow).
     */
    void setStepSize(size_t ticks) { stepSize_ = ticks; }

    /**
     * @brief Add gamma values to sweep.
     */
    void addGammaRange(double min, double max, int steps) {
        gammaValues_.clear();
        for (int i = 0; i < steps; ++i) {
            double t = static_cast<double>(i) / std::max(steps - 1, 1);
            gammaValues_.push_back(min + t * (max - min));
        }
    }

    /**
     * @brief Add k (intensity) values to sweep.
     */
    void addKRange(double min, double max, int steps) {
        kValues_.clear();
        for (int i = 0; i < steps; ++i) {
            double t = static_cast<double>(i) / std::max(steps - 1, 1);
            kValues_.push_back(min + t * (max - min));
        }
    }

    /**
     * @brief Add z-score threshold values to sweep.
     */
    void addZThresholdRange(double min, double max, int steps) {
        zValues_.clear();
        for (int i = 0; i < steps; ++i) {
            double t = static_cast<double>(i) / std::max(steps - 1, 1);
            zValues_.push_back(min + t * (max - min));
        }
    }

    /**
     * @brief Run walk-forward optimization on tick data.
     *
     * @param ticks Complete dataset of tick events
     * @return WalkForwardResult with per-fold and aggregate metrics
     */
    WalkForwardResult run(const std::vector<TickEvent>& ticks) {
        WalkForwardResult result;

        if (ticks.size() < trainWindow_ + testWindow_) {
            return result;
        }

        // Default parameter ranges if not set
        if (gammaValues_.empty()) addGammaRange(0.001, 0.1, 5);
        if (kValues_.empty()) addKRange(0.5, 5.0, 5);
        if (zValues_.empty()) addZThresholdRange(1.0, 3.0, 5);

        size_t step = (stepSize_ > 0) ? stepSize_ : testWindow_;
        int foldIdx = 0;

        for (size_t start = 0; start + trainWindow_ + testWindow_ <= ticks.size();
             start += step) {

            size_t trainEnd = start + trainWindow_;
            size_t testEnd = trainEnd + testWindow_;

            WalkForwardFold fold;
            fold.foldIndex = foldIdx++;
            fold.trainStart = start;
            fold.trainEnd = trainEnd;
            fold.testStart = trainEnd;
            fold.testEnd = testEnd;

            // Grid search on training window
            double bestSharpe = -1e30;

            for (double gamma : gammaValues_) {
                for (double k : kValues_) {
                    for (double z : zValues_) {
                        double sharpe = evaluateParams(ticks, start, trainEnd,
                                                       gamma, k, z);
                        if (sharpe > bestSharpe) {
                            bestSharpe = sharpe;
                            fold.bestGamma = gamma;
                            fold.bestK = k;
                            fold.bestZThreshold = z;
                        }
                    }
                }
            }

            fold.inSampleSharpe = bestSharpe;

            // Evaluate best params on test window
            auto testMetrics = evaluateWithMetrics(
                ticks, trainEnd, testEnd,
                fold.bestGamma, fold.bestK, fold.bestZThreshold);

            fold.outOfSampleSharpe = testMetrics.sharpeRatio;
            fold.outOfSamplePnL = testMetrics.totalPnL;
            fold.outOfSampleDrawdown = testMetrics.maxDrawdown;
            fold.inSamplePnL = evaluateWithMetrics(
                ticks, start, trainEnd,
                fold.bestGamma, fold.bestK, fold.bestZThreshold).totalPnL;

            result.folds.push_back(fold);
        }

        result.compute();
        return result;
    }

private:
    size_t trainWindow_ = 5000;
    size_t testWindow_ = 1000;
    size_t stepSize_ = 0;

    std::vector<double> gammaValues_;
    std::vector<double> kValues_;
    std::vector<double> zValues_;

    /**
     * @brief Evaluate parameters on a window and return Sharpe.
     */
    double evaluateParams(const std::vector<TickEvent>& ticks,
                          size_t start, size_t end,
                          double gamma, double k, double zThreshold) {
        auto metrics = evaluateWithMetrics(ticks, start, end, gamma, k, zThreshold);
        return metrics.sharpeRatio;
    }

    /**
     * @brief Evaluate parameters and return full metrics.
     */
    SimulationMetrics evaluateWithMetrics(
        const std::vector<TickEvent>& ticks,
        size_t start, size_t end,
        double gamma, double k, double zThreshold) {

        Simulator sim;
        sim.strategy().gamma = gamma;
        sim.strategy().k = k;
        sim.strategy().zEntryThreshold = zThreshold;
        sim.strategy().zExitThreshold = zThreshold / 3.0;

        // Set session times from data
        if (end > start) {
            sim.strategy().setSessionTimes(ticks[start].timestamp,
                                            ticks[end - 1].timestamp);
        }

        for (size_t i = start; i < end && i < ticks.size(); ++i) {
            sim.onTick(ticks[i]);
        }

        return sim.metrics();
    }
};

} // namespace backtest
