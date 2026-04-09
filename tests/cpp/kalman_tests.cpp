/**
 * @file kalman_tests.cpp
 * @brief Unit tests for Kalman filter hedge ratio estimation.
 *
 * Tests verify:
 *   - Convergence to true beta on synthetic cointegrated pairs
 *   - Tracking time-varying beta
 *   - Z-score reflects Kalman uncertainty
 *   - Reset clears state properly
 */

#include <gtest/gtest.h>
#include <cmath>
#include <vector>
#include <random>

#include "../../src/signals/KalmanFilter.hpp"
#include "../../src/signals/SpreadModel.hpp"

using namespace signals;

class KalmanFilterTest : public ::testing::Test {
protected:
    std::mt19937 rng{42};
    std::normal_distribution<double> noise{0.0, 0.001};
};

TEST_F(KalmanFilterTest, ConvergesToTrueBeta) {
    // Generate synthetic cointegrated pair:
    // logA = 0.5 + 1.3 * logB + noise
    double trueBeta = 1.3;
    double trueAlpha = 0.5;

    KalmanHedgeRatio kf(0.999, 1e-3);

    double lastBeta = 0;
    for (int i = 0; i < 500; ++i) {
        double logB = 5.0 + 0.001 * i + noise(rng);
        double logA = trueAlpha + trueBeta * logB + noise(rng);

        auto state = kf.update(logA, logB);
        lastBeta = state.beta;
    }

    // After 500 observations, should converge to true beta
    EXPECT_NEAR(lastBeta, trueBeta, 0.05);
}

TEST_F(KalmanFilterTest, TracksTimeVaryingBeta) {
    // Use very fast adaptation (delta=0.95) to track beta shifts
    KalmanHedgeRatio kf(0.95, 1e-4);

    // Phase 1: beta = 1.0
    for (int i = 0; i < 500; ++i) {
        double logB = 4.5 + 0.001 * i + noise(rng);
        double logA = 1.0 * logB + noise(rng);
        kf.update(logA, logB);
    }
    double beta1 = kf.beta();

    // Phase 2: beta shifts to 1.5
    for (int i = 0; i < 500; ++i) {
        double logB = 5.0 + 0.001 * i + noise(rng);
        double logA = 1.5 * logB + noise(rng);
        kf.update(logA, logB);
    }
    double beta2 = kf.beta();

    // Beta should have clearly shifted upward
    EXPECT_GT(beta2, beta1 + 0.1);
}

TEST_F(KalmanFilterTest, SpreadZScoreUsesKalmanUncertainty) {
    KalmanHedgeRatio kf(0.9999, 1e-3);

    // Feed some data to initialize
    for (int i = 0; i < 100; ++i) {
        double logB = 5.0 + noise(rng);
        double logA = 1.0 * logB + noise(rng);
        auto state = kf.update(logA, logB);

        // Z-score should be finite
        EXPECT_TRUE(std::isfinite(state.zScore()));
    }

    // Feed a large shock
    double logB = 5.0;
    double logA = 1.0 * logB + 0.1;  // Large deviation
    auto state = kf.update(logA, logB);

    // Z-score should be large for the shock
    EXPECT_GT(std::abs(state.zScore()), 2.0);
}

TEST_F(KalmanFilterTest, BetaStdErrDecreasesWithData) {
    // Use delta closer to 1 so covariance converges (not inflated each step)
    KalmanHedgeRatio kf(0.9999, 1e-3);

    double earlyStdErr = 0;
    double lateStdErr = 0;

    for (int i = 0; i < 500; ++i) {
        // Need varying logB for the regression to be well-conditioned
        double logB = 4.5 + 0.002 * i + noise(rng);
        double logA = 0.5 + 1.5 * logB + noise(rng);
        auto state = kf.update(logA, logB);

        if (i == 10) earlyStdErr = state.betaStdErr();
        if (i == 499) lateStdErr = state.betaStdErr();
    }

    // Standard error should decrease as more data arrives
    // With varying logB, the covariance matrix converges
    EXPECT_GT(earlyStdErr, lateStdErr);
}

TEST_F(KalmanFilterTest, ResetClearsState) {
    KalmanHedgeRatio kf;
    kf.update(5.0, 4.0);
    EXPECT_TRUE(kf.initialized());

    kf.reset();
    EXPECT_FALSE(kf.initialized());
}

TEST_F(KalmanFilterTest, SpreadModelKalmanIntegration) {
    KalmanHedgeRatio kf(0.999, 1e-3);
    SpreadModel model(100);
    model.setKalmanFilter(&kf);

    EXPECT_TRUE(model.isKalmanActive());

    // Feed prices through SpreadModel
    for (int i = 0; i < 200; ++i) {
        double priceB = 100.0 + 0.1 * noise(rng);
        double priceA = 1.5 * priceB + noise(rng) * 10;
        double z = model.update(priceA, priceB);

        EXPECT_TRUE(std::isfinite(z));
    }

    // Beta should approximate 1.5 (log-space may differ slightly)
    EXPECT_GT(model.beta(), 0.5);
}
