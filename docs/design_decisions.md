# Design Decisions

## 1. PMR Monotonic Buffer vs. tcmalloc/jemalloc

**Choice**: `std::pmr::monotonic_buffer_resource` with 512MB pre-allocated buffer.

**Why**: The order book performs millions of allocations per second for `PriceLevel::orders` vectors. General-purpose allocators (malloc, tcmalloc, jemalloc) average 50-200ns per allocation due to thread-safety locks, free-list traversal, and system call overhead. PMR monotonic allocation is ~5ns (pointer bump). The tradeoff is that memory is never freed until the resource is destroyed, but for an order book that resets per session, this is acceptable.

**Measured impact**: 61ns per add-order operation (including allocation), which would be 100-250ns with malloc.

## 2. CRTP vs. Virtual Dispatch on Matching Hot Path

**Choice**: `MatchingStrategyBase<Derived>` (CRTP) for compile-time dispatch, with `MatchingStrategy` virtual base preserved for flexibility.

**Why**: The matching engine processes every order. Virtual dispatch adds:
- vtable pointer dereference (~3ns)
- Indirect branch misprediction penalty (~5-15ns on branch miss)
- Inlining prevention (compiler cannot see through `virtual`)

At 16M ops/sec, saving 5-10ns is an 8-16% throughput improvement. CRTP resolves the call at compile time, allowing the compiler to inline `matchImpl()` into the caller. The virtual base is kept for contexts where runtime polymorphism is genuinely needed (e.g., configuration-driven strategy selection).

## 3. Hand-Rolled 2x2 Kalman vs. Eigen

**Choice**: Manual 2x2 matrix operations (4 doubles per matrix, ~80 lines of code).

**Why**: The Kalman filter runs on every tick (hot path). Eigen's `Matrix2d` introduces:
- Header bloat (~2MB of template instantiation)
- Potential heap allocation for dynamic-size matrices
- Expression template overhead for trivially small operations

For a 2x2 system, the predict-update cycle is 6 multiplications and 4 additions per matrix operation. Hand-rolling this is simpler, produces tighter assembly, and avoids the Eigen dependency on the hot path. Eigen is used in the analytics layer (cointegration tests) where latency doesn't matter.

## 4. Fixed Arrays vs. Hash Maps in RiskManager

**Choice**: `std::array<SymbolRisk, 64>` indexed by `symbolId`.

**Why**: `preTradeCheck()` runs on every quote (hot path). `std::unordered_map` has:
- Hash computation (~10ns)
- Pointer chasing through bucket chains (~20ns on cache miss)
- Heap allocation on insert

A fixed array with direct indexing is O(1) with no hash, no allocation, and guaranteed cache-line alignment. The tradeoff is wasted memory for unused symbol slots (64 * sizeof(SymbolRisk) ~= 2KB), which is negligible. For production with thousands of symbols, a flat array up to 4096 would still be preferable to a hash map.

## 5. Event-Driven vs. Vectorized Backtesting

**Choice**: Event-driven (`Simulator::onTick()` processes one tick at a time).

**Why**: Vectorized backtests (e.g., pandas-style column operations) suffer from two critical flaws:
1. **Lookahead bias**: Operations on entire columns can accidentally use future data. For example, `df['signal'] = df['spread'].rolling(100).mean()` uses the current and next 99 values in the default center-aligned mode.
2. **Fill model impossibility**: Realistic fill simulation requires maintaining queue position state, which depends on the full order lifecycle (submit, queue, partial fill, cancel). Vectorized approaches typically assume fills at mid or at the limit price, producing unrealistically high Sharpe ratios (often 2-5x above reality).

The event-driven architecture processes ticks sequentially, making lookahead bias structurally impossible and enabling realistic fill simulation with latency, queue position, and adverse selection.

## 6. OU MLE via Brent's Method vs. AR(1) OLS

**Choice**: Concentrated MLE with Brent's method, AR(1) OLS kept as fallback.

**Why**: AR(1) OLS estimates of the OU persistence parameter $\phi$ are biased downward in finite samples (Shaman & Stine, 1988). This bias inflates mean-reversion speed $\theta$ and underestimates half-life, leading to premature entry signals. MLE is consistent and asymptotically efficient, and the concentrated likelihood makes the optimization one-dimensional, solvable by Brent's method without external optimization libraries.

## 7. Volume-Synchronized Sampling (VPIN) vs. Time-Based Toxicity

**Choice**: VPIN with fixed-volume buckets.

**Why**: Time-based sampling treats quiet and active periods equally. During low-volume periods, each trade carries more information, but time-based metrics dilute this signal. Volume-synchronized sampling naturally adjusts: during quiet periods, buckets fill slowly, giving each trade appropriate weight. During high activity, buckets fill quickly, adapting the measurement frequency to market conditions. This matches the information-theoretic insight that informed traders choose when to trade, so volume is a better clock than time.

## 8. Atomic Kill Switch vs. Mutex-Protected State

**Choice**: `std::atomic<bool>` with relaxed memory ordering.

**Why**: The kill switch is the first check in `preTradeCheck()`. A mutex would add ~25ns of lock/unlock overhead on every single quote (millions per second). An atomic load with `memory_order_relaxed` is ~1ns. The relaxed ordering is acceptable because the kill switch is a one-way gate: once activated, all subsequent reads will eventually see `true` (within one cache coherency round-trip, typically <100ns on modern CPUs). There is no data race because the kill switch only gates a boolean decision, not a complex state transition.
