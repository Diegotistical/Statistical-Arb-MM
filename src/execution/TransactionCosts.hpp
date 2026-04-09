#pragma once
/**
 * @file TransactionCosts.hpp
 * @brief Transaction cost model with market impact estimation.
 *
 * Implements a multi-component cost model based on Almgren-Chriss (2001):
 *
 *   TotalCost = FixedCost + SpreadCost + TemporaryImpact + Fee
 *
 * Components:
 *   1. Fixed cost (epsilon): per-share fixed execution cost
 *   2. Spread cost: half the bid-ask spread for crossing orders
 *   3. Temporary impact (eta): price impact proportional to trade rate
 *      TempImpact = eta * sign(q) * |q / V_daily|^alpha
 *   4. Exchange fees: maker/taker fee schedule with rebates
 *
 * The temporary impact term captures the observation that larger orders
 * relative to daily volume cause greater price displacement. The exponent
 * alpha is typically between 0.5 (square-root) and 1.0 (linear).
 *
 * Reference: Almgren & Chriss (2001) "Optimal Execution of Portfolio
 * Transactions"
 */

#include <cmath>
#include <algorithm>
#include <cstdint>

namespace execution {

/**
 * @brief Transaction cost breakdown for a single trade.
 */
struct CostBreakdown {
    double spreadCost = 0.0;       ///< Half-spread crossing cost
    double fixedCost = 0.0;        ///< Per-share fixed cost
    double temporaryImpact = 0.0;  ///< Almgren-Chriss temporary impact
    double exchangeFee = 0.0;      ///< Exchange/venue fee (or rebate)
    double totalCost = 0.0;        ///< Sum of all components

    /**
     * @brief Cost per share.
     */
    [[nodiscard]] double costPerShare(int32_t quantity) const noexcept {
        if (quantity == 0) return 0.0;
        return totalCost / std::abs(quantity);
    }
};

/**
 * @brief Fee schedule (maker/taker model).
 */
struct FeeSchedule {
    double makerFeePerShare = -0.002;   ///< Maker rebate (negative = rebate)
    double takerFeePerShare = 0.003;    ///< Taker fee
    double clearingFeePerShare = 0.0002; ///< Clearing cost
    double secFeeRate = 0.0000278;       ///< SEC fee (sells only, per dollar)
};

/**
 * @brief Transaction cost model with Almgren-Chriss market impact.
 *
 * Usage:
 * @code
 *   TransactionCostModel tcm;
 *   tcm.setDailyVolume(5000000);  // 5M shares/day
 *
 *   auto cost = tcm.estimateCost(
 *       100,      // quantity
 *       150.25,   // price
 *       0.02,     // spread (bid-ask)
 *       true,     // is_maker
 *       true      // is_buy
 *   );
 *
 *   double totalBps = cost.totalCost / (100 * 150.25) * 10000;
 * @endcode
 */
class TransactionCostModel {
public:
    /**
     * @brief Construct with default parameters.
     */
    TransactionCostModel() noexcept = default;

    /**
     * @brief Estimate transaction costs for a trade.
     *
     * @param quantity  Number of shares (unsigned)
     * @param price     Execution price
     * @param spread    Current bid-ask spread
     * @param isMaker   True if passive (resting) order
     * @param isBuy     True if buy, false if sell
     * @return CostBreakdown with itemized costs
     */
    [[nodiscard]] CostBreakdown estimateCost(
        int32_t quantity, double price, double spread,
        bool isMaker, bool isBuy) const noexcept {

        CostBreakdown cost;
        double absQty = static_cast<double>(std::abs(quantity));

        // 1. Spread cost: for aggressive orders, pay half the spread
        //    For passive orders, you capture half the spread (negative cost)
        if (isMaker) {
            cost.spreadCost = -absQty * spread / 2.0;  // Rebate: capture spread
        } else {
            cost.spreadCost = absQty * spread / 2.0;   // Cost: cross the spread
        }

        // 2. Fixed cost per share
        cost.fixedCost = absQty * epsilon_;

        // 3. Temporary impact: eta * |q / V|^alpha * price * q
        if (avgDailyVolume_ > 0) {
            double participationRate = absQty / avgDailyVolume_;
            double impact = eta_ * std::pow(participationRate, alpha_) * price;
            cost.temporaryImpact = impact * absQty;
        }

        // 4. Exchange fees
        if (isMaker) {
            cost.exchangeFee = absQty * fees_.makerFeePerShare;
        } else {
            cost.exchangeFee = absQty * fees_.takerFeePerShare;
        }
        cost.exchangeFee += absQty * fees_.clearingFeePerShare;

        // SEC fee on sells only (per dollar of notional)
        if (!isBuy) {
            cost.exchangeFee += absQty * price * fees_.secFeeRate;
        }

        cost.totalCost = cost.spreadCost + cost.fixedCost
                         + cost.temporaryImpact + cost.exchangeFee;

        return cost;
    }

    // ========================================================================
    // Configuration
    // ========================================================================

    void setDailyVolume(double volume) noexcept { avgDailyVolume_ = volume; }
    void setEpsilon(double eps) noexcept { epsilon_ = eps; }
    void setEta(double eta) noexcept { eta_ = eta; }
    void setAlpha(double alpha) noexcept { alpha_ = alpha; }
    void setFeeSchedule(const FeeSchedule& fees) noexcept { fees_ = fees; }

    [[nodiscard]] double avgDailyVolume() const noexcept { return avgDailyVolume_; }
    [[nodiscard]] const FeeSchedule& fees() const noexcept { return fees_; }

private:
    double epsilon_ = 0.0005;        ///< Fixed cost per share ($)
    double eta_ = 0.1;              ///< Temporary impact coefficient
    double alpha_ = 0.5;            ///< Impact exponent (square-root law)
    double avgDailyVolume_ = 1e6;   ///< Average daily volume (shares)
    FeeSchedule fees_;
};

} // namespace execution
