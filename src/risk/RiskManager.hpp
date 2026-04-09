#pragma once
/**
 * @file RiskManager.hpp
 * @brief Production-grade risk management with zero-allocation hot-path checks.
 *
 * Implements the risk controls expected in any real market-making system:
 *   - Per-symbol and portfolio position limits (hard kills)
 *   - Real-time PnL tracking (mark-to-mid)
 *   - Maximum drawdown circuit breaker
 *   - Fat-finger protection (max order size, max notional, rate limiting)
 *   - Kill switch (atomic, thread-safe)
 *
 * Design principles:
 *   - All state in fixed-size arrays indexed by symbolId (no hash maps)
 *   - preTradeCheck() is const noexcept with zero allocation
 *   - Returns typed rejection reasons (not bool) for audit logging
 *   - Kill switch uses std::atomic for thread-safe emergency shutdown
 *
 * This module sits on the critical path between signal generation and
 * order submission. Every quote passes through preTradeCheck() before
 * reaching the execution layer.
 */

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <algorithm>

#include "../core/Order.hpp"
#include "../core/Timestamp.hpp"

namespace risk {

/// Maximum number of tradeable symbols (fixed at compile time for zero-allocation)
inline constexpr int32_t MAX_SYMBOLS = 64;

/// Maximum orders per second before rate limiting triggers
inline constexpr int32_t DEFAULT_MAX_ORDERS_PER_SECOND = 1000;

/**
 * @brief Result of a pre-trade risk check.
 *
 * Typed rejection reasons enable audit logging and debugging.
 * A production system would log every rejection with the reason.
 */
enum class RiskCheckResult : uint8_t {
    Passed = 0,
    KillSwitchActive,       ///< Global emergency shutdown
    SymbolPositionLimit,    ///< Per-symbol position limit breached
    PortfolioPositionLimit, ///< Portfolio-wide position limit breached
    SymbolLossLimit,        ///< Per-symbol loss limit breached
    PortfolioLossLimit,     ///< Portfolio-wide loss limit breached
    MaxDrawdownBreached,    ///< Maximum drawdown circuit breaker
    MaxOrderSize,           ///< Single order too large (fat finger)
    MaxNotional,            ///< Single order notional too large
    RateLimitExceeded,      ///< Too many orders per second
    InvalidSymbol           ///< Symbol ID out of range
};

/**
 * @brief Per-symbol risk state.
 * All POD, cache-line friendly layout.
 */
struct SymbolRisk {
    int32_t position = 0;       ///< Current net position (signed)
    double realizedPnL = 0.0;   ///< Cumulative realized PnL
    double unrealizedPnL = 0.0; ///< Mark-to-market unrealized PnL
    double avgCost = 0.0;       ///< Volume-weighted average entry price
    double lastMid = 0.0;       ///< Last known mid price
    int32_t orderCount = 0;     ///< Orders in current second window
    int64_t windowStartNs = 0;  ///< Start of current rate limit window
};

/**
 * @brief Risk management configuration.
 */
struct RiskConfig {
    // Position limits
    int32_t maxPositionPerSymbol = 5000;    ///< Max |position| per symbol
    int32_t maxPortfolioPosition = 20000;   ///< Max sum(|position|) across all

    // Loss limits
    double maxLossPerSymbol = -50000.0;     ///< Max loss per symbol before halt
    double maxPortfolioLoss = -200000.0;    ///< Max portfolio loss before halt
    double maxDrawdown = 100000.0;          ///< Max drawdown before circuit breaker

    // Fat finger
    Quantity maxOrderSize = 10000;          ///< Max shares per order
    double maxNotional = 1000000.0;         ///< Max notional per order (USD)

    // Rate limiting
    int32_t maxOrdersPerSecond = DEFAULT_MAX_ORDERS_PER_SECOND;

