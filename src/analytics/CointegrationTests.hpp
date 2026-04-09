#pragma once
/**
 * @file CointegrationTests.hpp
 * @brief Statistical tests for cointegration, mean reversion, and regime detection.
 *
 * Implements:
 *   1. Engle-Granger two-step test (OLS + ADF on residuals)
 *   2. Johansen cointegration rank test (trace + max-eigenvalue)
 *   3. Augmented Dickey-Fuller with BIC lag selection
 *   4. Ornstein-Uhlenbeck MLE via Brent's method
 *   5. Regime break detection via correlation/volatility changes
 *
 * Key difference from textbook implementations:
 *   - ADF includes lagged differences (truly *augmented*, not plain DF)
 *   - OU uses concentrated MLE, not biased AR(1) OLS
 *   - MacKinnon (1996) p-values via response surface approximation
 *   - Johansen uses generalized eigenvalue for bivariate case
 *
 * All methods are static with no state. Thread-safe by construction.
 */

#include <cmath>
#include <vector>
#include <algorithm>
#include <numeric>
#include <limits>

namespace analytics {

/**
 * @brief Cointegration test results.
 */
struct CointegrationResult {
    double beta;                    ///< OLS hedge ratio
    double alpha;                   ///< OLS intercept
    double adf_stat;                ///< ADF test statistic
    double p_value;                 ///< MacKinnon approximate p-value
    bool is_cointegrated;           ///< True if ADF rejects unit root at 5%
    double half_life;               ///< OU half-life in periods
    double mean_reversion_speed;    ///< OU theta parameter
    double ou_mu;                   ///< OU long-run mean
    double ou_sigma;                ///< OU diffusion coefficient
    double log_likelihood;          ///< OU MLE log-likelihood
    int adf_lags;                   ///< Number of ADF lags selected by BIC
};

/**
 * @brief Johansen test results.
 */
struct JohansenResult {
    int rank;                       ///< Cointegration rank (0, 1, or 2)
    double traceStatR0;             ///< Trace statistic for H0: r=0
    double traceStatR1;             ///< Trace statistic for H0: r<=1
    double maxEigStatR0;            ///< Max eigenvalue statistic for r=0
    double eigenvalue1;             ///< Largest eigenvalue
    double eigenvalue2;             ///< Smallest eigenvalue
    double beta1;                   ///< First cointegrating vector component
    double beta2;                   ///< Second cointegrating vector component
    bool traceRejectsR0;            ///< Trace test rejects r=0 at 5%
    bool maxEigRejectsR0;           ///< Max-eigenvalue test rejects r=0 at 5%
};

/**
 * @brief OU MLE fit results (more complete than AR(1) approximation).
 */
struct OUMLEResult {
    double theta;                   ///< Mean reversion speed
    double mu;                      ///< Long-run mean
    double sigma;                   ///< Diffusion coefficient
    double halfLife;                ///< ln(2) / theta
    double logLikelihood;           ///< Maximized log-likelihood
    double aic;                     ///< Akaike Information Criterion
    double bic;                     ///< Bayesian Information Criterion
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
 *
 * All methods are static. No internal state.
 */
class CointegrationAnalyzer {
public:
    // ========================================================================
    // Engle-Granger Two-Step Test
    // ========================================================================

