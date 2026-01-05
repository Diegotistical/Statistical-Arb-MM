/**
 * @file OrderBook.cpp
 * @brief Implementation of ultra-low-latency order book.
 */

#include "OrderBook.hpp"

#include <algorithm>
#include <iostream>

// ============================================================================
// Constructor
// ============================================================================

OrderBook::OrderBook(size_t bufferSize)
    : buffer_(bufferSize),
      pool_(buffer_.data(), buffer_.size(), std::pmr::new_delete_resource()),
      bidMask_(MAX_PRICE),
      askMask_(MAX_PRICE),
      generation_(0) {
    
    // Fixed-size ID lookup table - NO dynamic resizing for safety
    idToLocation_.resize(MAX_ORDER_ID);
    
    // Pre-allocate all price levels
    bids_.reserve(MAX_PRICE);
    asks_.reserve(MAX_PRICE);
    
    for (int i = 0; i < MAX_PRICE; ++i) {
        bids_.emplace_back(&pool_);
        asks_.emplace_back(&pool_);
    }
}

// ============================================================================
// addOrder - O(1) order insertion
// ============================================================================

bool OrderBook::addOrder(const Order& order) noexcept {
    RejectReason reason;
    return addOrder(order, reason);
}

bool OrderBook::addOrder(const Order& order, RejectReason& reason) noexcept {
    // Validate price bounds
    if (order.price < 0 || order.price >= MAX_PRICE) {
        reason = RejectReason::InvalidPrice;
        return false;
    }

    // Validate quantity
    if (order.quantity == 0) {
        reason = RejectReason::InvalidQuantity;
        return false;
    }

    // Validate ID bounds
    if (order.id >= MAX_ORDER_ID) {
        reason = RejectReason::InvalidOrderId;
        return false;
    }

    // Check for duplicate (only if same generation and active)
    const auto& existingLoc = idToLocation_[order.id];
    if (existingLoc.generation == generation_ && existingLoc.price != PRICE_INVALID) {
        reason = RejectReason::DuplicateOrderId;
        return false;
    }

    const bool isBid = (order.side == OrderSide::Buy);
    auto& levels = isBid ? bids_ : asks_;
    auto& level = levels[order.price];

    // Store order location with current generation and side
    idToLocation_[order.id] = {
        order.price,
        static_cast<int32_t>(level.orders.size()),
        order.side,
        generation_
    };

    // Add order to level
    level.orders.push_back(order);
    level.activeCount++;

    // Update bitmask and best price
    if (isBid) {
        bidMask_.set(static_cast<size_t>(order.price));
        if (bestBid_ == PRICE_INVALID || order.price > bestBid_) {
            bestBid_ = order.price;
        }
    } else {
        askMask_.set(static_cast<size_t>(order.price));
        if (bestAsk_ == PRICE_INVALID || order.price < bestAsk_) {
            bestAsk_ = order.price;
        }
    }

    reason = RejectReason::None;
    return true;
}

// ============================================================================
// cancelOrder - O(1) order cancellation
// ============================================================================

bool OrderBook::cancelOrder(OrderId orderId) noexcept {
    if (orderId >= MAX_ORDER_ID) {
        return false;
    }

    const OrderLocation& loc = idToLocation_[orderId];
    
    // Check generation - if mismatched, order was from previous session
    if (loc.generation != generation_) {
        return false;
    }
    
    if (loc.price == PRICE_INVALID || loc.index < 0) {
        return false;
    }

    // Direct lookup using stored side
    auto& levels = (loc.side == OrderSide::Buy) ? bids_ : asks_;
    
    if (loc.price >= MAX_PRICE) {
        return false;
    }
    
    auto& level = levels[loc.price];
    
    if (static_cast<size_t>(loc.index) >= level.orders.size()) {
        return false;
    }

    Order& order = level.orders[loc.index];
    
    if (order.id != orderId || !order.isActive()) {
        return false;
    }

    // Mark as inactive
    order.deactivate();
    level.activeCount--;

    // Update bitmask if level is now empty
    if (level.activeCount == 0) {
        if (loc.side == OrderSide::Buy) {
            bidMask_.clear(static_cast<size_t>(loc.price));
            if (loc.price == bestBid_) {
                updateBestBidAfterRemoval(loc.price);
            }
        } else {
            askMask_.clear(static_cast<size_t>(loc.price));
            if (loc.price == bestAsk_) {
                updateBestAskAfterRemoval(loc.price);
            }
        }
    }

    // Invalidate location
    idToLocation_[orderId].price = PRICE_INVALID;
    return true;
}

