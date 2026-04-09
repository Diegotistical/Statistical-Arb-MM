#pragma once
/**
 * @file KalmanFilter.hpp
 * @brief Online Kalman filter for dynamic hedge ratio estimation.
 *
 * State-space model for time-varying cointegration:
 *   State:       x_t = [alpha_t, beta_t]' (intercept and hedge ratio)
 *   Transition:  x_t = x_{t-1} + w_t,     w_t ~ N(0, Q)
 *   Observation: y_t = H_t * x_t + v_t,   v_t ~ N(0, R)
 *
 * Where y_t = log(price_A_t) and H_t = [1, log(price_B_t)].
 *
 * The delta parameter controls adaptation speed:
 *   Q = P_{t|t-1} * (1 - delta) / delta
 * This avoids specifying Q directly. delta in (0,1):
 *   delta -> 1: slow adaptation (stable hedge ratio)
 *   delta -> 0: fast adaptation (tracking regime changes)
 *
 * Reference: Chan (2013) "Algorithmic Trading", Chapter 2.
 *
 * Design: Hand-rolled 2x2 matrix operations (4 doubles per matrix).
 * No Eigen dependency. Zero heap allocation. All operations are O(1).
 */

#include <cmath>
#include <algorithm>

namespace signals {

/**
 * @brief Kalman-filtered hedge ratio with uncertainty estimates.
 */
struct KalmanState {
    double alpha;           ///< Filtered intercept
    double beta;            ///< Filtered hedge ratio
    double spread;          ///< Innovation (prediction error)
    double spreadVariance;  ///< Innovation variance (for z-score)

    // Covariance matrix P (2x2 symmetric, store 3 elements)
    double P00;             ///< Var(alpha)
    double P01;             ///< Cov(alpha, beta)
    double P11;             ///< Var(beta)

    /**
     * @brief Z-score of spread using Kalman uncertainty.
     * More principled than rolling window z-score because it
     * accounts for parameter estimation uncertainty.
     */
    [[nodiscard]] double zScore() const noexcept {
        if (spreadVariance < 1e-15) return 0.0;
        return spread / std::sqrt(spreadVariance);
    }

    /**
     * @brief Standard error of hedge ratio estimate.
     */
    [[nodiscard]] double betaStdErr() const noexcept {
        return std::sqrt(std::max(P11, 0.0));
    }
};

/**
 * @brief Online Kalman filter for dynamic hedge ratio tracking.
 *
 * Usage:
 * @code
 *   KalmanHedgeRatio kf(1e-4, 1e-3);  // delta=0.9999, R=0.001
 *   for (auto& [priceA, priceB] : prices) {
 *       auto state = kf.update(std::log(priceA), std::log(priceB));
 *       double hedgeRatio = state.beta;
 *       double zScore = state.zScore();
 *   }
 * @endcode
 */
class KalmanHedgeRatio {
public:
    /**
     * @brief Construct Kalman filter for hedge ratio estimation.
     * @param delta Forgetting factor in (0,1). Controls Q = P*(1-delta)/delta.
     *              Higher delta = slower adaptation. Typical: 0.9999 to 0.99.
     * @param Ve Observation noise variance R. Typical: 1e-4 to 1e-2.
     */
    explicit KalmanHedgeRatio(double delta = 0.9999, double Ve = 1e-3) noexcept
        : delta_(delta), Ve_(Ve),
          alpha_(0), beta_(0),
          P00_(1.0), P01_(0.0), P11_(1.0),
          initialized_(false) {}

