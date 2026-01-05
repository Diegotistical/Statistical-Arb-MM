#pragma once
/**
 * @file CointegrationTests.hpp
 * @brief Statistical tests for cointegration and mean reversion.
 * 
 * Implements:
 *   - Engle-Granger test (ADF on residuals)
 *   - OU half-life estimation
 *   - Regime break detection
 */

#include <cmath>
#include <vector>
#include <algorithm>
#include <numeric>

namespace analytics {

/**
 * @brief Cointegration test results.
 */
struct CointegrationResult {
    double beta;           ///< Hedge ratio
    double adf_stat;       ///< ADF test statistic
    double p_value;        ///< Approximate p-value
    bool is_cointegrated;  ///< True if statistically cointegrated
    double half_life;      ///< OU half-life in periods
    double mean_reversion_speed;  ///< θ parameter
};

/**
 * @brief Regime break detection result.
 */
struct RegimeBreakResult {
    bool regime_break_detected;
    int break_index;
    double correlation_before;
    double correlation_after;
    double spread_std_before;
    double spread_std_after;
};

/**
 * @brief Cointegration analysis tools.
 */
class CointegrationAnalyzer {
public:
    /**
     * @brief Run Engle-Granger cointegration test.
     * 
     * Step 1: Estimate β via OLS (log(A) = α + β*log(B) + ε)
     * Step 2: Run ADF test on residuals
     * Step 3: If ADF rejects unit root, pair is cointegrated
     * 
     * @param pricesA First asset prices
     * @param pricesB Second asset prices
     * @return Test result with β, ADF stat, half-life
     */
    static CointegrationResult engleGranger(const std::vector<double>& pricesA,
                                             const std::vector<double>& pricesB) {
        CointegrationResult result{};
        
        if (pricesA.size() != pricesB.size() || pricesA.size() < 30) {
            return result;
        }
        
        size_t n = pricesA.size();
        
        // Convert to log prices
        std::vector<double> logA(n), logB(n);
        for (size_t i = 0; i < n; ++i) {
            logA[i] = std::log(pricesA[i]);
            logB[i] = std::log(pricesB[i]);
        }
        
        // OLS: logA = α + β*logB
        double sumA = 0, sumB = 0, sumAB = 0, sumB2 = 0;
        for (size_t i = 0; i < n; ++i) {
            sumA += logA[i];
            sumB += logB[i];
            sumAB += logA[i] * logB[i];
            sumB2 += logB[i] * logB[i];
        }
        
        double meanA = sumA / n;
        double meanB = sumB / n;
        double cov = sumAB / n - meanA * meanB;
        double varB = sumB2 / n - meanB * meanB;
        
        result.beta = cov / varB;
        double alpha = meanA - result.beta * meanB;
        
        // Compute residuals (spread)
        std::vector<double> residuals(n);
        for (size_t i = 0; i < n; ++i) {
            residuals[i] = logA[i] - alpha - result.beta * logB[i];
        }
        
        // ADF test on residuals
        result.adf_stat = adfTest(residuals);
        
        // Critical values (5% level, n=100-500)
        // Engle-Granger critical values differ from standard ADF
        double criticalValue = -3.37;  // 5% level for 2 variables
        result.is_cointegrated = (result.adf_stat < criticalValue);
        
        // Approximate p-value (rough interpolation)
        if (result.adf_stat < -4.0) {
            result.p_value = 0.01;
        } else if (result.adf_stat < -3.5) {
            result.p_value = 0.05;
        } else if (result.adf_stat < -3.0) {
            result.p_value = 0.10;
        } else {
            result.p_value = 0.50;
        }
        
        // Estimate OU parameters from residuals
        auto [theta, halfLife] = fitOU(residuals);
        result.mean_reversion_speed = theta;
        result.half_life = halfLife;
        
        return result;
    }

