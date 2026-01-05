#pragma once
/**
 * @file PriceLevel.hpp
 * @brief FIFO queue of orders at a single price level.
 * 
 * Design:
 *   - PMR-backed vector for zero-allocation hot path
 *   - Lazy deletion via headIndex (avoids vector shifts)
 *   - activeCount tracks number of non-cancelled orders
 * 
 * Memory model:
 *   - Orders are POD, so monotonic_buffer_resource::release() is safe
 *   - No destructors called on individual orders
 * 
 * @note This is an internal structure. Use OrderBook for public API.
 */

#include <memory_resource>
#include <vector>

#include "Order.hpp"

/**
 * @brief FIFO queue of orders at a single price level.
 * 
 * Orders are matched in FIFO order (price-time priority).
 * Cancelled orders are lazily deleted by marking inactive.
 * 
 * INVARIANTS:
 *   - activeCount == number of orders where isActive() == true
 *   - headIndex <= orders.size()
 *   - All orders at indices < headIndex are inactive
 */
struct PriceLevel {
    /** Orders at this price level (FIFO order) */
    std::pmr::vector<Order> orders;
    
    /** Count of active (non-cancelled) orders */
    int32_t activeCount = 0;
    
    /** Index of first potentially active order (lazy deletion optimization) */
    size_t headIndex = 0;

    /**
     * @brief Construct with PMR allocator.
     * @param mr Memory resource (typically from OrderBook's pool)
     */
    explicit PriceLevel(std::pmr::memory_resource* mr = std::pmr::get_default_resource())
        : orders(mr) {}

    // Move only - no copying for performance
    PriceLevel(PriceLevel&&) = default;
    PriceLevel& operator=(PriceLevel&&) = default;
    PriceLevel(const PriceLevel&) = delete;
    PriceLevel& operator=(const PriceLevel&) = delete;

    /**
     * @brief Reset level state (does NOT deallocate - handled by PMR).
     */
    void reset() noexcept {
        orders.clear();
        activeCount = 0;
        headIndex = 0;
    }

    /**
     * @brief Check if level has any active orders.
     */
    [[nodiscard]] bool isEmpty() const noexcept {
        return activeCount == 0;
    }

    /**
     * @brief Get total volume at this price level.
     */
    [[nodiscard]] Quantity totalVolume() const noexcept {
        Quantity vol = 0;
        for (size_t i = headIndex; i < orders.size(); ++i) {
            if (orders[i].isActive()) {
                vol += orders[i].quantity;
            }
        }
        return vol;
    }
};