    /**
     * @brief Update filter with new observation.
     * @param logPriceA log(price_A) - the dependent variable
     * @param logPriceB log(price_B) - the independent variable
     * @return Updated Kalman state with filtered beta, spread, z-score
     */
    KalmanState update(double logPriceA, double logPriceB) noexcept {
        if (!initialized_) {
            // Initialize with simple ratio
            beta_ = 1.0;
            alpha_ = logPriceA - beta_ * logPriceB;
            initialized_ = true;
        }

        // --- Predict step ---
        // State transition is identity (random walk): x_{t|t-1} = x_{t-1|t-1}
        // Covariance prediction: P_{t|t-1} = P_{t-1|t-1} / delta
        // This is equivalent to Q = P * (1-delta)/delta
        double invDelta = 1.0 / delta_;
        double Pp00 = P00_ * invDelta;
        double Pp01 = P01_ * invDelta;
        double Pp11 = P11_ * invDelta;

        // --- Update step ---
        // Observation: y = [1, logPriceB] * [alpha, beta]' + noise
        // Innovation: e = y - H * x_predicted
        double yPred = alpha_ + beta_ * logPriceB;
        double e = logPriceA - yPred;  // Innovation (spread)

        // Innovation variance: S = H * P_{t|t-1} * H' + R
        // H = [1, logPriceB], so:
        // S = P00 + 2*logPriceB*P01 + logPriceB^2*P11 + Ve
        double S = Pp00 + 2.0 * logPriceB * Pp01
                   + logPriceB * logPriceB * Pp11 + Ve_;

        // Kalman gain: K = P_{t|t-1} * H' / S
        // K is 2x1: K = [P00 + P01*logPriceB, P01 + P11*logPriceB]' / S
        double invS = 1.0 / S;
        double K0 = (Pp00 + Pp01 * logPriceB) * invS;
        double K1 = (Pp01 + Pp11 * logPriceB) * invS;

        // State update: x_{t|t} = x_{t|t-1} + K * e
        alpha_ += K0 * e;
        beta_  += K1 * e;

        // Covariance update: P_{t|t} = (I - K*H) * P_{t|t-1}
        // Expanded:
        //   P00_new = (1 - K0) * Pp00 - K0 * logPriceB * Pp01   ... wait
        // Actually: P = P_pred - K * S * K'  (Joseph form is more stable but overkill here)
        // P = P_pred - K * H * P_pred
        // Row 0: [P00, P01] -= K0 * [Pp00 + Pp01*B, Pp01 + Pp11*B]  ... no
        // (I - K*H) = [[1-K0, -K0*B], [-K1, 1-K1*B]]
        // P_new = (I-KH) * P_pred
        double IKH00 = 1.0 - K0;
        double IKH01 = -K0 * logPriceB;
        double IKH10 = -K1;
        double IKH11 = 1.0 - K1 * logPriceB;

        P00_ = IKH00 * Pp00 + IKH01 * Pp01;
        P01_ = IKH00 * Pp01 + IKH01 * Pp11;
        P11_ = IKH10 * Pp01 + IKH11 * Pp11;

        // Return state
        KalmanState state;
        state.alpha = alpha_;
        state.beta = beta_;
        state.spread = e;
        state.spreadVariance = S;
        state.P00 = P00_;
        state.P01 = P01_;
        state.P11 = P11_;

        return state;
    }

    /**
     * @brief Get current hedge ratio estimate.
     */
    [[nodiscard]] double beta() const noexcept { return beta_; }

    /**
     * @brief Get current intercept estimate.
     */
    [[nodiscard]] double alpha() const noexcept { return alpha_; }

    /**
     * @brief Check if filter has been initialized.
     */
    [[nodiscard]] bool initialized() const noexcept { return initialized_; }

    /**
     * @brief Reset filter state.
     */
    void reset() noexcept {
        alpha_ = beta_ = 0;
        P00_ = P11_ = 1.0;
        P01_ = 0;
        initialized_ = false;
    }

    /**
     * @brief Set adaptation speed.
     * @param delta Forgetting factor in (0,1). Lower = faster adaptation.
     */
    void setDelta(double delta) noexcept { delta_ = delta; }

    /**
     * @brief Set observation noise variance.
     */
    void setVe(double Ve) noexcept { Ve_ = Ve; }

private:
    double delta_;          ///< Forgetting factor
    double Ve_;             ///< Observation noise variance

    // State: x = [alpha, beta]'
    double alpha_;          ///< Intercept
    double beta_;           ///< Hedge ratio

    // Covariance P (2x2 symmetric)
    double P00_, P01_, P11_;

    bool initialized_;
};

} // namespace signals