    /**
     * @brief Detect regime break in spread dynamics.
     * 
     * Splits data in half and compares:
     *   - Correlation between assets
     *   - Spread volatility
     * 
     * @param pricesA First asset prices
     * @param pricesB Second asset prices
     * @param windowSize Lookback for rolling stats
     */
    static RegimeBreakResult detectRegimeBreak(const std::vector<double>& pricesA,
                                                const std::vector<double>& pricesB,
                                                size_t windowSize = 100) {
        RegimeBreakResult result{};
        
        if (pricesA.size() < 2 * windowSize) {
            return result;
        }
        
        size_t n = pricesA.size();
        size_t mid = n / 2;
        
        // Compute correlation before and after midpoint
        result.correlation_before = correlation(pricesA, pricesB, 0, mid);
        result.correlation_after = correlation(pricesA, pricesB, mid, n);
        
        // Compute spread std before and after
        std::vector<double> spreadBefore, spreadAfter;
        for (size_t i = 0; i < mid; ++i) {
            spreadBefore.push_back(std::log(pricesA[i]) - std::log(pricesB[i]));
        }
        for (size_t i = mid; i < n; ++i) {
            spreadAfter.push_back(std::log(pricesA[i]) - std::log(pricesB[i]));
        }
        
        result.spread_std_before = stdDev(spreadBefore);
        result.spread_std_after = stdDev(spreadAfter);
        
        // Detect break if correlation dropped significantly or vol doubled
        double corrDrop = result.correlation_before - result.correlation_after;
        double volRatio = result.spread_std_after / result.spread_std_before;
        
        result.regime_break_detected = (corrDrop > 0.2) || (volRatio > 2.0);
        result.break_index = static_cast<int>(mid);
        
        return result;
    }

private:
    /**
     * @brief Augmented Dickey-Fuller test statistic.
     * 
     * Tests: Δy_t = α + β*y_{t-1} + ε_t
     * H0: β = 0 (unit root)
     * H1: β < 0 (stationary)
     * 
     * Returns t-statistic for β.
     */
    static double adfTest(const std::vector<double>& series) {
        size_t n = series.size();
        if (n < 10) return 0;
        
        // Compute first differences
        std::vector<double> dy(n - 1);
        std::vector<double> y_lag(n - 1);
        
        for (size_t i = 0; i < n - 1; ++i) {
            dy[i] = series[i + 1] - series[i];
            y_lag[i] = series[i];
        }
        
        // OLS: dy = α + β*y_lag
        double sumDy = 0, sumY = 0, sumDyY = 0, sumY2 = 0;
        for (size_t i = 0; i < n - 1; ++i) {
            sumDy += dy[i];
            sumY += y_lag[i];
            sumDyY += dy[i] * y_lag[i];
            sumY2 += y_lag[i] * y_lag[i];
        }
        
        double m = n - 1;
        double meanDy = sumDy / m;
        double meanY = sumY / m;
        double cov = sumDyY / m - meanDy * meanY;
        double varY = sumY2 / m - meanY * meanY;
        
        double beta = cov / varY;
        double alpha = meanDy - beta * meanY;
        
        // Compute residuals and standard error
        double sse = 0;
        for (size_t i = 0; i < n - 1; ++i) {
            double resid = dy[i] - alpha - beta * y_lag[i];
            sse += resid * resid;
        }
        
        double se = std::sqrt(sse / (m - 2) / varY / m);
        
        // t-statistic
        return beta / se;
    }

    /**
     * @brief Fit Ornstein-Uhlenbeck process.
     * @return {θ (mean reversion speed), half-life}
     */
    static std::pair<double, double> fitOU(const std::vector<double>& series) {
        size_t n = series.size();
        if (n < 10) return {0, 0};
        
        // AR(1): X_t = ρ*X_{t-1} + ε
        double sumX = 0, sumY = 0, sumXY = 0, sumX2 = 0;
        for (size_t i = 0; i < n - 1; ++i) {
            sumX += series[i];
            sumY += series[i + 1];
            sumXY += series[i] * series[i + 1];
            sumX2 += series[i] * series[i];
        }
        
        double m = n - 1;
        double rho = (sumXY - sumX * sumY / m) / (sumX2 - sumX * sumX / m);
        
        // θ = -log(ρ) (for dt=1)
        double theta = -std::log(std::max(rho, 0.01));
        double halfLife = std::log(2) / theta;
        
        return {theta, halfLife};
    }

    static double correlation(const std::vector<double>& a, const std::vector<double>& b,
                               size_t start, size_t end) {
        double sumA = 0, sumB = 0, sumAB = 0, sumA2 = 0, sumB2 = 0;
        size_t n = end - start;
        
        for (size_t i = start; i < end; ++i) {
            sumA += a[i];
            sumB += b[i];
            sumAB += a[i] * b[i];
            sumA2 += a[i] * a[i];
            sumB2 += b[i] * b[i];
        }
        
        double meanA = sumA / n;
        double meanB = sumB / n;
        double cov = sumAB / n - meanA * meanB;
        double stdA = std::sqrt(sumA2 / n - meanA * meanA);
        double stdB = std::sqrt(sumB2 / n - meanB * meanB);
        
        return cov / (stdA * stdB);
    }

    static double stdDev(const std::vector<double>& v) {
        double sum = std::accumulate(v.begin(), v.end(), 0.0);
        double mean = sum / v.size();
        double sq_sum = 0;
        for (double x : v) sq_sum += (x - mean) * (x - mean);
        return std::sqrt(sq_sum / v.size());
    }
};

} // namespace analytics