    /**
     * @brief Run Engle-Granger cointegration test.
     *
     * Step 1: OLS regression log(A) = alpha + beta * log(B) + epsilon
     * Step 2: ADF test on residuals with BIC lag selection
     * Step 3: Fit OU process via MLE for half-life estimation
     *
     * Note: Engle-Granger critical values differ from standard ADF
     * because the residuals are from a fitted regression.
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

        // OLS: logA = alpha + beta*logB
        double sumA = 0, sumB = 0, sumAB = 0, sumB2 = 0;
        for (size_t i = 0; i < n; ++i) {
            sumA += logA[i];
            sumB += logB[i];
            sumAB += logA[i] * logB[i];
            sumB2 += logB[i] * logB[i];
        }

        double meanA = sumA / static_cast<double>(n);
        double meanB = sumB / static_cast<double>(n);
        double cov = sumAB / static_cast<double>(n) - meanA * meanB;
        double varB = sumB2 / static_cast<double>(n) - meanB * meanB;

        if (std::abs(varB) < 1e-15) return result;

        result.beta = cov / varB;
        result.alpha = meanA - result.beta * meanB;

        // Compute residuals
        std::vector<double> residuals(n);
        for (size_t i = 0; i < n; ++i) {
            residuals[i] = logA[i] - result.alpha - result.beta * logB[i];
        }

        // ADF test on residuals with BIC lag selection
        auto [adfStat, lags] = adfTestWithLags(residuals);
        result.adf_stat = adfStat;
        result.adf_lags = lags;

        // Engle-Granger critical values (for 2 variables, no trend)
        // From MacKinnon (1996) response surface
        result.p_value = mackinnonPValue(adfStat, n, 2);
        result.is_cointegrated = (result.p_value < 0.05);

        // OU MLE for half-life
        auto ouResult = fitOU_MLE(residuals);
        result.mean_reversion_speed = ouResult.theta;
        result.half_life = ouResult.halfLife;
        result.ou_mu = ouResult.mu;
        result.ou_sigma = ouResult.sigma;
        result.log_likelihood = ouResult.logLikelihood;

        return result;
    }

    // ========================================================================
    // Johansen Cointegration Test (Bivariate)
    // ========================================================================

    /**
     * @brief Johansen cointegration rank test for bivariate system.
     *
     * For the bivariate case, the VECM is:
     *   Delta(X_t) = Pi * X_{t-1} + sum(Gamma_i * Delta(X_{t-i})) + epsilon
     * where Pi = alpha * beta' has rank r (the cointegration rank).
     *
     * The test computes eigenvalues of the 2x2 matrix:
     *   S01 * inv(S00) * S01' * inv(S11)
     * via the quadratic formula (no Eigen needed for 2x2).
     *
     * @param pricesA First asset prices
     * @param pricesB Second asset prices
     * @param lags Number of VAR lags (default: 1)
     */
    static JohansenResult johansenTest(const std::vector<double>& pricesA,
                                        const std::vector<double>& pricesB,
                                        int lags = 1) {
        JohansenResult result{};

        size_t n = pricesA.size();
        if (n != pricesB.size() || n < static_cast<size_t>(30 + lags)) {
            return result;
        }

        // Convert to log prices
        std::vector<double> logA(n), logB(n);
        for (size_t i = 0; i < n; ++i) {
            logA[i] = std::log(pricesA[i]);
            logB[i] = std::log(pricesB[i]);
        }

        // Compute first differences
        size_t T = n - 1 - lags;
        std::vector<double> dA(T), dB(T);       // Delta X_t
        std::vector<double> lA(T), lB(T);       // X_{t-1} (levels)

        for (size_t t = 0; t < T; ++t) {
            size_t idx = t + lags;
            dA[t] = logA[idx + 1] - logA[idx];
            dB[t] = logB[idx + 1] - logB[idx];
            lA[t] = logA[idx];
            lB[t] = logB[idx];
        }

        // Residuals from regressing Delta X on lagged Delta X
        // For simplicity with lag=1, we use the concentration approach:
        // R0 = residuals from regressing dX on constant (and lagged diffs)
        // R1 = residuals from regressing X_{t-1} on constant (and lagged diffs)

        // For the basic case (no lagged diffs correction), compute moment matrices
        // S_ij = (1/T) * R_i' * R_j

        // Demean the variables
        double meanDA = 0, meanDB = 0, meanLA = 0, meanLB = 0;
        for (size_t t = 0; t < T; ++t) {
            meanDA += dA[t]; meanDB += dB[t];
            meanLA += lA[t]; meanLB += lB[t];
        }
        meanDA /= T; meanDB /= T;
        meanLA /= T; meanLB /= T;

        // S00: 2x2 covariance of demeaned dX
        double s00_11 = 0, s00_12 = 0, s00_22 = 0;
        // S11: 2x2 covariance of demeaned X_{t-1}
        double s11_11 = 0, s11_12 = 0, s11_22 = 0;
        // S01: 2x2 cross-covariance
        double s01_11 = 0, s01_12 = 0, s01_21 = 0, s01_22 = 0;

        for (size_t t = 0; t < T; ++t) {
            double r0a = dA[t] - meanDA;
            double r0b = dB[t] - meanDB;
            double r1a = lA[t] - meanLA;
            double r1b = lB[t] - meanLB;

            s00_11 += r0a * r0a; s00_12 += r0a * r0b; s00_22 += r0b * r0b;
            s11_11 += r1a * r1a; s11_12 += r1a * r1b; s11_22 += r1b * r1b;
            s01_11 += r0a * r1a; s01_12 += r0a * r1b;
            s01_21 += r0b * r1a; s01_22 += r0b * r1b;
        }

        double Tn = static_cast<double>(T);
        s00_11 /= Tn; s00_12 /= Tn; s00_22 /= Tn;
        s11_11 /= Tn; s11_12 /= Tn; s11_22 /= Tn;
        s01_11 /= Tn; s01_12 /= Tn; s01_21 /= Tn; s01_22 /= Tn;

        // Solve generalized eigenvalue problem:
        // |lambda * S11 - S10 * inv(S00) * S01| = 0
        // S10 = S01' (transpose)

        // inv(S00) for 2x2
        double detS00 = s00_11 * s00_22 - s00_12 * s00_12;
        if (std::abs(detS00) < 1e-20) return result;
        double invS00_11 =  s00_22 / detS00;
        double invS00_12 = -s00_12 / detS00;
        double invS00_22 =  s00_11 / detS00;

        // M = S10 * inv(S00) * S01  (S10 = S01')
        // First: tmp = inv(S00) * S01
        double tmp_11 = invS00_11 * s01_11 + invS00_12 * s01_21;
        double tmp_12 = invS00_11 * s01_12 + invS00_12 * s01_22;
        double tmp_21 = invS00_12 * s01_11 + invS00_22 * s01_21;
        double tmp_22 = invS00_12 * s01_12 + invS00_22 * s01_22;

        // M = S10 * tmp = S01' * tmp
        double m_11 = s01_11 * tmp_11 + s01_21 * tmp_21;
        double m_12 = s01_11 * tmp_12 + s01_21 * tmp_22;
        double m_21 = s01_12 * tmp_11 + s01_22 * tmp_21;
        double m_22 = s01_12 * tmp_12 + s01_22 * tmp_22;

        // Solve |lambda * S11 - M| = 0
        // inv(S11) * M has eigenvalues lambda
        double detS11 = s11_11 * s11_22 - s11_12 * s11_12;
        if (std::abs(detS11) < 1e-20) return result;
        double invS11_11 =  s11_22 / detS11;
        double invS11_12 = -s11_12 / detS11;
        double invS11_22 =  s11_11 / detS11;

        // A = inv(S11) * M
        double a_11 = invS11_11 * m_11 + invS11_12 * m_21;
        double a_12 = invS11_11 * m_12 + invS11_12 * m_22;
        double a_21 = invS11_12 * m_11 + invS11_22 * m_21;
        double a_22 = invS11_12 * m_12 + invS11_22 * m_22;

        // Eigenvalues of A via quadratic formula
        double trace = a_11 + a_22;
        double det = a_11 * a_22 - a_12 * a_21;
        double disc = trace * trace - 4.0 * det;
        if (disc < 0) disc = 0;

        double sqrtDisc = std::sqrt(disc);
        result.eigenvalue1 = (trace + sqrtDisc) / 2.0;
        result.eigenvalue2 = (trace - sqrtDisc) / 2.0;

        // Ensure eigenvalues are in [0, 1]
        result.eigenvalue1 = std::clamp(result.eigenvalue1, 0.0, 0.9999);
        result.eigenvalue2 = std::clamp(result.eigenvalue2, 0.0, 0.9999);

        // Test statistics
        result.traceStatR0 = -Tn * (std::log(1.0 - result.eigenvalue1)
                                      + std::log(1.0 - result.eigenvalue2));
        result.traceStatR1 = -Tn * std::log(1.0 - result.eigenvalue2);
        result.maxEigStatR0 = -Tn * std::log(1.0 - result.eigenvalue1);

        // Osterwald-Lenum (1992) critical values at 5% for bivariate
        // Trace test: r=0: 15.41, r<=1: 3.76
        // Max-eigenvalue: r=0: 14.07, r<=1: 3.76
        constexpr double traceCritR0 = 15.41;
        constexpr double maxEigCritR0 = 14.07;

        result.traceRejectsR0 = (result.traceStatR0 > traceCritR0);
        result.maxEigRejectsR0 = (result.maxEigStatR0 > maxEigCritR0);

        // Determine rank
        if (result.traceRejectsR0 && result.traceStatR1 > 3.76) {
            result.rank = 2;  // Both variables are stationary (unlikely for prices)
        } else if (result.traceRejectsR0) {
            result.rank = 1;  // One cointegrating relationship
        } else {
            result.rank = 0;  // No cointegration
        }

        // Extract cointegrating vector (eigenvector for largest eigenvalue)
        // (A - lambda*I) * v = 0
        double d = a_11 - result.eigenvalue1;
        if (std::abs(d) > std::abs(a_21)) {
            result.beta1 = 1.0;
            result.beta2 = -d / a_12;
        } else {
            result.beta1 = -a_22 + result.eigenvalue1;
            result.beta2 = a_21;
        }
        // Normalize so first component is 1
        if (std::abs(result.beta1) > 1e-15) {
            result.beta2 /= result.beta1;
            result.beta1 = 1.0;
        }

        return result;
    }

