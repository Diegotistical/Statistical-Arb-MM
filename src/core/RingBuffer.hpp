#pragma once

#include <atomic>
#include <cstddef>
#include <cstring>
#include <memory>
#include <new>
#include <thread>

// Platform-specific pause/yield intrinsics
#if defined(__x86_64__) || defined(_M_X64)
    #ifdef _MSC_VER
        #include <intrin.h>
        #define CPU_PAUSE() _mm_pause()
    #else
        #include <emmintrin.h>
        #define CPU_PAUSE() _mm_pause()
    #endif
#elif defined(__aarch64__)
    #define CPU_PAUSE() asm volatile("yield")
#else
    #define CPU_PAUSE() std::this_thread::yield()
#endif

// Hardware cache line size
inline constexpr size_t CACHE_LINE_SIZE = 64;

// ============================================================================
// SpinLock - Low-latency spinlock with TTAS (Test-Test-And-Set)
// ============================================================================
class alignas(CACHE_LINE_SIZE) SpinLock {
public:
    void lock() noexcept {
        for (;;) {
            // Optimistic fast-path
            if (!flag_.test_and_set(std::memory_order_acquire)) {
                return;
            }
            // Spin-wait with pause
            while (flag_.test(std::memory_order_relaxed)) {
                CPU_PAUSE();
            }
        }
    }

    void unlock() noexcept {
        flag_.clear(std::memory_order_release);
    }

    [[nodiscard]] bool try_lock() noexcept {
        return !flag_.test(std::memory_order_relaxed) &&
               !flag_.test_and_set(std::memory_order_acquire);
    }

private:
    std::atomic_flag flag_ = ATOMIC_FLAG_INIT;
};

// ============================================================================
// SPSCQueue - Lock-free Single-Producer Single-Consumer Queue
// ============================================================================
template <typename T, size_t Capacity>
class alignas(CACHE_LINE_SIZE) SPSCQueue {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of 2");
    static_assert(Capacity >= 2, "Capacity must be at least 2");

public:
    SPSCQueue() : head_(0), tail_(0) {
        static_assert(std::is_trivially_copyable_v<T>, 
                      "T must be trivially copyable for memcpy optimization");
    }

    // Push item (producer only) - returns false if full
    [[nodiscard]] bool push(const T& item) noexcept {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        const size_t next = (tail + 1) & MASK;
        
        if (next == head_.load(std::memory_order_acquire)) [[unlikely]] {
            return false; // Queue full
        }
        
        buffer_[tail] = item;
        tail_.store(next, std::memory_order_release);
        return true;
    }

    // Pop item (consumer only) - returns false if empty
    [[nodiscard]] bool pop(T& item) noexcept {
        const size_t head = head_.load(std::memory_order_relaxed);
        
        if (head == tail_.load(std::memory_order_acquire)) [[unlikely]] {
            return false; // Queue empty
        }
        
        item = buffer_[head];
        head_.store((head + 1) & MASK, std::memory_order_release);
        return true;
    }

    // Blocking push (spins until space available)
    void push_blocking(const T& item) noexcept {
        while (!push(item)) {
            CPU_PAUSE();
        }
    }

    // Batch push for high throughput
    [[nodiscard]] bool push_batch(const T* items, size_t count) noexcept {
        if (count == 0) return true;
        
        const size_t tail = tail_.load(std::memory_order_relaxed);
        const size_t head = head_.load(std::memory_order_acquire);
        const size_t available = (Capacity + head - tail - 1) & MASK;
        
        if (count > available) [[unlikely]] {
            return false;
        }

        // Copy in up to two segments (wrap-around)
        const size_t first_chunk = std::min(count, Capacity - tail);
        std::memcpy(&buffer_[tail], items, first_chunk * sizeof(T));
        
        if (first_chunk < count) {
            std::memcpy(&buffer_[0], items + first_chunk, 
                       (count - first_chunk) * sizeof(T));
        }
        
        tail_.store((tail + count) & MASK, std::memory_order_release);
        return true;
    }