// ============================================================================
// modifyOrder - Reduce quantity only
// ============================================================================

bool OrderBook::modifyOrder(OrderId orderId, Quantity newQuantity) noexcept {
    if (orderId >= MAX_ORDER_ID) {
        return false;
    }

    const OrderLocation& loc = idToLocation_[orderId];
    
    if (loc.generation != generation_ || loc.price == PRICE_INVALID) {
        return false;
    }

    auto& levels = (loc.side == OrderSide::Buy) ? bids_ : asks_;
    auto& level = levels[loc.price];
    
    if (static_cast<size_t>(loc.index) >= level.orders.size()) {
        return false;
    }

    Order& order = level.orders[loc.index];
    
    if (order.id != orderId || !order.isActive()) {
        return false;
    }

    // Only allow reduce
    if (newQuantity >= order.quantity) {
        return false;
    }

    if (newQuantity == 0) {
        return cancelOrder(orderId);
    }

    order.quantity = newQuantity;
    return true;
}

// ============================================================================
// Helper functions
// ============================================================================

void OrderBook::updateBestBidAfterRemoval(Price removedPrice) noexcept {
    if (removedPrice == 0) {
        size_t found = bidMask_.findFirstSetDown(MAX_PRICE - 1);
        bestBid_ = (found >= static_cast<size_t>(MAX_PRICE)) 
                   ? PRICE_INVALID 
                   : static_cast<Price>(found);
    } else {
        size_t found = bidMask_.findFirstSetDown(static_cast<size_t>(removedPrice) - 1);
        bestBid_ = (found >= static_cast<size_t>(MAX_PRICE)) 
                   ? PRICE_INVALID 
                   : static_cast<Price>(found);
    }
}

void OrderBook::updateBestAskAfterRemoval(Price removedPrice) noexcept {
    size_t found = askMask_.findFirstSet(static_cast<size_t>(removedPrice) + 1);
    bestAsk_ = (found >= static_cast<size_t>(MAX_PRICE)) 
               ? PRICE_INVALID 
               : static_cast<Price>(found);
}

// ============================================================================
// reset
// ============================================================================

void OrderBook::reset() noexcept {
    generation_++;

    for (auto& level : bids_) {
        level.reset();
    }
    for (auto& level : asks_) {
        level.reset();
    }

    pool_.release();
    bidMask_.clearAll();
    askMask_.clearAll();
    bestBid_ = PRICE_INVALID;
    bestAsk_ = PRICE_INVALID;
}

// ============================================================================
// printBook
// ============================================================================

void OrderBook::printBook() const {
    int bidCount = 0, askCount = 0;
    Quantity bidVol = 0, askVol = 0;
    
    for (const auto& level : bids_) {
        bidCount += level.activeCount;
        bidVol += level.totalVolume();
    }
    for (const auto& level : asks_) {
        askCount += level.activeCount;
        askVol += level.totalVolume();
    }
    
    std::cout << "========================================\n"
              << "OrderBook State (generation=" << generation_ << ")\n"
              << "----------------------------------------\n"
              << "  Best Bid:    " << (bestBid_ == PRICE_INVALID ? -1 : bestBid_) << "\n"
              << "  Best Ask:    " << (bestAsk_ == PRICE_INVALID ? -1 : bestAsk_) << "\n"
              << "  Bid Orders:  " << bidCount << " (vol=" << bidVol << ")\n"
              << "  Ask Orders:  " << askCount << " (vol=" << askVol << ")\n"
              << "========================================\n";
}