    // ========================================================================
    // Augmented Dickey-Fuller Test
    // ========================================================================

    /**
     * @brief ADF test with BIC-based lag selection.
     *
     * Tests: Delta(y_t) = alpha + beta*y_{t-1} + sum(gamma_i*Delta(y_{t-i})) + e_t
     * H0: beta = 0 (unit root, non-stationary)
     * H1: beta < 0 (stationary)
     *
     * Lag selection follows Schwert (1989): maxLag = floor(12*(n/100)^{1/4})
     * Then selects the lag that minimizes BIC.
     *
     * @return {ADF t-statistic, optimal lag count}
     */
    static std::pair<double, int> adfTestWithLags(const std::vector<double>& series) {
        size_t n = series.size();
        if (n < 20) return {0.0, 0};

        // Schwert (1989) max lag rule
        int maxLag = static_cast<int>(std::floor(
            12.0 * std::pow(static_cast<double>(n) / 100.0, 0.25)));
        maxLag = std::min(maxLag, static_cast<int>(n / 4));
        maxLag = std::max(maxLag, 1);

        double bestBIC = std::numeric_limits<double>::max();
        double bestStat = 0;
        int bestLag = 0;

        for (int lag = 0; lag <= maxLag; ++lag) {
            auto [stat, bic] = adfWithFixedLag(series, lag);
            if (bic < bestBIC) {
                bestBIC = bic;
                bestStat = stat;
                bestLag = lag;
            }
        }

        return {bestStat, bestLag};
    }