    // Tick size for notional calculation
    double tickSize = 0.01;
};

/**
 * @brief Portfolio-level risk metrics (read-only snapshot).
 */
struct RiskSnapshot {
    double totalPnL = 0.0;
    double totalRealizedPnL = 0.0;
    double totalUnrealizedPnL = 0.0;
    double peakPnL = 0.0;
    double currentDrawdown = 0.0;
    double maxDrawdownSeen = 0.0;
    int32_t totalAbsPosition = 0;
    int32_t activeSymbols = 0;
    bool killSwitchActive = false;
};

/**
 * @brief Zero-allocation risk manager for the trading hot path.
 *
 * Usage:
 * @code
 *   RiskManager rm;
 *   rm.config().maxPositionPerSymbol = 1000;
 *
 *   // On every quote:
 *   auto check = rm.preTradeCheck(symbolId, OrderSide::Buy, 100, 15025);
 *   if (check != RiskCheckResult::Passed) { // reject order }
 *
 *   // On every fill:
 *   rm.onFill(symbolId, OrderSide::Buy, 100, 150.25);
 *
 *   // On every market data update:
 *   rm.updateMark(symbolId, 150.30);
 * @endcode
 */
class RiskManager {
public:
    RiskManager() noexcept : killSwitch_(false), peakPnL_(0.0), maxDrawdownSeen_(0.0) {
        symbols_.fill(SymbolRisk{});
    }

    // ========================================================================
    // Configuration
    // ========================================================================

    [[nodiscard]] RiskConfig& config() noexcept { return config_; }
    [[nodiscard]] const RiskConfig& config() const noexcept { return config_; }

    // ========================================================================
    // Pre-Trade Risk Check (HOT PATH - zero allocation, const noexcept)
    // ========================================================================

    /**
     * @brief Check if an order passes all risk controls.
     *
     * This is the most latency-critical method. Called for every quote
     * before submission. Must complete in <100ns.
     *
     * @param symbolId  Symbol identifier (0 to MAX_SYMBOLS-1)
     * @param side      Buy or Sell
     * @param qty       Order quantity
     * @param priceTicks Price in ticks (for notional calculation)
     * @param currentTimeNs Current timestamp for rate limiting
     * @return RiskCheckResult::Passed if order is safe to submit
     */
    [[nodiscard]] RiskCheckResult preTradeCheck(
        int32_t symbolId, OrderSide side, Quantity qty, Price priceTicks,
        int64_t currentTimeNs = 0) const noexcept {

        // 1. Kill switch (atomic read, ~1ns)
        if (killSwitch_.load(std::memory_order_relaxed)) [[unlikely]] {
            return RiskCheckResult::KillSwitchActive;
        }

        // 2. Symbol validation
        if (symbolId < 0 || symbolId >= MAX_SYMBOLS) [[unlikely]] {
            return RiskCheckResult::InvalidSymbol;
        }

        const auto& sym = symbols_[static_cast<size_t>(symbolId)];

        // 3. Fat finger: max order size
        if (qty > config_.maxOrderSize) [[unlikely]] {
            return RiskCheckResult::MaxOrderSize;
        }

        // 4. Fat finger: max notional
        double notional = static_cast<double>(priceTicks) * config_.tickSize
                          * static_cast<double>(qty);
        if (notional > config_.maxNotional) [[unlikely]] {
            return RiskCheckResult::MaxNotional;
        }

        // 5. Per-symbol position limit
        int32_t projectedPosition = sym.position;
        if (side == OrderSide::Buy) {
            projectedPosition += static_cast<int32_t>(qty);
        } else {
            projectedPosition -= static_cast<int32_t>(qty);
        }
        if (std::abs(projectedPosition) > config_.maxPositionPerSymbol) [[unlikely]] {
            return RiskCheckResult::SymbolPositionLimit;
        }

        // 6. Portfolio position limit
        int32_t totalAbs = computeTotalAbsPosition_();
        // Adjust for this order: remove old symbol contribution, add new
        totalAbs -= std::abs(sym.position);
        totalAbs += std::abs(projectedPosition);
        if (totalAbs > config_.maxPortfolioPosition) [[unlikely]] {
            return RiskCheckResult::PortfolioPositionLimit;
        }

        // 7. Per-symbol loss limit
        double symbolPnL = sym.realizedPnL + sym.unrealizedPnL;
        if (symbolPnL < config_.maxLossPerSymbol) [[unlikely]] {
            return RiskCheckResult::SymbolLossLimit;
        }

        // 8. Portfolio loss limit
        double portfolioPnL = computeTotalPnL_();
        if (portfolioPnL < config_.maxPortfolioLoss) [[unlikely]] {
            return RiskCheckResult::PortfolioLossLimit;
        }

        // 9. Drawdown circuit breaker
        if (maxDrawdownSeen_ > config_.maxDrawdown) [[unlikely]] {
            return RiskCheckResult::MaxDrawdownBreached;
        }

        // 10. Rate limiting
        if (currentTimeNs > 0) {
            int64_t windowNs = 1'000'000'000LL;  // 1 second
            if (currentTimeNs - sym.windowStartNs < windowNs) {
                if (sym.orderCount >= config_.maxOrdersPerSecond) [[unlikely]] {
                    return RiskCheckResult::RateLimitExceeded;
                }
            }
        }

        return RiskCheckResult::Passed;
    }

