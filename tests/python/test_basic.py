import pytest
import sys
import os

# Ensure we can import the module if running from root without install
sys.path.append(os.path.join(os.getcwd(), 'python'))

try:
    import stat_arb_mm
except ImportError:
    pytest.fail("Could not import stat_arb_mm. Did you build the project?")

def test_engine_initialization():
    engine = stat_arb_mm.MatchingEngine()
    assert engine is not None

def test_spread_calculation():
    engine = stat_arb_mm.MatchingEngine()
    prices_a = [100.0, 101.0, 102.0]
    prices_b = [99.0, 100.0, 101.0]
    
    # Expected spread is exactly 1.0 everywhere
    spread = engine.calculate_spread(prices_a, prices_b)
    assert spread == pytest.approx(1.0, 0.0001)

def test_status_output(capsys):
    engine = stat_arb_mm.MatchingEngine()
    engine.print_status()
    captured = capsys.readouterr()
    assert "Core is online" in captured.out