    // ========================================================================
    // Ornstein-Uhlenbeck MLE
    // ========================================================================

    /**
     * @brief Fit OU process via concentrated Maximum Likelihood Estimation.
     *
     * The OU process: dX_t = theta*(mu - X_t)*dt + sigma*dW_t
     * Discretized: X_{t+1} = phi*X_t + (1-phi)*mu + sigma_d*epsilon
     * where phi = exp(-theta*dt), sigma_d = sigma*sqrt((1-phi^2)/(2*theta))
     *
     * The concentrated log-likelihood (profiling out mu and sigma_d) is
     * a function of phi alone, optimized via Brent's method on (0, 1).
     *
     * Why MLE over AR(1) OLS?
     * - AR(1) OLS gives biased theta estimates in finite samples
     * - MLE is consistent and asymptotically efficient
     * - MLE naturally provides model selection criteria (AIC, BIC)
     *
     * @param series Time series of spread values
     * @param dt Time step (default 1.0 for discrete periods)
     */
    static OUMLEResult fitOU_MLE(const std::vector<double>& series, double dt = 1.0) {
        OUMLEResult result{};
        size_t n = series.size();
        if (n < 10) return result;

        // Sufficient statistics
        double Sx = 0, Sy = 0, Sxx = 0, Syy = 0, Sxy = 0;
        for (size_t i = 0; i < n - 1; ++i) {
            double x = series[i];
            double y = series[i + 1];
            Sx += x;
            Sy += y;
            Sxx += x * x;
            Syy += y * y;
            Sxy += x * y;
        }
        double m = static_cast<double>(n - 1);

        // Brent's method to maximize concentrated log-likelihood over phi in (0, 1)
        // The concentrated log-likelihood (up to constant) for given phi:
        //   mu_hat = (Sy - phi*Sx) / (m*(1-phi))
        //   sigma2_hat = (1/m) * sum((y_i - phi*x_i - (1-phi)*mu_hat)^2)
        //   logL = -m/2 * log(sigma2_hat)

        auto concentratedNegLogL = [&](double phi) -> double {
            if (phi <= 0 || phi >= 1) return 1e30;
            double mu_hat = (Sy - phi * Sx) / (m * (1.0 - phi));
            double c = (1.0 - phi) * mu_hat;

            // sigma2_hat = (Syy - 2*phi*Sxy - 2*c*Sy + phi^2*Sxx + 2*phi*c*Sx + m*c^2) / m
            double sigma2 = (Syy - 2.0 * phi * Sxy - 2.0 * c * Sy
                             + phi * phi * Sxx + 2.0 * phi * c * Sx
                             + m * c * c) / m;
            if (sigma2 <= 0) return 1e30;
            return m / 2.0 * std::log(sigma2);
        };

        // Brent's method on [epsilon, 1-epsilon]
        double phiOpt = brentMinimize(concentratedNegLogL, 0.001, 0.999, 1e-8, 100);

        // Recover parameters at optimal phi
        double mu_hat = (Sy - phiOpt * Sx) / (m * (1.0 - phiOpt));
        double c = (1.0 - phiOpt) * mu_hat;
        double sigma2_hat = (Syy - 2.0 * phiOpt * Sxy - 2.0 * c * Sy
                             + phiOpt * phiOpt * Sxx + 2.0 * phiOpt * c * Sx
                             + m * c * c) / m;

        // Convert to continuous-time OU parameters
        result.theta = -std::log(std::max(phiOpt, 1e-10)) / dt;
        result.mu = mu_hat;

        // sigma_continuous = sigma_discrete * sqrt(2*theta / (1-phi^2))
        if (1.0 - phiOpt * phiOpt > 1e-15) {
            result.sigma = std::sqrt(sigma2_hat * 2.0 * result.theta
                                     / (1.0 - phiOpt * phiOpt));
        } else {
            result.sigma = std::sqrt(sigma2_hat);
        }

        result.halfLife = std::log(2.0) / std::max(result.theta, 1e-10);

        // Log-likelihood
        result.logLikelihood = -m / 2.0 * std::log(2.0 * M_PI)
                               - m / 2.0 * std::log(sigma2_hat) - m / 2.0;

        // Information criteria (3 parameters: theta, mu, sigma)
        result.aic = -2.0 * result.logLikelihood + 6.0;
        result.bic = -2.0 * result.logLikelihood + 3.0 * std::log(m);

        return result;
    }

