#pragma once
/**
 * @file Bitset.hpp
 * @brief SIMD-optimized bitmask for O(1) price level discovery.
 * 
 * Used by OrderBook to track which price levels have active orders.
 * Key operations:
 *   - findFirstSet(start): Find lowest ask (scan up)
 *   - findFirstSetDown(start): Find highest bid (scan down)
 * 
 * Implementation uses CPU intrinsics:
 *   - MSVC: _BitScanForward64 / _BitScanReverse64
 *   - GCC/Clang: __builtin_ctzll / __builtin_clzll
 * 
 * @note This is critical for O(1) best bid/ask discovery.
 */

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

// ============================================================================
// Cross-platform bit manipulation intrinsics
// ============================================================================

#ifdef _MSC_VER
    #include <intrin.h>
    #pragma intrinsic(_BitScanForward64, _BitScanReverse64)
    
    /**
     * @brief Count trailing zeros (position of lowest set bit).
     * @param x Non-zero 64-bit value
     * @return Index of lowest set bit (0-63)
     */
    inline int ctz64(uint64_t x) noexcept {
        unsigned long idx;
        _BitScanForward64(&idx, x);
        return static_cast<int>(idx);
    }
    
    /**
     * @brief Count leading zeros.
     * @param x Non-zero 64-bit value
     * @return Number of leading zeros (0-63)
     */
    inline int clz64(uint64_t x) noexcept {
        unsigned long idx;
        _BitScanReverse64(&idx, x);
        return 63 - static_cast<int>(idx);
    }
#else
    inline int ctz64(uint64_t x) noexcept {
        return __builtin_ctzll(x);
    }
    
    inline int clz64(uint64_t x) noexcept {
        return __builtin_clzll(x);
    }
#endif

/**
 * @brief SIMD-optimized bitmask for O(1) price level discovery.
 * 
 * Each bit represents a price level (1 = has orders, 0 = empty).
 * Uses 64-bit words for efficient scanning with CPU intrinsics.
 * 
 * Memory: ceil(size/64) * 8 bytes
 * For MAX_PRICE=100000: ~12.5 KB
 * 
 * Thread safety: NOT thread-safe. Use external synchronization.
 */
class PriceBitset {
public:
    /**
     * @brief Construct bitmask for given number of price levels.
     * @param size Number of price levels (e.g., OrderBook::MAX_PRICE)
     */
    explicit PriceBitset(size_t size) 
        : data_((size + 63) / 64, 0), size_(size) {}

    /**
     * @brief Set bit at price index (mark level as active).
     * @param index Price level index
     */
    void set(size_t index) noexcept {
        if (index < size_) [[likely]] {
            data_[index >> 6] |= (1ULL << (index & 63));
        }
    }

    /**
     * @brief Clear bit at price index (mark level as empty).
     * @param index Price level index
     */
    void clear(size_t index) noexcept {
        if (index < size_) [[likely]] {
            data_[index >> 6] &= ~(1ULL << (index & 63));
        }
    }

    /**
     * @brief Clear all bits (bulk reset).
     */
    void clearAll() noexcept {
        std::fill(data_.begin(), data_.end(), 0ULL);
    }

    /**
     * @brief Test if bit is set at index.
     * @param index Price level index
     * @return true if level has active orders
     */
    [[nodiscard]] bool test(size_t index) const noexcept {
        if (index >= size_) [[unlikely]] return false;
        return (data_[index >> 6] & (1ULL << (index & 63))) != 0;
    }

    /**
     * @brief Find first set bit starting from 'start' (scan up).
     * 
     * Used to find best ask (lowest price with orders).
     * 
     * @param start Starting index to search from
     * @return Index of first set bit, or size_ if not found
     */
    [[nodiscard]] size_t findFirstSet(size_t start) const noexcept {
        if (start >= size_) [[unlikely]] return size_;
        
        size_t wordIdx = start >> 6;
        size_t bitPos = start & 63;
        
        // Mask off bits below start position in first word
        uint64_t word = data_[wordIdx] & (~0ULL << bitPos);
        
        if (word != 0) [[likely]] {
            return (wordIdx << 6) + ctz64(word);
        }
        
        // Scan remaining words
        const size_t numWords = data_.size();
        for (++wordIdx; wordIdx < numWords; ++wordIdx) {
            if (data_[wordIdx] != 0) [[likely]] {
                return (wordIdx << 6) + ctz64(data_[wordIdx]);
            }
        }
        
        return size_;
    }

    /**
     * @brief Find first set bit scanning down from 'start'.
     * 
     * Used to find best bid (highest price with orders).
     * 
     * @param start Starting index to search from (inclusive)
     * @return Index of first set bit found, or size_ if not found
     */
    [[nodiscard]] size_t findFirstSetDown(size_t start) const noexcept {
        if (start >= size_) start = size_ - 1;
        
        size_t wordIdx = start >> 6;
        size_t bitPos = start & 63;
        
        // Mask off bits above start position
        uint64_t mask = (bitPos == 63) ? ~0ULL : ((1ULL << (bitPos + 1)) - 1);
        uint64_t word = data_[wordIdx] & mask;
        
        if (word != 0) [[likely]] {
            return (wordIdx << 6) + (63 - clz64(word));
        }
        
        // Scan remaining words downward
        for (size_t i = wordIdx; i-- > 0;) {
            if (data_[i] != 0) [[likely]] {
                return (i << 6) + (63 - clz64(data_[i]));
            }
        }
        
        return size_;
    }

    /**
     * @brief Get total size (number of price levels).
     */
    [[nodiscard]] size_t size() const noexcept { return size_; }

private:
    std::vector<uint64_t> data_;  ///< Bit storage (64 bits per word)
    size_t size_;                 ///< Number of price levels
};
