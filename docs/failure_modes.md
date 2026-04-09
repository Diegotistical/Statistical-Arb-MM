# Failure Modes and Lessons Learned

## 1. Cointegration Breakdown

**What happens**: The pair's cointegration relationship dissolves. The spread becomes a random walk instead of mean-reverting. The strategy keeps entering positions expecting reversion that never comes.

**Symptoms**:
- Half-life increases suddenly (>100 periods)
- ADF test p-value crosses above 0.10
- Johansen rank drops from 1 to 0
- Large, sustained drawdown

**Detection**: Rolling Engle-Granger test with a 252-period window. Alert when:
- p-value > 0.10 for 3 consecutive windows
- Half-life doubles from its calibrated value
- Hedge ratio (beta) moves outside 2-sigma confidence interval

**Mitigation**:
- Stop trading the pair immediately
- Flatten existing position at market
- Re-screen pair universe for new cointegration candidates
- The regime break detector (`detectRegimeBreak()`) catches this automatically

**Lesson**: Cointegration is not permanent. Pairs that were cointegrated for 5 years can decouple permanently due to mergers, sector rotation, or regulatory changes. The strategy must continuously validate cointegration, not just at calibration time.

## 2. Flash Crash / Liquidity Withdrawal

**What happens**: A sudden, large price move triggers a cascade of selling. The order book empties. The strategy's resting orders get adversely selected against.

**Symptoms**:
- VPIN spikes above 0.7
- Kyle's lambda increases by 5x+
- Spread widens dramatically
- Queue depth collapses

**Detection**: VPIN > 0.6 sustained for 5+ volume buckets. Combined with spread > 5x normal.

**Mitigation**:
- Signal modulation already widens spreads when VPIN is high
- At extreme VPIN (>0.7), the risk manager's drawdown breaker should halt trading
- Fat-finger checks prevent chasing the move with oversized orders
- Never rely on resting orders in a flash crash -- they will be picked off

**Lesson**: The "nickel problem" -- flash crashes are rare but have infinite expected loss in the absence of risk limits. The drawdown circuit breaker is not optional.

## 3. Adverse Selection Spiral

**What happens**: The strategy consistently loses on fills because it's trading against informed flow. Each fill moves the price against the position. The strategy then doubles down (adding to the losing position) because the z-score signal strengthens.

**Symptoms**:
- Negative spread capture (losing more on AS than earning on spread)
- PnL decomposition shows adverse_selection > spread_capture
- Fill rate is unusually high (informed traders are picking off your quotes)
- OFI consistently moves against filled positions

**Detection**: Rolling ratio of adverse_selection / spread_capture > 1.5 for 100+ ticks.

**Mitigation**:
- VPIN and Kyle's lambda modulation should widen spreads before this spiral develops
- OFI gating prevents entering when order flow is toxic
- Reduce quote sizes (inventory scaling already does this near position limits)
- Consider asymmetric quoting: only quote the non-toxic side

**Lesson**: Being filled is NOT always good. In market making, getting filled more than expected is a warning sign, not a success signal. High fill rates often indicate your quotes are too aggressive relative to the information environment.

## 4. Parameter Drift

**What happens**: The optimal parameters (gamma, k, z-threshold) change over time as market conditions evolve. The strategy operates with stale parameters, producing suboptimal or negative Sharpe.

**Symptoms**:
- Walk-forward OOS Sharpe declining over successive folds
- Overfit ratio dropping below 0.3
- Increasing divergence between in-sample and out-of-sample performance

**Detection**: Walk-forward overfit ratio trend analysis. If the 5-fold moving average of overfit ratio crosses below 0.4, parameters are drifting.

**Mitigation**:
- Regular parameter re-estimation (weekly or monthly)
- Use Kalman filter for parameters that need continuous adaptation (hedge ratio)
- Maintain a parameter stability score: reject parameter sets that change >30% between recalibrations
- Consider online learning for intensity parameter k

**Lesson**: A strategy that works with fixed parameters is fragile. Robust strategies have built-in adaptation mechanisms (Kalman filter for beta, rolling cointegration for pair selection) rather than relying on periodic manual recalibration.

## 5. Overfitting in Walk-Forward

**What happens**: Despite using walk-forward (which is supposed to prevent overfitting), the strategy finds spurious patterns in the training data that don't generalize.

**Causes**:
- Too many parameters in the grid (curse of dimensionality)
- Training window too short relative to strategy's holding period
- Test window too short to distinguish signal from noise
- Multiple testing bias (trying many pairs, keeping the best)

**Detection**: OOS Sharpe / IS Sharpe < 0.3 consistently.

**Mitigation**:
- Limit parameter grid to at most 3 parameters with at most 10 values each
- Training window should be at least 10x the half-life
- Test window should contain at least 30 trades
- Apply Bonferroni or FDR correction when screening multiple pairs

**Lesson**: Walk-forward is necessary but not sufficient. The overfit ratio is the key diagnostic. A strategy with IS Sharpe = 5.0 and OOS Sharpe = 0.5 (OR = 0.1) is worse than one with IS Sharpe = 1.5 and OOS Sharpe = 1.2 (OR = 0.8).

## 6. What Didn't Work

### Naive Fixed-Spread Market Making
Before implementing A-S, we tested a fixed-spread market maker (always quote at mid +/- 2 ticks). Result: Sharpe was negative because the fixed spread was either too wide (missed fills in calm markets) or too narrow (adverse selection in volatile markets). The A-S model adapts spread to volatility and inventory, which is the core improvement.

### Z-Score Entry Without OFI Confirmation
Pure z-score entry (no OFI gating) produced 30% more trades but 40% more adverse selection. The OFI filter eliminates entries where the z-score signal is contradicted by order flow direction. The OFI validation A/B test (`OFIValidator`) confirmed this quantitatively.

### AR(1) OLS for OU Half-Life
The AR(1) OLS estimator consistently underestimated half-life by 20-30% compared to MLE, leading to premature entries. The MLE estimator produces more accurate half-lives, especially for slow mean reversion (half-life > 20 periods).