    /**
     * @brief Fast AR(1) OU approximation (kept for backward compatibility).
     * @return {theta, halfLife}
     */
    static std::pair<double, double> fitOU_AR1(const std::vector<double>& series) {
        size_t n = series.size();
        if (n < 10) return {0, 0};

        double sumX = 0, sumY = 0, sumXY = 0, sumX2 = 0;
        for (size_t i = 0; i < n - 1; ++i) {
            sumX += series[i];
            sumY += series[i + 1];
            sumXY += series[i] * series[i + 1];
            sumX2 += series[i] * series[i];
        }

        double m = static_cast<double>(n - 1);
        double varX = sumX2 / m - (sumX / m) * (sumX / m);
        double covXY = sumXY / m - (sumX / m) * (sumY / m);

        if (std::abs(varX) < 1e-15) return {0, 0};

        double rho = covXY / varX;
        double theta = -std::log(std::clamp(rho, 0.01, 0.9999));
        double halfLife = std::log(2.0) / theta;

        return {theta, halfLife};
    }

    // ========================================================================
    // Regime Break Detection
    // ========================================================================

    /**
     * @brief Detect regime break in spread dynamics.
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

        result.correlation_before = correlation(pricesA, pricesB, 0, mid);
        result.correlation_after = correlation(pricesA, pricesB, mid, n);

        std::vector<double> spreadBefore, spreadAfter;
        for (size_t i = 0; i < mid; ++i) {
            spreadBefore.push_back(std::log(pricesA[i]) - std::log(pricesB[i]));
        }
        for (size_t i = mid; i < n; ++i) {
            spreadAfter.push_back(std::log(pricesA[i]) - std::log(pricesB[i]));
        }

        result.spread_std_before = stdDev(spreadBefore);
        result.spread_std_after = stdDev(spreadAfter);

        double corrDrop = result.correlation_before - result.correlation_after;
        double volRatio = (result.spread_std_before > 1e-15)
                          ? result.spread_std_after / result.spread_std_before
                          : 1.0;

        result.regime_break_detected = (corrDrop > 0.2) || (volRatio > 2.0);
        result.break_index = static_cast<int>(mid);

        return result;
    }

private:
    // ========================================================================
    // ADF with fixed lag count
    // ========================================================================

    /**
     * @brief ADF test with a specific number of lagged differences.
     * @return {t-statistic, BIC}
     */
    static std::pair<double, double> adfWithFixedLag(
        const std::vector<double>& series, int lag) {

        size_t n = series.size();
        size_t T = n - 1 - static_cast<size_t>(lag);  // Effective sample size

        if (T < 10) return {0.0, 1e30};

        // Construct regression variables
        // dy[t] = alpha + beta*y[t-1] + sum(gamma_i * dy[t-i]) + epsilon
        std::vector<double> dy(T);
        std::vector<double> yLag(T);
        std::vector<std::vector<double>> dyLags(lag, std::vector<double>(T));

        for (size_t t = 0; t < T; ++t) {
            size_t idx = t + static_cast<size_t>(lag);
            dy[t] = series[idx + 1] - series[idx];
            yLag[t] = series[idx];
            for (int j = 0; j < lag; ++j) {
                dyLags[j][t] = series[idx - j] - series[idx - j - 1];
            }
        }

        // OLS: dy = alpha + beta*yLag + sum(gamma_i*dyLag_i)
        // Using normal equations with manual column construction
        // Number of regressors: 1 (constant) + 1 (yLag) + lag (dyLags) = 2 + lag
        int k = 2 + lag;  // Number of regressors

        // For simplicity, use the Frisch-Waugh approach:
        // Partial out the constant and lagged diffs, then regress residuals
        // But for correctness, implement full OLS for the beta coefficient

        // Demean approach for the case with constant:
        // After demeaning (and partialling out lagged diffs), get beta and SE

        // Simple approach: compute beta via partial regression
        // Residualize dy and yLag on {constant, dyLags}

        auto residualize = [&](const std::vector<double>& y,
                                const std::vector<std::vector<double>>& X) -> std::vector<double> {
            // y - X * (X'X)^{-1} * X'y
            // For the constant + lagged diffs, use iterative demeaning
            std::vector<double> res = y;
            size_t sz = y.size();

            // Remove mean
            double mean = 0;
            for (double v : res) mean += v;
            mean /= static_cast<double>(sz);
            for (double& v : res) v -= mean;

            // Remove each lagged diff via sequential OLS
            for (const auto& x : X) {
                double xMean = 0;
                for (double v : x) xMean += v;
                xMean /= static_cast<double>(sz);

                double sxy = 0, sxx = 0;
                for (size_t i = 0; i < sz; ++i) {
                    double xd = x[i] - xMean;
                    sxy += res[i] * xd;
                    sxx += xd * xd;
                }
                if (std::abs(sxx) > 1e-15) {
                    double coeff = sxy / sxx;
                    for (size_t i = 0; i < sz; ++i) {
                        res[i] -= coeff * (x[i] - xMean);
                    }
                }
            }
            return res;
        };

        std::vector<double> dyResid = residualize(dy, dyLags);
        std::vector<double> yLagResid = residualize(yLag, dyLags);

        // Now regress dyResid on yLagResid
        double sumXY = 0, sumX2 = 0;
        for (size_t t = 0; t < T; ++t) {
            sumXY += dyResid[t] * yLagResid[t];
            sumX2 += yLagResid[t] * yLagResid[t];
        }

        if (std::abs(sumX2) < 1e-15) return {0.0, 1e30};

        double beta = sumXY / sumX2;

        // Residuals and standard error
        double sse = 0;
        for (size_t t = 0; t < T; ++t) {
            double resid = dyResid[t] - beta * yLagResid[t];
            sse += resid * resid;
        }

        double s2 = sse / static_cast<double>(T - k);
        double seBeta = std::sqrt(s2 / sumX2);
        double tStat = beta / seBeta;

        // BIC = T*log(sse/T) + k*log(T)
        double bic = static_cast<double>(T) * std::log(sse / static_cast<double>(T))
                     + static_cast<double>(k) * std::log(static_cast<double>(T));

        return {tStat, bic};
    }