    // ========================================================================
    // State Updates (called on fills and market data)
    // ========================================================================

    /**
     * @brief Update state after a fill.
     */
    void onFill(int32_t symbolId, OrderSide side, Quantity qty,
                double fillPrice) noexcept {
        if (symbolId < 0 || symbolId >= MAX_SYMBOLS) return;
        auto& sym = symbols_[static_cast<size_t>(symbolId)];

        int32_t signedQty = (side == OrderSide::Buy)
                            ? static_cast<int32_t>(qty)
                            : -static_cast<int32_t>(qty);

        int32_t oldPosition = sym.position;
        sym.position += signedQty;

        // Compute realized PnL on position reduction
        bool isReducing = (oldPosition > 0 && signedQty < 0) ||
                          (oldPosition < 0 && signedQty > 0);
        if (isReducing) {
            int32_t closingQty = std::min(std::abs(oldPosition), std::abs(signedQty));
            double realizedPnL = closingQty * (fillPrice - sym.avgCost);
            if (oldPosition < 0) realizedPnL = -realizedPnL;
            sym.realizedPnL += realizedPnL;
        }

        // Update average cost
        if (sym.position != 0) {
            bool isAdding = (oldPosition >= 0 && signedQty > 0) ||
                            (oldPosition <= 0 && signedQty < 0);
            if (isAdding) {
                sym.avgCost = (sym.avgCost * std::abs(oldPosition)
                               + fillPrice * std::abs(signedQty))
                              / std::abs(sym.position);
            }
            // If flipping sign, new avgCost = fillPrice for the new direction
            if ((oldPosition > 0 && sym.position < 0) ||
                (oldPosition < 0 && sym.position > 0)) {
                sym.avgCost = fillPrice;
            }
        } else {
            sym.avgCost = 0.0;
        }

        // Update unrealized after position change
        if (sym.lastMid > 0.0) {
            sym.unrealizedPnL = sym.position * (sym.lastMid - sym.avgCost);
        }

        updateDrawdown_();
    }

    /**
     * @brief Update mark-to-market for a symbol.
     * Called on every market data tick.
     */
    void updateMark(int32_t symbolId, double midPrice) noexcept {
        if (symbolId < 0 || symbolId >= MAX_SYMBOLS) return;
        auto& sym = symbols_[static_cast<size_t>(symbolId)];

        sym.lastMid = midPrice;
        sym.unrealizedPnL = sym.position * (midPrice - sym.avgCost);

        updateDrawdown_();
    }

    /**
     * @brief Record an order submission for rate limiting.
     */
    void recordOrder(int32_t symbolId, int64_t currentTimeNs) noexcept {
        if (symbolId < 0 || symbolId >= MAX_SYMBOLS) return;
        auto& sym = symbols_[static_cast<size_t>(symbolId)];

        int64_t windowNs = 1'000'000'000LL;
        if (currentTimeNs - sym.windowStartNs >= windowNs) {
            sym.windowStartNs = currentTimeNs;
            sym.orderCount = 1;
        } else {
            sym.orderCount++;
        }
    }

    // ========================================================================
    // Kill Switch
    // ========================================================================

    /**
     * @brief Activate emergency kill switch. Thread-safe.
     * All subsequent preTradeCheck() calls will return KillSwitchActive.
     */
    void activateKillSwitch() noexcept {
        killSwitch_.store(true, std::memory_order_release);
    }

    /**
     * @brief Deactivate kill switch. Requires manual intervention.
     */
    void deactivateKillSwitch() noexcept {
        killSwitch_.store(false, std::memory_order_release);
    }

    /**
     * @brief Check if kill switch is active.
     */
    [[nodiscard]] bool isKillSwitchActive() const noexcept {
        return killSwitch_.load(std::memory_order_relaxed);
    }

    // ========================================================================
    // Queries
    // ========================================================================

