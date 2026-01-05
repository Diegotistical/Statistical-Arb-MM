#pragma once
/**
 * @file TickNormalizer.hpp
 * @brief Converts real equity prices to tick indices for OrderBook.
 * 
 * US equity tick sizes:
 *   - Stocks >= $1.00: tick = $0.01 (100 ticks per dollar)
 *   - Stocks < $1.00:  tick = $0.0001 (sub-penny, less common)
 * 
 * Example for AAPL @ $150.25 with $0.01 tick:
 *   tick_index = 15025
 *   price_usd = 150.25
 * 
 * The OrderBook uses tick indices [0, MAX_PRICE) for O(1) access.
 * This class handles the conversion between human-readable prices
 * and internal tick representation.
 * 
 * @note For production, tick size may vary by exchange/symbol.
 */

#include <cstdint>
#include <cmath>

#include "Order.hpp"

/**
 * @brief Converts between USD prices and tick indices.
 * 
 * Thread-safe: All methods are const and use only local state.
 * 
 * Usage:
 * @code
 *   TickNormalizer norm(0.01);  // $0.01 tick size
 *   Price ticks = norm.toTicks(150.25);  // -> 15025
 *   double usd = norm.toPrice(15025);    // -> 150.25
 * @endcode
 */
class TickNormalizer {
public:
    /**
     * @brief Construct with specified tick size.
     * @param tickSize Tick size in dollars (e.g., 0.01 for US equities)
     */
    explicit constexpr TickNormalizer(double tickSize = 0.01) noexcept
        : tickSize_(tickSize),
          invTickSize_(1.0 / tickSize) {}

    /**
     * @brief Convert USD price to tick index.
     * 
     * @param priceUsd Price in dollars (e.g., 150.25)
     * @return Tick index (e.g., 15025)
     * 
     * @note Uses rounding to handle floating-point precision.
     */
    [[nodiscard]] Price toTicks(double priceUsd) const noexcept {
        return static_cast<Price>(std::round(priceUsd * invTickSize_));
    }

    /**
     * @brief Convert tick index to USD price.
     * 
     * @param ticks Tick index (e.g., 15025)
     * @return Price in dollars (e.g., 150.25)
     */
    [[nodiscard]] double toPrice(Price ticks) const noexcept {
        return static_cast<double>(ticks) * tickSize_;
    }

    /**
     * @brief Get the tick size in dollars.
     */
    [[nodiscard]] constexpr double tickSize() const noexcept { 
        return tickSize_; 
    }

    /**
     * @brief Round price to nearest valid tick.
     * 
     * @param priceUsd Raw price in dollars
     * @return Price rounded to nearest tick
     */
    [[nodiscard]] double roundToTick(double priceUsd) const noexcept {
        return toPrice(toTicks(priceUsd));
    }

    /**
     * @brief Validate price is within OrderBook range.
     * 
     * @param ticks Tick index
     * @param maxPrice Maximum tick index (OrderBook::MAX_PRICE)
     * @return true if price is valid
     */
    [[nodiscard]] static constexpr bool isValidTick(Price ticks, int maxPrice) noexcept {
        return ticks >= 0 && ticks < maxPrice;
    }

private:
    double tickSize_;     ///< Tick size in dollars
    double invTickSize_;  ///< 1/tickSize for fast multiplication
};

/**
 * @brief Standard US equity tick normalizer ($0.01 tick)
 */
inline constexpr TickNormalizer US_EQUITY_TICKS{0.01};