    // ========================================================================
    // MacKinnon (1996) Approximate P-Values
    // ========================================================================

    /**
     * @brief MacKinnon response surface p-value approximation.
     *
     * Uses cubic interpolation of critical values as a function of
     * test statistic. Coefficients from MacKinnon (1996).
     */
    static double mackinnonPValue(double adfStat, size_t sampleSize, int numVars) {
        // Response surface coefficients for tau statistic
        // Format: {1%, 5%, 10%} critical values = c1 + c2/T + c3/T^2
        // For Engle-Granger (numVars = 2, with constant, no trend):
        struct CriticalValues {
            double c1, c2, c3;
        };

        // MacKinnon (1996) Table 1 for case 2 (constant, no trend), N=2
        constexpr CriticalValues cv1pct  = {-3.9001, -10.534, -30.03};  // 1%
        constexpr CriticalValues cv5pct  = {-3.3377,  -5.967, -8.98};   // 5%
        constexpr CriticalValues cv10pct = {-3.0462,  -4.069, -5.73};   // 10%

        (void)numVars;  // Currently only supports N=2

        double T = static_cast<double>(sampleSize);
        double invT = 1.0 / T;
        double invT2 = invT * invT;

        double crit1 = cv1pct.c1 + cv1pct.c2 * invT + cv1pct.c3 * invT2;
        double crit5 = cv5pct.c1 + cv5pct.c2 * invT + cv5pct.c3 * invT2;
        double crit10 = cv10pct.c1 + cv10pct.c2 * invT + cv10pct.c3 * invT2;

        // Interpolate p-value
        if (adfStat < crit1) return 0.005;
        if (adfStat < crit5) {
            // Linear interpolation between 1% and 5%
            double frac = (adfStat - crit1) / (crit5 - crit1);
            return 0.01 + frac * 0.04;
        }
        if (adfStat < crit10) {
            double frac = (adfStat - crit5) / (crit10 - crit5);
            return 0.05 + frac * 0.05;
        }
        // Beyond 10% critical value
        double frac = std::min(1.0, (adfStat - crit10) / 2.0);
        return 0.10 + frac * 0.40;
    }