    // Batch pop for high throughput
    [[nodiscard]] size_t pop_batch(T* dest, size_t max_count) noexcept {
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t tail = tail_.load(std::memory_order_acquire);
        
        if (head == tail) [[unlikely]] {
            return 0;
        }

        const size_t available = (Capacity + tail - head) & MASK;
        const size_t count = std::min(max_count, available);

        // Copy in up to two segments (wrap-around)
        const size_t first_chunk = std::min(count, Capacity - head);
        std::memcpy(dest, &buffer_[head], first_chunk * sizeof(T));
        
        if (first_chunk < count) {
            std::memcpy(dest + first_chunk, &buffer_[0], 
                       (count - first_chunk) * sizeof(T));
        }
        
        head_.store((head + count) & MASK, std::memory_order_release);
        return count;
    }

    // Query current size (approximate, for monitoring only)
    [[nodiscard]] size_t size() const noexcept {
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t tail = tail_.load(std::memory_order_relaxed);
        return (Capacity + tail - head) & MASK;
    }

    [[nodiscard]] bool empty() const noexcept {
        return head_.load(std::memory_order_relaxed) == 
               tail_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] static constexpr size_t capacity() noexcept {
        return Capacity - 1; // One slot reserved
    }

private:
    static constexpr size_t MASK = Capacity - 1;

    // Separate cache lines to prevent false sharing
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> head_;
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> tail_;
    alignas(CACHE_LINE_SIZE) T buffer_[Capacity];
};

// ============================================================================
// RingBuffer - Mutex-based ring buffer (for multi-producer scenarios)
// ============================================================================
template <typename T>
class RingBuffer {
public:
    explicit RingBuffer(size_t size)
        : size_(size), 
          buffer_(std::make_unique<T[]>(size)), 
          head_(0), 
          tail_(0) {}

    RingBuffer(const RingBuffer&) = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;

    [[nodiscard]] bool push(const T& item) noexcept {
        lock_.lock();
        const size_t next_tail = (tail_ + 1) % size_;
        
        if (next_tail == head_) {
            lock_.unlock();
            return false;
        }
        
        buffer_[tail_] = item;
        tail_ = next_tail;
        lock_.unlock();
        return true;
    }

    [[nodiscard]] bool pop(T& item) noexcept {
        lock_.lock();
        
        if (head_ == tail_) {
            lock_.unlock();
            return false;
        }
        
        item = buffer_[head_];
        head_ = (head_ + 1) % size_;
        lock_.unlock();
        return true;
    }

    void push_blocking(const T& item) noexcept {
        while (!push(item)) {
            CPU_PAUSE();
        }
    }

    [[nodiscard]] bool push_batch(const T* items, size_t count) noexcept {
        if (count == 0) return true;
        
        lock_.lock();
        const size_t available = (size_ + head_ - tail_ - 1) % size_;
        
        if (count > available) {
            lock_.unlock();
            return false;
        }

        const size_t first_chunk = std::min(count, size_ - tail_);
        std::memcpy(&buffer_[tail_], items, first_chunk * sizeof(T));
        
        if (first_chunk < count) {
            std::memcpy(&buffer_[0], items + first_chunk,
                       (count - first_chunk) * sizeof(T));
        }
        
        tail_ = (tail_ + count) % size_;
        lock_.unlock();
        return true;
    }

    [[nodiscard]] size_t pop_batch(T* dest, size_t max_count) noexcept {
        lock_.lock();
        
        if (head_ == tail_) {
            lock_.unlock();
            return 0;
        }

        const size_t available = (size_ + tail_ - head_) % size_;
        const size_t count = std::min(max_count, available);

        const size_t first_chunk = std::min(count, size_ - head_);
        std::memcpy(dest, &buffer_[head_], first_chunk * sizeof(T));
        
        if (first_chunk < count) {
            std::memcpy(dest + first_chunk, &buffer_[0],
                       (count - first_chunk) * sizeof(T));
        }
        
        head_ = (head_ + count) % size_;
        lock_.unlock();
        return count;
    }

    [[nodiscard]] size_t size() noexcept {
        lock_.lock();
        const size_t count = (size_ + tail_ - head_) % size_;
        lock_.unlock();
        return count;
    }

private:
    size_t size_;
    std::unique_ptr<T[]> buffer_;
    size_t head_;
    size_t tail_;
    SpinLock lock_;
};