    /**
     * @brief Get a snapshot of portfolio-level risk metrics.
     */
    [[nodiscard]] RiskSnapshot snapshot() const noexcept {
        RiskSnapshot snap;
        snap.totalRealizedPnL = 0;
        snap.totalUnrealizedPnL = 0;
        snap.totalAbsPosition = 0;
        snap.activeSymbols = 0;

        for (int32_t i = 0; i < MAX_SYMBOLS; ++i) {
            const auto& sym = symbols_[static_cast<size_t>(i)];
            snap.totalRealizedPnL += sym.realizedPnL;
            snap.totalUnrealizedPnL += sym.unrealizedPnL;
            snap.totalAbsPosition += std::abs(sym.position);
            if (sym.position != 0) snap.activeSymbols++;
        }

        snap.totalPnL = snap.totalRealizedPnL + snap.totalUnrealizedPnL;
        snap.peakPnL = peakPnL_;
        snap.currentDrawdown = peakPnL_ - snap.totalPnL;
        snap.maxDrawdownSeen = maxDrawdownSeen_;
        snap.killSwitchActive = killSwitch_.load(std::memory_order_relaxed);

        return snap;
    }

    /**
     * @brief Get per-symbol risk state (read-only).
     */
    [[nodiscard]] const SymbolRisk& symbolRisk(int32_t symbolId) const noexcept {
        static const SymbolRisk empty{};
        if (symbolId < 0 || symbolId >= MAX_SYMBOLS) return empty;
        return symbols_[static_cast<size_t>(symbolId)];
    }

    /**
     * @brief Reset all risk state.
     */
    void reset() noexcept {
        symbols_.fill(SymbolRisk{});
        killSwitch_.store(false, std::memory_order_relaxed);
        peakPnL_ = 0.0;
        maxDrawdownSeen_ = 0.0;
    }

    /**
     * @brief Get human-readable rejection reason.
     */
    [[nodiscard]] static const char* rejectionString(RiskCheckResult r) noexcept {
        switch (r) {
            case RiskCheckResult::Passed:                return "PASSED";
            case RiskCheckResult::KillSwitchActive:      return "KILL_SWITCH";
            case RiskCheckResult::SymbolPositionLimit:    return "SYMBOL_POS_LIMIT";
            case RiskCheckResult::PortfolioPositionLimit: return "PORTFOLIO_POS_LIMIT";
            case RiskCheckResult::SymbolLossLimit:        return "SYMBOL_LOSS_LIMIT";
            case RiskCheckResult::PortfolioLossLimit:     return "PORTFOLIO_LOSS_LIMIT";
            case RiskCheckResult::MaxDrawdownBreached:    return "MAX_DRAWDOWN";
            case RiskCheckResult::MaxOrderSize:           return "MAX_ORDER_SIZE";
            case RiskCheckResult::MaxNotional:            return "MAX_NOTIONAL";
            case RiskCheckResult::RateLimitExceeded:      return "RATE_LIMIT";
            case RiskCheckResult::InvalidSymbol:          return "INVALID_SYMBOL";
            default:                                     return "UNKNOWN";
        }
    }

private:
    RiskConfig config_;
    std::array<SymbolRisk, MAX_SYMBOLS> symbols_;
    std::atomic<bool> killSwitch_;
    double peakPnL_;
    double maxDrawdownSeen_;

    [[nodiscard]] int32_t computeTotalAbsPosition_() const noexcept {
        int32_t total = 0;
        for (const auto& sym : symbols_) {
            total += std::abs(sym.position);
        }
        return total;
    }

    [[nodiscard]] double computeTotalPnL_() const noexcept {
        double total = 0;
        for (const auto& sym : symbols_) {
            total += sym.realizedPnL + sym.unrealizedPnL;
        }
        return total;
    }

    void updateDrawdown_() noexcept {
        double totalPnL = computeTotalPnL_();
        if (totalPnL > peakPnL_) {
            peakPnL_ = totalPnL;
        }
        double dd = peakPnL_ - totalPnL;
        if (dd > maxDrawdownSeen_) {
            maxDrawdownSeen_ = dd;
        }

        // Auto-activate kill switch on max drawdown breach
        if (maxDrawdownSeen_ > config_.maxDrawdown) {
            killSwitch_.store(true, std::memory_order_release);
        }
    }
};

} // namespace risk
