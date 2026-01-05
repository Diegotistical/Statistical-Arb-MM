"""
Statistical Arbitrage Market Making Simulator

Python bindings for ultra-low-latency C++ order book engine.
"""

try:
    from .stat_arb_mm import *
except ImportError:
    # Not yet compiled
    pass

__version__ = "1.0.0"
__all__ = [
    "OrderBook",
    "Order",
    "Trade",
    "MatchingStrategy",
    "TickNormalizer",
    "SpreadModel",
    "OFI",
    "Side",
    "Type",
    "TIF",
]