    // ========================================================================
    // Brent's Method for 1D Minimization
    // ========================================================================

    /**
     * @brief Brent's method for minimizing f on [a, b].
     * Combines golden section with parabolic interpolation.
     */
    template <typename F>
    static double brentMinimize(F&& f, double a, double b,
                                 double tol, int maxIter) {
        constexpr double GOLDEN = 0.3819660;  // (3 - sqrt(5)) / 2

        double x = a + GOLDEN * (b - a);
        double w = x, v = x;
        double fx = f(x), fw = fx, fv = fx;
        double d = 0, e = 0;

        for (int iter = 0; iter < maxIter; ++iter) {
            double midpoint = 0.5 * (a + b);
            double tol1 = tol * std::abs(x) + 1e-10;
            double tol2 = 2.0 * tol1;

            if (std::abs(x - midpoint) <= tol2 - 0.5 * (b - a)) {
                return x;
            }

            // Try parabolic interpolation
            bool useParabolic = false;
            double u = 0;

            if (std::abs(e) > tol1) {
                double r = (x - w) * (fx - fv);
                double q = (x - v) * (fx - fw);
                double p = (x - v) * q - (x - w) * r;
                q = 2.0 * (q - r);
                if (q > 0) p = -p;
                else q = -q;
                double oldE = e;
                e = d;

                if (std::abs(p) < std::abs(0.5 * q * oldE) &&
                    p > q * (a - x) && p < q * (b - x)) {
                    d = p / q;
                    u = x + d;
                    if (u - a < tol2 || b - u < tol2) {
                        d = (x < midpoint) ? tol1 : -tol1;
                    }
                    useParabolic = true;
                }
            }

            if (!useParabolic) {
                e = (x < midpoint) ? b - x : a - x;
                d = GOLDEN * e;
            }

            u = x + ((std::abs(d) >= tol1) ? d : ((d > 0) ? tol1 : -tol1));
            double fu = f(u);

            if (fu <= fx) {
                if (u < x) b = x; else a = x;
                v = w; fv = fw;
                w = x; fw = fx;
                x = u; fx = fu;
            } else {
                if (u < x) a = u; else b = u;
                if (fu <= fw || w == x) {
                    v = w; fv = fw;
                    w = u; fw = fu;
                } else if (fu <= fv || v == x || v == w) {
                    v = u; fv = fu;
                }
            }
        }
        return x;
    }

