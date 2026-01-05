#pragma once
/**
 * @file ExecutionReport.hpp
 * @brief Order lifecycle tracking and execution analytics.
 * 
 * Features:
 *   - Order state machine (New → PartiallyFilled → Filled/Cancelled)
 *   - Time-to-first-fill metrics
 *   - Time-to-complete-fill metrics
 *   - Queue position tracking
 */

#include <chrono>
#include <cstdint>
#include <vector>

#include "Order.hpp"

// ============================================================================
// Order State Machine
// ============================================================================

/**
 * @brief Order lifecycle states.
 * 
 * State transitions:
 *   New → PartiallyFilled → Filled
 *   New → Cancelled
 *   PartiallyFilled → Cancelled
 *   New → Expired (for time-limited orders)
 */
enum class OrderState : uint8_t {
    New = 0,            ///< Order submitted, not yet filled
    PartiallyFilled,    ///< Order partially executed
    Filled,             ///< Order fully executed
    Cancelled,          ///< Order cancelled by user
    Rejected,           ///< Order rejected (invalid price/qty)
    Expired             ///< Order expired (TIF timeout)
};

/**
 * @brief Convert OrderState to string.
 */
inline const char* orderStateToString(OrderState state) {
    switch (state) {
        case OrderState::New: return "NEW";
        case OrderState::PartiallyFilled: return "PARTIAL";
        case OrderState::Filled: return "FILLED";
        case OrderState::Cancelled: return "CANCELLED";
        case OrderState::Rejected: return "REJECTED";
        case OrderState::Expired: return "EXPIRED";
        default: return "UNKNOWN";
    }
}

// ============================================================================
// Queue Position
// ============================================================================

/**
 * @brief Position of order in price level queue.
 * 
 * Used for execution probability modeling.
 */
struct QueuePosition {
    Quantity shares_ahead = 0;   ///< Total shares ahead in queue
    int32_t orders_ahead = 0;    ///< Number of orders ahead in queue
    Price price = PRICE_INVALID; ///< Price level
    int32_t queue_depth = 0;     ///< Total orders at this level
    
    /**
     * @brief Estimate fill probability based on queue position.
     * 
     * Simple model: P(fill) ∝ 1 / (1 + shares_ahead / avg_trade_size)
     * 
     * @param avgTradeSize Average trade size for the symbol
     * @return Estimated fill probability [0, 1]
     */
    [[nodiscard]] double estimateFillProbability(Quantity avgTradeSize = 100) const noexcept {
        if (avgTradeSize == 0) return 0.0;
        return 1.0 / (1.0 + static_cast<double>(shares_ahead) / avgTradeSize);
    }
};

// ============================================================================
// Execution Report
// ============================================================================

/**
 * @brief Detailed execution report for an order.
 */
struct ExecutionReport {
    OrderId order_id;
    OrderState state = OrderState::New;
    
    // Quantities
    Quantity original_qty = 0;     ///< Original order quantity
    Quantity filled_qty = 0;       ///< Total filled quantity
    Quantity remaining_qty = 0;    ///< Remaining quantity
    
    // Timestamps (nanoseconds since epoch)
    int64_t submit_time = 0;       ///< Time order was submitted
    int64_t first_fill_time = 0;   ///< Time of first fill (0 if no fills)
    int64_t complete_time = 0;     ///< Time order completed (fill/cancel)
    
    // Execution prices
    double avg_fill_price = 0.0;   ///< Volume-weighted average fill price
    Price best_fill_price = 0;     ///< Best fill price achieved
    Price worst_fill_price = 0;    ///< Worst fill price achieved
    
    // Fill breakdown
    int32_t fill_count = 0;        ///< Number of partial fills
    
    // ========================================================================
    // Computed Metrics
    // ========================================================================
    
    /**
     * @brief Time from submit to first fill (nanoseconds).
     */
    [[nodiscard]] int64_t timeToFirstFill() const noexcept {
        if (first_fill_time == 0) return -1;
        return first_fill_time - submit_time;
    }
    
    /**
     * @brief Time from submit to complete (nanoseconds).
     */
    [[nodiscard]] int64_t timeToComplete() const noexcept {
        if (complete_time == 0) return -1;
        return complete_time - submit_time;
    }
    
    /**
     * @brief Fill ratio (0 to 1).
     */
    [[nodiscard]] double fillRatio() const noexcept {
        if (original_qty == 0) return 0.0;
        return static_cast<double>(filled_qty) / original_qty;
    }
    
    /**
     * @brief Check if order is terminal (no more fills possible).
     */
    [[nodiscard]] bool isTerminal() const noexcept {
        return state == OrderState::Filled ||
               state == OrderState::Cancelled ||
               state == OrderState::Rejected ||
               state == OrderState::Expired;
    }
};

// ============================================================================
// Execution Tracker
// ============================================================================

/**
 * @brief Tracks execution reports for all orders.
 */
class ExecutionTracker {
public:
    /**
     * @brief Record new order submission.
     */
    void onOrderSubmit(OrderId id, Quantity qty, int64_t timestamp) {
        if (id >= reports_.size()) {
            reports_.resize(id + 1);
        }
        
        auto& report = reports_[id];
        report.order_id = id;
        report.state = OrderState::New;
        report.original_qty = qty;
        report.remaining_qty = qty;
        report.submit_time = timestamp;
    }
    
    /**
     * @brief Record partial or full fill.
     */
    void onFill(OrderId id, Quantity fillQty, Price fillPrice, int64_t timestamp) {
        if (id >= reports_.size()) return;
        
        auto& report = reports_[id];
        
        // First fill
        if (report.first_fill_time == 0) {
            report.first_fill_time = timestamp;
            report.best_fill_price = fillPrice;
            report.worst_fill_price = fillPrice;
        }
        
        // Update prices
        report.best_fill_price = std::max(report.best_fill_price, fillPrice);
        report.worst_fill_price = std::min(report.worst_fill_price, fillPrice);
        
        // Update VWAP
        double totalValue = report.avg_fill_price * report.filled_qty + 
                           static_cast<double>(fillPrice) * fillQty;
        report.filled_qty += fillQty;
        report.remaining_qty -= fillQty;
        report.avg_fill_price = totalValue / report.filled_qty;
        report.fill_count++;
        
        // Update state
        if (report.remaining_qty == 0) {
            report.state = OrderState::Filled;
            report.complete_time = timestamp;
        } else {
            report.state = OrderState::PartiallyFilled;
        }
    }
    
    /**
     * @brief Record order cancellation.
     */
    void onCancel(OrderId id, int64_t timestamp) {
        if (id >= reports_.size()) return;
        
        auto& report = reports_[id];
        report.state = OrderState::Cancelled;
        report.complete_time = timestamp;
    }
    
    /**
     * @brief Get execution report for order.
     */
    [[nodiscard]] const ExecutionReport* getReport(OrderId id) const {
        if (id >= reports_.size()) return nullptr;
        return &reports_[id];
    }
    
    /**
     * @brief Get all reports.
     */
    [[nodiscard]] const std::vector<ExecutionReport>& reports() const {
        return reports_;
    }
    
    /**
     * @brief Clear all reports.
     */
    void clear() { reports_.clear(); }

private:
    std::vector<ExecutionReport> reports_;
};
