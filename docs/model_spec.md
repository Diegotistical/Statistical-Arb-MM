# Model Specification

## 1. Avellaneda-Stoikov Optimal Market Making

### HJB Equation

The market maker maximizes expected utility of terminal wealth:

$$\max_{\delta^a, \delta^b} \mathbb{E}\left[-\exp(-\gamma W_T)\right]$$

where $W_T$ is terminal wealth, $\gamma$ is risk aversion, and $\delta^a, \delta^b$ are the ask and bid offsets from the mid-price.

The Hamilton-Jacobi-Bellman equation for the value function $u(s, x, q, t)$ is:

$$\partial_t u + \frac{1}{2}\sigma^2 \partial_{ss} u + \lambda^a(\delta^a)(u(s, x+s+\delta^a, q-1, t) - u) + \lambda^b(\delta^b)(u(s, x-s+\delta^b, q+1, t) - u) = 0$$

### Closed-Form Solution

Under exponential utility with Poisson order arrivals $\lambda(\delta) = A\exp(-k\delta)$:

**Reservation price:**

$$r(s, q, t) = s - q \cdot \gamma \cdot \sigma^2 \cdot (T - t)$$

**Optimal spread:**

$$\delta^*(t) = \gamma \sigma^2 (T - t) + \frac{2}{\gamma} \ln\left(1 + \frac{\gamma}{k}\right)$$

**Optimal quotes:**

$$p^{bid} = r - \frac{\delta^*}{2}, \quad p^{ask} = r + \frac{\delta^*}{2}$$

### Parameters

| Parameter | Symbol | Description | Typical Range |
|-----------|--------|-------------|---------------|
| Risk aversion | $\gamma$ | Inventory penalty intensity | 0.001 - 0.1 |
| Order intensity | $k$ | Fill rate sensitivity to spread | 0.5 - 5.0 |
| Volatility | $\sigma$ | Per-tick price volatility | Data-dependent |
| Time remaining | $T - t$ | Fraction of session remaining | [0.001, 1.0] |

### Intensity Calibration

The order arrival rate follows $\lambda(\delta) = A \exp(-k\delta)$. We estimate $k$ via log-linear OLS:

$$\ln(\hat{\lambda}_i) = \ln(A) - k \cdot \delta_i + \epsilon_i$$

Given observed (spread, fill rate) pairs from historical or simulated data.

---

## 2. Kalman Filter for Dynamic Hedge Ratio

### State-Space Model

$$\mathbf{x}_t = \begin{bmatrix} \alpha_t \\ \beta_t \end{bmatrix}, \quad \mathbf{x}_t = \mathbf{x}_{t-1} + \mathbf{w}_t, \quad \mathbf{w}_t \sim \mathcal{N}(0, \mathbf{Q})$$

$$y_t = \mathbf{H}_t \mathbf{x}_t + v_t, \quad v_t \sim \mathcal{N}(0, R)$$

where $y_t = \log(P^A_t)$, $\mathbf{H}_t = [1, \log(P^B_t)]$.

### Prediction

$$\hat{\mathbf{x}}_{t|t-1} = \hat{\mathbf{x}}_{t-1|t-1}, \quad \mathbf{P}_{t|t-1} = \frac{1}{\delta}\mathbf{P}_{t-1|t-1}$$

### Update

$$e_t = y_t - \mathbf{H}_t \hat{\mathbf{x}}_{t|t-1} \quad \text{(innovation)}$$

$$S_t = \mathbf{H}_t \mathbf{P}_{t|t-1} \mathbf{H}_t' + R \quad \text{(innovation variance)}$$

$$\mathbf{K}_t = \mathbf{P}_{t|t-1} \mathbf{H}_t' S_t^{-1} \quad \text{(Kalman gain)}$$

$$\hat{\mathbf{x}}_{t|t} = \hat{\mathbf{x}}_{t|t-1} + \mathbf{K}_t e_t$$

$$\mathbf{P}_{t|t} = (\mathbf{I} - \mathbf{K}_t \mathbf{H}_t) \mathbf{P}_{t|t-1}$$

The z-score is: $z_t = e_t / \sqrt{S_t}$

---

