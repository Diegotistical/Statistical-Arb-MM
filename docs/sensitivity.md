# Sensitivity Analysis

## Overview

This document describes the key parameter sensitivities of the statistical arbitrage market-making strategy. Understanding these sensitivities is critical for:
1. Robust parameter selection (avoiding overfitting)
2. Understanding strategy behavior under regime changes
3. Identifying which parameters the strategy is most/least sensitive to

## 1. Sharpe vs. Risk Aversion (gamma)

**Parameter**: `gamma` in StatArbMM (risk aversion / inventory penalty intensity)

**Expected behavior**:
- Low gamma (0.001): Large inventory buildup, higher return potential but extreme tail risk
- Medium gamma (0.01-0.05): Balanced inventory control, moderate Sharpe
- High gamma (0.1+): Very aggressive inventory reduction, tight quotes, low return but low risk

**Key insight**: The Sharpe-maximizing gamma depends on the spread's mean-reversion speed. Fast mean reversion (short half-life) tolerates lower gamma because inventory risk resolves quickly. Slow mean reversion requires higher gamma to avoid drawdowns during extended excursions.

**How to test**:
```cpp
WalkForwardOptimizer wfo;
wfo.addGammaRange(0.001, 0.1, 20);
wfo.addKRange(1.5, 1.5, 1);  // Fix k
wfo.addZThresholdRange(2.0, 2.0, 1);  // Fix z
auto result = wfo.run(ticks);
```

## 2. Sharpe vs. Order Intensity (k)

**Parameter**: `k` in StatArbMM (fill rate sensitivity to spread width)

**Expected behavior**:
- Low k (0.5): Fill rate is insensitive to spread. Wide optimal spread. Fewer fills but higher per-trade profit.
- High k (5.0): Fill rate drops sharply with spread. Narrow optimal spread. More fills but lower per-trade profit.

**Key insight**: The intensity term `(2/gamma) * ln(1 + gamma/k)` is the dominant contribution to the optimal spread. Getting `k` wrong by 2x changes the spread by ~50%, which directly impacts both fill rate and adverse selection exposure.

**Calibration**: `k` should be estimated from historical fill data, not set arbitrarily.

## 3. Sharpe vs. Z-Score Entry Threshold

**Parameter**: `zEntryThreshold` in StatArbMM

**Expected behavior**:
- Low threshold (1.0): More frequent trading, captures smaller mispricings, higher adverse selection
- Medium threshold (1.5-2.0): Balanced frequency, filters noise
- High threshold (2.5+): Rare trading, only captures extreme mispricings, potential alpha decay before fill

**Key insight**: The optimal threshold depends on the spread's distributional properties. If the spread is truly mean-reverting (OU), a threshold of ~2 sigma captures entries at the 2.5% and 97.5% tails, which maximizes expected profit per trade. If the spread has fat tails, lower thresholds increase adverse selection risk.

## 4. Sharpe vs. Kalman Filter Delta

**Parameter**: `delta` in KalmanHedgeRatio (forgetting factor)

**Expected behavior**:
- High delta (0.9999): Slow adaptation. Stable hedge ratio. Good for pairs with structural stability.
- Low delta (0.99): Fast adaptation. Tracks regime changes but noisy. Good for pairs with time-varying betas.

**Key insight**: If delta is too low, the Kalman filter tracks noise in the hedge ratio, producing spurious z-scores. If delta is too high, it misses genuine structural breaks, leading to large drawdowns when the cointegration relationship changes.

## 5. PnL vs. Maximum Position Limit

**Parameter**: `maxPositionPerSymbol` in RiskManager

**Expected behavior**:
- Tight limit (100): Caps exposure but truncates profitable trends. Lower variance.
- Loose limit (5000): Allows full position buildup. Higher variance, potentially higher return.

**Key insight**: The position limit interacts with gamma. A tight position limit with low gamma creates contradictory incentives (the strategy wants to build position but can't), reducing Sharpe. A loose position limit with high gamma is redundant (the strategy self-limits via reservation price). The optimal pairing is moderate gamma with a position limit set to ~2x the typical position from gamma alone.

## 6. Strategy Robustness Under Parameter Perturbation

**Methodology**: Monte Carlo perturbation of the parameter vector
1. Start from the walk-forward optimal parameters
2. Perturb each parameter by +/- 20%
3. Re-run backtest 100 times with random perturbations
4. Measure: mean and std of OOS Sharpe across perturbations

**Interpretation**:
- If Sharpe std < 0.3 across perturbations: robust strategy
- If Sharpe std > 0.5: brittle, likely overfit
- If Sharpe mean drops > 50% under perturbation: parameter sensitivity is too high

## 7. Fill Rate Sensitivity

**Fill rate** is the fraction of submitted quotes that result in fills. It depends on:
- Spread width (wider -> fewer fills)
- Queue position (later in queue -> fewer fills)
- Latency (slower -> worse queue position)

In backtesting, fill rate is typically 10-30% for passive orders. If the backtester reports fill rates above 50%, the fill model is likely too optimistic. The execution simulator's queue position decay and adverse selection modeling produce realistic fill rates.

## 8. Key Takeaways

1. **gamma and k are the most sensitive parameters** -- getting either wrong by 2x can change Sharpe by 50%+
2. **z-threshold is moderately sensitive** -- the Sharpe surface is relatively flat between 1.5 and 2.5
3. **Kalman delta is a structural choice** -- wrong delta doesn't just reduce Sharpe, it can change the strategy's behavior qualitatively (trending vs. mean-reverting)
4. **Walk-forward overfit ratio is the single best diagnostic** -- if OOS Sharpe / IS Sharpe < 0.3, no amount of parameter tuning will save the strategy