    // ========================================================================
    // Helper Functions
    // ========================================================================

    static double correlation(const std::vector<double>& a, const std::vector<double>& b,
                               size_t start, size_t end) {
        double sumA = 0, sumB = 0, sumAB = 0, sumA2 = 0, sumB2 = 0;
        double n = static_cast<double>(end - start);

        for (size_t i = start; i < end; ++i) {
            sumA += a[i]; sumB += b[i];
            sumAB += a[i] * b[i];
            sumA2 += a[i] * a[i]; sumB2 += b[i] * b[i];
        }

        double meanA = sumA / n, meanB = sumB / n;
        double covAB = sumAB / n - meanA * meanB;
        double stdA = std::sqrt(std::max(sumA2 / n - meanA * meanA, 0.0));
        double stdB = std::sqrt(std::max(sumB2 / n - meanB * meanB, 0.0));

        if (stdA < 1e-15 || stdB < 1e-15) return 0;
        return covAB / (stdA * stdB);
    }

    static double stdDev(const std::vector<double>& v) {
        if (v.empty()) return 0;
        double sum = std::accumulate(v.begin(), v.end(), 0.0);
        double mean = sum / static_cast<double>(v.size());
        double sq_sum = 0;
        for (double x : v) sq_sum += (x - mean) * (x - mean);
        return std::sqrt(sq_sum / static_cast<double>(v.size()));
    }
};

} // namespace analytics
