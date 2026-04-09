#pragma once
/**
 * @file KyleLambda.hpp
 * @brief Kyle's lambda: permanent price impact per unit of signed order flow.
 *
 * In Kyle (1985), the market maker's pricing rule is:
 *   P_t = P_{t-1} + lambda * OFI_t + noise
 *
 * Lambda measures the permanent price impact of order flow:
 *   - High lambda: illiquid market, each unit of flow moves price more
 *   - Low lambda: liquid market, flow is absorbed with minimal impact
 *
 * Estimated via rolling OLS regression of price changes on signed OFI.
 * The regression uses ring-buffer accumulators (Sx, Sy, Sxy, Sx2) for
 * O(1) per-tick updates with no heap allocation.
 *
 * Usage in A-S framework:
 *   When lambda is high, the market maker should widen spreads because
 *   each fill carries larger adverse selection risk.
 *
 * Reference: Kyle (1985) "Continuous Auctions and Insider Trading"
 */

#include <cmath>
#include <algorithm>
#include <array>

namespace signals {

/// Maximum window size for Kyle's lambda estimation
inline constexpr size_t MAX_KYLE_WINDOW = 500;

/**
 * @brief Kyle's lambda estimation result.
 */
struct KyleLambdaResult {
    double lambda;      ///< Price impact coefficient (dp/dOFI)
    double rSquared;    ///< Goodness of fit
    double tStat;       ///< t-statistic for lambda significance
    double stdErr;      ///< Standard error of lambda
    size_t samples;     ///< Number of observations in window

    /**
     * @brief Check if lambda is statistically significant at 5% level.
     */
    [[nodiscard]] bool isSignificant() const noexcept {
        return samples >= 30 && std::abs(tStat) > 1.96;
    }
};

/**
 * @brief Rolling estimation of Kyle's lambda (permanent price impact).
 *
 * Usage:
 * @code
 *   KyleLambda kyle(200);  // 200-tick window
 *   kyle.update(priceChange, ofiValue);
 *   auto result = kyle.estimate();
 *   if (result.isSignificant()) {
 *       double impact = result.lambda;  // bp per unit OFI
 *   }
 * @endcode
 */
class KyleLambda {
public:
    /**
     * @brief Construct Kyle's lambda estimator.
     * @param window Rolling window size for OLS regression.
     */
    explicit KyleLambda(size_t window = 200) noexcept
        : window_(std::min(window, MAX_KYLE_WINDOW)),
          head_(0), count_(0),
          Sx_(0), Sy_(0), Sxy_(0), Sx2_(0), Sy2_(0) {}

    /**
     * @brief Update with a new observation.
     * @param deltaP Price change (P_t - P_{t-1})
     * @param ofi Signed order flow imbalance
     */
    void update(double deltaP, double ofi) noexcept {
        // If window is full, remove oldest observation
        if (count_ >= window_) {
            size_t idx = head_ % window_;
            double oldX = xBuffer_[idx];
            double oldY = yBuffer_[idx];
            Sx_  -= oldX;
            Sy_  -= oldY;
            Sxy_ -= oldX * oldY;
            Sx2_ -= oldX * oldX;
            Sy2_ -= oldY * oldY;
            count_--;
        }

        // Store new observation
        size_t idx = head_ % window_;
        xBuffer_[idx] = ofi;
        yBuffer_[idx] = deltaP;
        head_ = (head_ + 1) % window_;

        // Update accumulators
        Sx_  += ofi;
        Sy_  += deltaP;
        Sxy_ += ofi * deltaP;
        Sx2_ += ofi * ofi;
        Sy2_ += deltaP * deltaP;
        count_++;
    }

    /**
     * @brief Compute Kyle's lambda and regression statistics.
     * @return KyleLambdaResult with lambda, R^2, t-stat.
     */
    [[nodiscard]] KyleLambdaResult estimate() const noexcept {
        KyleLambdaResult result{0, 0, 0, 0, count_};

        if (count_ < 10) return result;

        double n = static_cast<double>(count_);
        double meanX = Sx_ / n;
        double meanY = Sy_ / n;

        // OLS: lambda = Cov(X,Y) / Var(X)
        double varX = Sx2_ / n - meanX * meanX;
        double covXY = Sxy_ / n - meanX * meanY;

        if (std::abs(varX) < 1e-15) return result;

        result.lambda = covXY / varX;

        // R-squared
        double varY = Sy2_ / n - meanY * meanY;
        if (std::abs(varY) > 1e-15) {
            result.rSquared = (covXY * covXY) / (varX * varY);
        }

        // Standard error and t-stat
        // Residual variance: s^2 = (1/(n-2)) * sum(e_i^2)
        // sum(e_i^2) = sum(y_i^2) - 2*beta*sum(x_i*y_i) + beta^2*sum(x_i^2)
        //            - n*(alpha^2 - 2*alpha*mean_y + 2*alpha*beta*mean_x)
        // Simpler: SSR = n * varY * (1 - R^2)
        double ssr = n * varY * (1.0 - result.rSquared);
        double s2 = ssr / std::max(n - 2.0, 1.0);

        // se(lambda) = sqrt(s^2 / (n * varX))
        result.stdErr = std::sqrt(s2 / (n * varX));

        if (result.stdErr > 1e-15) {
            result.tStat = result.lambda / result.stdErr;
        }

        return result;
    }

    /**
     * @brief Get current lambda (shorthand).
     */
    [[nodiscard]] double lambda() const noexcept {
        return estimate().lambda;
    }

    /**
     * @brief Reset estimator state.
     */
    void reset() noexcept {
        head_ = count_ = 0;
        Sx_ = Sy_ = Sxy_ = Sx2_ = Sy2_ = 0;
    }

private:
    size_t window_;
    size_t head_;
    size_t count_;

    // Ring buffer storage (no heap allocation)
    std::array<double, MAX_KYLE_WINDOW> xBuffer_{};  ///< OFI values
    std::array<double, MAX_KYLE_WINDOW> yBuffer_{};  ///< Price changes

    // Running accumulators for O(1) OLS
    double Sx_, Sy_, Sxy_, Sx2_, Sy2_;
};

} // namespace signals
