#pragma once
/**
 * @file OrderBook.hpp
 * @brief Ultra-low-latency limit order book for single stock equities.
 * 
 * Architecture:
 *   - Price-indexed arrays for O(1) level access
 *   - Bitmask for O(1) best bid/ask discovery
 *   - PMR allocator for zero-allocation hot path
 *   - Generation counters for safe reset semantics
 * 
 * Complexity:
 *   - addOrder: O(1) amortized
 *   - cancelOrder: O(1)
 *   - getBestBid/Ask: O(1)
 *   - reset: O(1) for memory, O(n) for state clear
 * 
 * Thread safety:
 *   - NOT thread-safe. Use one OrderBook per shard/thread.
 *   - For multi-threaded: use Exchange with per-shard OrderBooks.
 * 
 * Equity-specific:
 *   - Prices are tick indices (use TickNormalizer for conversion)
 *   - MAX_PRICE = 100,000 (covers $0-$1000 at $0.01 tick)
 *   - Lot size validation available via validateOrder()
 * 
 * @see TickNormalizer for price conversion
 * @see MatchingStrategy for order matching
 */

#include <cstdint>
#include <memory_resource>
#include <utility>
#include <vector>

#include "Bitset.hpp"
#include "Order.hpp"
#include "PriceLevel.hpp"

/**
 * @brief Trade execution record.
 * 
 * Generated when an aggressive order matches against a resting order.
 */
struct Trade {
    int32_t symbolId;        ///< Symbol that was traded
    Price price;             ///< Execution price (in ticks)
    Quantity quantity;       ///< Quantity traded
    OrderId makerOrderId;    ///< Resting (passive) order ID
    OrderId takerOrderId;    ///< Aggressor (incoming) order ID
    ClientId makerClientId;  ///< Maker's client ID (for STP tracking)
    ClientId takerClientId;  ///< Taker's client ID (for STP tracking)

    Trade(OrderId maker, OrderId taker, int32_t sym, Price p, Quantity q,
          ClientId makerClient = 0, ClientId takerClient = 0) noexcept
        : symbolId(sym),
          price(p),
          quantity(q),
          makerOrderId(maker),
          takerOrderId(taker),
          makerClientId(makerClient),
          takerClientId(takerClient) {}
};

/**
 * @brief Location of an order in the book for O(1) cancel.
 */
struct OrderLocation {
    Price price = PRICE_INVALID;   ///< Price level (-1 = invalid)
    int32_t index = -1;            ///< Index within price level
    OrderSide side = OrderSide::Buy;  ///< Side for direct lookup
    uint32_t generation = 0;       ///< Book generation (for reset safety)
};

/**
 * @brief Order rejection reason.
 */
enum class RejectReason : uint8_t {
    None = 0,
    InvalidPrice,      ///< Price out of range [0, MAX_PRICE)
    InvalidQuantity,   ///< Quantity is zero
    InvalidOrderId,    ///< Order ID >= MAX_ORDER_ID
    DuplicateOrderId,  ///< Order ID already active
    BookFull           ///< PMR buffer exhausted
};

/**
 * @brief Ultra-low-latency limit order book.
 * 
 * Designed for single stock equity trading with:
 *   - Sub-microsecond add/cancel operations
 *   - Zero allocations on hot path (PMR)
 *   - O(1) best bid/ask via bitmask
 *   - Generation-based reset safety
 * 
 * Usage:
 * @code
 *   OrderBook book;
 *   Order order(0, 12345, 0, OrderSide::Buy, OrderType::Limit, 15025, 100);
 *   if (book.addOrder(order)) {
 *       // Order added successfully
 *   }
 * @endcode
 */
class OrderBook {
public:
    // ========================================================================
    // Constants
    // ========================================================================
    
    /** 
     * @brief Maximum price in ticks.
     * 
     * At $0.01 tick: covers $0-$1000
     * Increase for higher-priced stocks or smaller tick sizes.
     */
    static constexpr int MAX_PRICE = 100000;
    
    /** Default PMR buffer size: 512MB */
    static constexpr size_t DEFAULT_BUFFER_SIZE = 512 * 1024 * 1024;
    
    /** Maximum order ID (controlled ID space) */
    static constexpr OrderId MAX_ORDER_ID = 10'000'000;

    // ========================================================================
    // Construction
    // ========================================================================

    /**
     * @brief Construct order book with specified buffer size.
     * @param bufferSize Size of PMR buffer in bytes
     */
    explicit OrderBook(size_t bufferSize = DEFAULT_BUFFER_SIZE);
    
    ~OrderBook() = default;

    // No copying - resource management
    OrderBook(const OrderBook&) = delete;
    OrderBook& operator=(const OrderBook&) = delete;
    OrderBook(OrderBook&&) = default;
    OrderBook& operator=(OrderBook&&) = default;

    // ========================================================================
    // Core Operations
    // ========================================================================
    
    /**
     * @brief Add order to book.
     * 
     * @param order Order to add (must have valid id, price, quantity)
     * @return true if order was added, false if rejected
     * 
     * @note O(1) amortized. Does NOT match - use MatchingStrategy for that.
     */
    [[nodiscard]] bool addOrder(const Order& order) noexcept;
    
    /**
     * @brief Add order with rejection reason output.
     * 
     * @param order Order to add
     * @param[out] reason Rejection reason if failed
     * @return true if order was added
     */
    [[nodiscard]] bool addOrder(const Order& order, RejectReason& reason) noexcept;
    
    /**
     * @brief Cancel order by ID.
     * 
     * @param orderId Order ID to cancel
     * @return true if order was found and cancelled
     * 
     * @note O(1). Safe to call after reset (generation check).
     */
    [[nodiscard]] bool cancelOrder(OrderId orderId) noexcept;
    
