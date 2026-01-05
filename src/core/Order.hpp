#pragma once
/**
 * @file Order.hpp
 * @brief Core order structure for single stock equity matching engine.
 * 
 * Design principles:
 *   - POD (Plain Old Data) for safe PMR usage and memcpy optimization
 *   - 32-byte alignment for cache efficiency (2 orders per cache line)
 *   - All fields trivially copyable/destructible for monotonic_buffer_resource
 * 
 * Equity-specific features:
 *   - clientId for self-trade prevention (STP)
 *   - TimeInForce: IOC, GTC, FOK
 *   - Price in ticks (not dollars) - use TickNormalizer for conversion
 * 
 * @note OrderId is uint32_t with max 10M orders per session.
 *       External UUIDs must be mapped to internal slot IDs.
 */

#include <cstdint>
#include <type_traits>

// ============================================================================
// Type Aliases - Controlled spaces for HFT safety
// ============================================================================

/** Internal order slot ID (NOT external client order ID) */
using OrderId = uint32_t;

/** Price in ticks (integer). Convert from dollars: price_ticks = price_usd / tick_size */
using Price = int64_t;

/** Quantity in shares (or lots for other markets) */
using Quantity = uint32_t;

/** Client/firm identifier for self-trade prevention */
using ClientId = uint32_t;

// ============================================================================
// Sentinel Values
// ============================================================================

/** Invalid price sentinel - indicates empty best bid/ask */
static constexpr Price PRICE_INVALID = -1;

/** Minimum valid price (0 ticks) */
static constexpr Price PRICE_MIN = 0;

// ============================================================================
// Enums
// ============================================================================

/**
 * @brief Order side: Buy or Sell
 */
enum class OrderSide : uint8_t { 
    Buy = 0,   ///< Bid side
    Sell = 1   ///< Ask/Offer side
};

/**
 * @brief Order type: Limit or Market
 * 
 * @note In production HFT, market orders are rare. Most flow is limit orders.
 */
enum class OrderType : uint8_t { 
    Limit = 0,   ///< Limit order with specified price
    Market = 1   ///< Market order - immediate execution at best available
};

/**
 * @brief Time in force - order lifetime behavior
 * 
 * Standard equity order types:
 *   - GTC: Good-til-cancelled (rests on book)
 *   - IOC: Immediate-or-cancel (fill what you can, cancel rest)
 *   - FOK: Fill-or-kill (full fill or nothing)
 */
enum class TimeInForce : uint8_t {
    GTC = 0,   ///< Good-til-cancelled: rests on book until filled/cancelled
    IOC = 1,   ///< Immediate-or-cancel: partial fill OK, remainder cancelled
    FOK = 2    ///< Fill-or-kill: must fill entirely or reject
};

/**
 * @brief Self-trade prevention action
 * 
 * When two orders from same client would match:
 *   - Cancel Newest: Cancel incoming order (default)
 *   - Cancel Oldest: Cancel resting order
 *   - Cancel Both: Cancel both orders
 */
enum class STPAction : uint8_t {
    CancelNewest = 0,  ///< Cancel the incoming (aggressor) order
    CancelOldest = 1,  ///< Cancel the resting (maker) order
    CancelBoth = 2     ///< Cancel both orders
};

// ============================================================================
// Order Struct
// ============================================================================

/**
 * @brief Core order structure for equity matching engine.
 * 
 * Memory layout (32 bytes):
 *   - id:            4 bytes - Internal slot ID
 *   - clientId:      4 bytes - Client/firm ID for STP
 *   - symbolId:      4 bytes - Instrument identifier
 *   - quantity:      4 bytes - Remaining shares
 *   - price:         8 bytes - Price in ticks
 *   - clientOrderId: 4 bytes - External order ID (truncated)
 *   - side:          1 byte
 *   - type:          1 byte
 *   - tif:           1 byte  - Time in force
 *   - active:        1 byte  - Soft-delete flag
 *   
 * INVARIANTS:
 *   - Must be trivially copyable (for memcpy in ring buffers)
 *   - Must be trivially destructible (for PMR release without destructors)
 *   - Must be standard layout (for predictable memory access)
 */
struct Order {
    OrderId id;              ///< Internal slot ID (0 to MAX_ORDER_ID-1)
    ClientId clientId;       ///< Client/firm ID for self-trade prevention
    int32_t symbolId;        ///< Symbol/instrument identifier
    Quantity quantity;       ///< Remaining quantity in shares
    Price price;             ///< Price in ticks (use TickNormalizer)
    uint32_t clientOrderId;  ///< External client order ID (for response)
    OrderSide side;          ///< Buy or Sell
    OrderType type;          ///< Limit or Market
    TimeInForce tif;         ///< Time in force (GTC/IOC/FOK)
    uint8_t active;          ///< 0=cancelled/filled, 1=active
    // Total: 32 bytes

    /**
     * @brief Default constructor (zero-initialized)
     */
    Order() noexcept = default;

    /**
     * @brief Construct a new order
     * 
     * @param id_           Internal slot ID
     * @param clientOrderId_ External client order ID
     * @param clientId_     Client/firm ID for STP
     * @param symbolId_     Symbol identifier
     * @param side_         Buy or Sell
     * @param type_         Limit or Market
     * @param tif_          Time in force
     * @param price_        Price in ticks
     * @param quantity_     Quantity in shares
     */
    Order(OrderId id_, uint32_t clientOrderId_, ClientId clientId_,
          int32_t symbolId_, OrderSide side_, OrderType type_, 
          TimeInForce tif_, Price price_, Quantity quantity_) noexcept
        : id(id_),
          clientId(clientId_),
          symbolId(symbolId_),
          quantity(quantity_),
          price(price_),
          clientOrderId(clientOrderId_),
          side(side_),
          type(type_),
          tif(tif_),
          active(1) {}

    /**
     * @brief Convenience constructor with default TIF=GTC
     */
    Order(OrderId id_, uint32_t clientOrderId_, int32_t symbolId_, 
          OrderSide side_, OrderType type_, Price price_, Quantity quantity_) noexcept
        : Order(id_, clientOrderId_, 0, symbolId_, side_, type_, 
                TimeInForce::GTC, price_, quantity_) {}

    // ========================================================================
    // Accessors
    // ========================================================================

    /** @return true if order is active (not cancelled/filled) */
    [[nodiscard]] bool isActive() const noexcept { return active != 0; }
    
    /** @return true if this is a buy order */
    [[nodiscard]] bool isBuy() const noexcept { return side == OrderSide::Buy; }
    
    /** @return true if this is an IOC order */
    [[nodiscard]] bool isIOC() const noexcept { return tif == TimeInForce::IOC; }
    
    /** @return true if this is a FOK order */
    [[nodiscard]] bool isFOK() const noexcept { return tif == TimeInForce::FOK; }

    /** Mark order as inactive (cancelled or fully filled) */
    void deactivate() noexcept { active = 0; }
};

// ============================================================================
// Compile-time POD Guarantees
// ============================================================================

static_assert(std::is_trivially_copyable_v<Order>, 
              "Order MUST be trivially copyable for PMR and memcpy safety");

static_assert(std::is_trivially_destructible_v<Order>,
              "Order MUST be trivially destructible for monotonic_buffer_resource");

static_assert(std::is_standard_layout_v<Order>,
              "Order MUST be standard layout for predictable memory layout");

static_assert(sizeof(Order) == 32, 
              "Order should be exactly 32 bytes (2 per cache line)");
