# Statistical-Arb-MM

![Build Status](https://img.shields.io/badge/build-passing-brightgreen)
![C++ Standard](https://img.shields.io/badge/C%2B%2B-20-blue)
![License](https://img.shields.io/badge/license-MIT-green)

A high-performance hybrid framework for **Market Microstructure Statistical Arbitrage**.

This repository implements a latency-critical trading engine in **C++20** with **Python (Pybind11)** bindings for research and strategy orchestration. It is designed to bridge the gap between high-level statistical modeling and low-level execution realities.

---

## 🚀 Key Features

### 1. Hybrid Architecture (C++ / Python)
* **Core Engine (C++20):** Handles the Limit Order Book (LOB), order matching, and signal generation with strict memory management and cache locality.
* **Research Layer (Python):** Uses `pybind11` to expose the C++ engine to Jupyter Notebooks for rapid prototyping of alphas (e.g., Cointegration, Ornstein-Uhlenbeck fitting).

### 2. Microstructure Signals
Unlike standard pairs trading, this engine incorporates L1/L2 order book dynamics:
* **Order Flow Imbalance (OFI):** Predictive modeling based on bid/ask queue depletion.
* **Book Pressure:** Weighted mid-price calculations to anticipate tick-level price moves.
* **Queue Position Simulation:** Backtesting engine accounts for FIFO queue priority to model adverse selection risk.

### 3. Engineering & Performance
* **Modern C++20:** Utilizes **Concepts** (`requires std::floating_point`) for type safety and template optimization.
* **Zero-Copy Data Paths:** Optimized for minimal overhead when passing tick data between Python and C++.
* **Event-Driven Backtesting:** Simulates execution tick-by-tick (non-vectorized) to prevent look-ahead bias.

---

## 🛠️ Tech Stack

| Component | Technology | Description |
| :--- | :--- | :--- |
| **Core Logic** | C++20 | Latency-critical execution path |
| **Bindings** | Pybind11 | Seamless C++ to Python interoperability |
| **Build System** | CMake 3.14+ | Cross-platform build configuration |
| **Data Analysis** | Pandas / NumPy | Signal research and PnL visualization |
| **Testing** | GTest / Pytest | Unit testing for both C++ core and Python logic |

---

## 🏗️ Repository Structure

*(Section intentionally left empty — see repo tree for full structure.)*

---

## ⚡ Quick Start

### Prerequisites
* **C++ Compiler:** GCC 10+, Clang 10+, or MSVC 2019+ (Must support C++20)
* **Python:** 3.8+
* **CMake:** 3.14+

### Build Instructions

**1. Clone and Configure**
```bash
git clone https://github.com/Diegotistical/Statistical-Arb-MM.git
cd Statistical-Arb-MM
mkdir build && cd build

**2. Compile the Engine**

```bash
# Linux / macOS
cmake ..
make -j4

# Windows (PowerShell)
cmake --build . --config Release

Here is the text converted into clean, copy-pasteable Markdown format. I have added the appropriate code blocks and bolding to make it readable on GitHub.

Markdown

### 2. Compile the Engine

```bash
# Linux / macOS
cmake ..
make -j4

# Windows (PowerShell)
cmake --build . --config Release
3. Run the "Steel Thread" Test
Verify the C++ engine is callable from Python:

```bash

python3 -c "import sys, os; sys.path.append('python'); import stat_arb_mm; engine = stat_arb_mm.MatchingEngine(); engine.print_status()"
Running the Standalone C++ Runner
For profiling and memory testing without Python overhead:

```bash

./bin/stat_arb_runner

### 📊 Strategy Logic
The core strategy focuses on mean-reversion of the spread between correlated assets, filtered by microstructure signals.

Ingestion: Tick data updates the internal Limit Order Book.

Signal: Calculate the Z-Score of the spread.

Filter: Trade execution is gated by OFI (Order Flow Imbalance). Enter trades only if order book pressure confirms the move.

Execution: Orders are simulated with configurable latency (e.g., 5ms) to model real-world slippage.

### 📜 Disclaimer
This software is for educational and research purposes only. Do not use this codebase for live financial trading without extensive testing and risk management protocols.

## ***📬 Contact***