    /**
     * @brief Modify order quantity (reduce only).
     * 
     * @param orderId Order ID to modify
     * @param newQuantity New quantity (must be less than current)
     * @return true if order was modified
     */
    [[nodiscard]] bool modifyOrder(OrderId orderId, Quantity newQuantity) noexcept;
    
    /**
     * @brief Reset entire book.
     * 
     * Invalidates ALL order IDs via generation counter.
     * Releases PMR memory for reuse.
     * 
     * @note O(n) for state clear, but O(1) for memory.
     */
    void reset() noexcept;
    
    /**
     * @brief Print book state for debugging.
     */
    void printBook() const;

    // ========================================================================
    // Accessors
    // ========================================================================
    
    /** @brief Get bid levels (mutable). */
    [[nodiscard]] std::vector<PriceLevel>& getBids() noexcept { return bids_; }
    
    /** @brief Get ask levels (mutable). */
    [[nodiscard]] std::vector<PriceLevel>& getAsks() noexcept { return asks_; }
    
    /** @brief Get bid levels (const). */
    [[nodiscard]] const std::vector<PriceLevel>& getBids() const noexcept { return bids_; }
    
    /** @brief Get ask levels (const). */
    [[nodiscard]] const std::vector<PriceLevel>& getAsks() const noexcept { return asks_; }

    /** @brief Get bid bitmask (mutable). */
    [[nodiscard]] PriceBitset& getBidMask() noexcept { return bidMask_; }
    
    /** @brief Get ask bitmask (mutable). */
    [[nodiscard]] PriceBitset& getAskMask() noexcept { return askMask_; }
    
    /** @brief Get bid bitmask (const). */
    [[nodiscard]] const PriceBitset& getBidMask() const noexcept { return bidMask_; }
    
    /** @brief Get ask bitmask (const). */
    [[nodiscard]] const PriceBitset& getAskMask() const noexcept { return askMask_; }

    /**
     * @brief Get price level by price and side.
     */
    [[nodiscard]] const PriceLevel& getLevel(Price price, OrderSide side) const noexcept {
        return (side == OrderSide::Buy) ? bids_[price] : asks_[price];
    }
    
    /**
     * @brief Get price level by price and side (mutable).
     */
    [[nodiscard]] PriceLevel& getLevelMutable(Price price, OrderSide side) noexcept {
        return (side == OrderSide::Buy) ? bids_[price] : asks_[price];
    }

    /**
     * @brief Get best bid price.
     * @return Best bid in ticks, or PRICE_INVALID if no bids
     */
    [[nodiscard]] Price getBestBid() const noexcept { return bestBid_; }
    
    /**
     * @brief Get best ask price.
     * @return Best ask in ticks, or PRICE_INVALID if no asks
     */
    [[nodiscard]] Price getBestAsk() const noexcept { return bestAsk_; }
    
    /**
     * @brief Check if book has any bids.
     */
    [[nodiscard]] bool hasBids() const noexcept { return bestBid_ != PRICE_INVALID; }
    
    /**
     * @brief Check if book has any asks.
     */
    [[nodiscard]] bool hasAsks() const noexcept { return bestAsk_ != PRICE_INVALID; }

    /**
     * @brief Get current generation (for external validation).
     */
    [[nodiscard]] uint32_t generation() const noexcept { return generation_; }

    /**
     * @brief Get order location by ID.
     * @return OrderLocation (check generation for validity)
     */
    [[nodiscard]] const OrderLocation& getOrderLocation(OrderId orderId) const noexcept {
        static const OrderLocation invalid{};
        if (orderId >= MAX_ORDER_ID) return invalid;
        return idToLocation_[orderId];
    }

    /**
     * @brief Get queue position for an order.
     * 
     * @param orderId Order ID to query
     * @return Shares and orders ahead in queue
     */
    [[nodiscard]] std::pair<Quantity, int32_t> getQueuePosition(OrderId orderId) const noexcept {
        if (orderId >= MAX_ORDER_ID) return {0, -1};
        
        const auto& loc = idToLocation_[orderId];
        if (loc.generation != generation_ || loc.price == PRICE_INVALID) {
            return {0, -1};
        }
        
        const auto& levels = (loc.side == OrderSide::Buy) ? bids_ : asks_;
        const auto& level = levels[loc.price];
        
        Quantity sharesAhead = 0;
        int32_t ordersAhead = 0;
        
        for (size_t i = level.headIndex; i < static_cast<size_t>(loc.index); ++i) {
            if (level.orders[i].isActive()) {
                sharesAhead += level.orders[i].quantity;
                ordersAhead++;
            }
        }
        
        return {sharesAhead, ordersAhead};
    }

private:
    void updateBestBidAfterRemoval(Price removedPrice) noexcept;
    void updateBestAskAfterRemoval(Price removedPrice) noexcept;

    std::vector<PriceLevel> bids_;    ///< Bid levels indexed by price
    std::vector<PriceLevel> asks_;    ///< Ask levels indexed by price
    PriceBitset bidMask_;             ///< Bitmask of active bid levels
    PriceBitset askMask_;             ///< Bitmask of active ask levels
    Price bestBid_ = PRICE_INVALID;   ///< Cached best bid
    Price bestAsk_ = PRICE_INVALID;   ///< Cached best ask
    uint32_t generation_ = 0;         ///< Generation counter for reset safety
    std::vector<OrderLocation> idToLocation_;  ///< Order ID to location map
    std::vector<std::byte> buffer_;   ///< PMR backing buffer
    std::pmr::monotonic_buffer_resource pool_;  ///< PMR allocator

    friend class StandardMatchingStrategy;
};