## 3. Cointegration Testing

### Engle-Granger Two-Step Test

1. **Step 1**: OLS regression: $\log P^A_t = \hat{\alpha} + \hat{\beta} \log P^B_t + \hat{\epsilon}_t$
2. **Step 2**: ADF test on residuals $\hat{\epsilon}_t$

ADF regression with $p$ lags:

$$\Delta \hat{\epsilon}_t = \alpha + \beta \hat{\epsilon}_{t-1} + \sum_{i=1}^{p} \gamma_i \Delta \hat{\epsilon}_{t-i} + u_t$$

$H_0: \beta = 0$ (unit root). Lag $p$ selected by BIC (Schwert 1989 rule).

### Johansen Trace Test

For the bivariate VECM $\Delta \mathbf{X}_t = \Pi \mathbf{X}_{t-1} + \epsilon_t$:

$$\text{Trace}(r_0) = -T \sum_{i=r_0+1}^{p} \ln(1 - \hat{\lambda}_i)$$

Critical values from Osterwald-Lenum (1992).

---

## 4. Ornstein-Uhlenbeck MLE

The OU process: $dX_t = \theta(\mu - X_t)dt + \sigma dW_t$

Discretized: $X_{t+1} = \phi X_t + (1-\phi)\mu + \sigma_d \epsilon_t$ where $\phi = e^{-\theta \Delta t}$.

Concentrated log-likelihood (profiling out $\mu$ and $\sigma_d^2$):

$$\hat{\mu}(\phi) = \frac{S_y - \phi S_x}{n(1-\phi)}, \quad \hat{\sigma}_d^2(\phi) = \frac{1}{n}\sum_i(y_i - \phi x_i - (1-\phi)\hat{\mu})^2$$

$$\ell^*(\phi) = -\frac{n}{2}\ln\hat{\sigma}_d^2(\phi)$$

Optimized via Brent's method on $\phi \in (0, 1)$.

---

## 5. Microstructure Signals

### VPIN (Easley, Lopez de Prado, O'Hara 2012)

$$\text{VPIN} = \frac{\sum_{i=1}^{n} |V^{buy}_i - V^{sell}_i|}{n \cdot V_{bucket}}$$

Bulk volume classification: $V^{buy} = V \cdot \Phi\left(\frac{\Delta P}{\sigma_{\Delta P}}\right)$

### Kyle's Lambda (Kyle 1985)

$$\Delta P_t = \lambda \cdot \text{OFI}_t + \epsilon_t$$

Estimated via rolling OLS. $\lambda$ measures permanent price impact per unit of signed order flow.

### Order Flow Imbalance (Cont, Kukanov, Stoikov 2014)

$$\text{OFI}_t = \sum_{k=1}^{K} w_k \left(\Delta \text{BidSize}_{t,k} - \Delta \text{AskSize}_{t,k}\right)$$

with weights $w_k = 1/k$ for level $k$.

---

## 6. Risk Management

### Pre-Trade Checks (in order of execution)

1. Kill switch (atomic boolean, ~1ns)
2. Symbol validation
3. Fat-finger: max order size
4. Fat-finger: max notional
5. Per-symbol position limit
6. Portfolio position limit
7. Per-symbol loss limit
8. Portfolio loss limit
9. Drawdown circuit breaker
10. Rate limiting (orders per second)

### Drawdown Circuit Breaker

$$\text{DD}_t = \max_{s \leq t} \text{PnL}_s - \text{PnL}_t$$

Auto-activates kill switch when $\text{DD}_t > \text{MaxDD}$.

---

## 7. Transaction Cost Model (Almgren-Chriss 2001)

$$\text{Cost}(q) = \underbrace{\epsilon \cdot |q|}_{\text{fixed}} + \underbrace{\frac{\text{spread}}{2} \cdot |q|}_{\text{spread}} + \underbrace{\eta \cdot \left(\frac{|q|}{V}\right)^{\alpha} \cdot P \cdot |q|}_{\text{temporary impact}} + \underbrace{\text{fees} \cdot |q|}_{\text{exchange}}$$

where $\alpha \approx 0.5$ (square-root impact law), $V$ is average daily volume.
