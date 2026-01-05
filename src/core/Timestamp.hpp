#pragma once
/**
 * @file Timestamp.hpp
 * @brief Explicit timestamp type for all events.
 * 
 * Ensures:
 *   - All events are timestamped
 *   - Time semantics are explicit
 *   - No implicit time assumptions
 */

#include <cstdint>
#include <chrono>

namespace core {

/**
 * @brief Nanosecond timestamp.
 * 
 * All timestamps are nanoseconds since epoch.
 * Use clock::now() for current time or create from raw value.
 */
struct Timestamp {
    int64_t ns = 0;  ///< Nanoseconds since epoch
    
    constexpr Timestamp() = default;
    constexpr explicit Timestamp(int64_t nanoseconds) : ns(nanoseconds) {}
    
    // Comparison
    constexpr bool operator<(const Timestamp& other) const { return ns < other.ns; }
    constexpr bool operator<=(const Timestamp& other) const { return ns <= other.ns; }
    constexpr bool operator>(const Timestamp& other) const { return ns > other.ns; }
    constexpr bool operator>=(const Timestamp& other) const { return ns >= other.ns; }
    constexpr bool operator==(const Timestamp& other) const { return ns == other.ns; }
    constexpr bool operator!=(const Timestamp& other) const { return ns != other.ns; }
    
    // Arithmetic
    constexpr Timestamp operator+(int64_t delta_ns) const { return Timestamp(ns + delta_ns); }
    constexpr Timestamp operator-(int64_t delta_ns) const { return Timestamp(ns - delta_ns); }
    constexpr int64_t operator-(const Timestamp& other) const { return ns - other.ns; }
    
    // Convert to microseconds
    [[nodiscard]] constexpr int64_t us() const { return ns / 1000; }
    
    // Convert to milliseconds
    [[nodiscard]] constexpr int64_t ms() const { return ns / 1'000'000; }
    
    // Convert to seconds
    [[nodiscard]] constexpr double seconds() const { return ns / 1e9; }
    
    // Create from duration
    template<typename Rep, typename Period>
    static Timestamp from(std::chrono::duration<Rep, Period> d) {
        return Timestamp(std::chrono::duration_cast<std::chrono::nanoseconds>(d).count());
    }
    
    // Current time (for live use)
    static Timestamp now() {
        auto now = std::chrono::high_resolution_clock::now();
        return Timestamp(std::chrono::duration_cast<std::chrono::nanoseconds>(
            now.time_since_epoch()).count());
    }
    
    // Invalid timestamp sentinel
    static constexpr Timestamp invalid() { return Timestamp(-1); }
    [[nodiscard]] constexpr bool isValid() const { return ns >= 0; }
};

// Common durations
namespace duration {
    constexpr int64_t NANOSECOND = 1;
    constexpr int64_t MICROSECOND = 1'000;
    constexpr int64_t MILLISECOND = 1'000'000;
    constexpr int64_t SECOND = 1'000'000'000;
    constexpr int64_t MINUTE = 60 * SECOND;
    constexpr int64_t HOUR = 60 * MINUTE;
}

/**
 * @brief Base class for timestamped events.
 */
struct TimestampedEvent {
    Timestamp timestamp;
    
    TimestampedEvent() = default;
    explicit TimestampedEvent(Timestamp ts) : timestamp(ts) {}
};

} // namespace core
