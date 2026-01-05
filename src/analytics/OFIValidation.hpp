#pragma once
/**
 * @file OFIValidation.hpp
 * @brief A/B testing framework for OFI signal validation.
 * 
 * Compares performance with and without OFI gating:
 *   - Sharpe ratio
 *   - Trade count
 *   - Fill quality
 *   - Adverse selection
 */

#include <vector>
#include <cmath>
#include <algorithm>
#include <numeric>

namespace analytics {

/**
 * @brief OFI validation metrics.
 */
struct OFIValidationResult {
    // With OFI gating
    double sharpe_with_ofi = 0;
    int trades_with_ofi = 0;
    double adverse_selection_with_ofi = 0;
    double pnl_with_ofi = 0;
    
    // Without OFI gating
    double sharpe_without_ofi = 0;
    int trades_without_ofi = 0;
    double adverse_selection_without_ofi = 0;
    double pnl_without_ofi = 0;
    
    // Improvement
    double sharpe_improvement() const {
        if (sharpe_without_ofi == 0) return 0;
        return (sharpe_with_ofi - sharpe_without_ofi) / std::abs(sharpe_without_ofi);
    }
    
    double trade_reduction() const {
        if (trades_without_ofi == 0) return 0;
        return 1.0 - static_cast<double>(trades_with_ofi) / trades_without_ofi;
    }
    
    double as_reduction() const {
        if (adverse_selection_without_ofi == 0) return 0;
        return 1.0 - adverse_selection_with_ofi / adverse_selection_without_ofi;
    }
    
    void print() const {
        std::cout << "=== OFI Validation Results ===\n"
                  << "                    With OFI    Without OFI\n"
                  << "  Sharpe:           " << sharpe_with_ofi << "        " << sharpe_without_ofi << "\n"
                  << "  Trades:           " << trades_with_ofi << "          " << trades_without_ofi << "\n"
                  << "  Adverse Sel:      " << adverse_selection_with_ofi << "     " << adverse_selection_without_ofi << "\n"
                  << "  PnL:              " << pnl_with_ofi << "        " << pnl_without_ofi << "\n"
                  << "\n"
                  << "  Sharpe improvement: " << sharpe_improvement() * 100 << "%\n"
                  << "  Trade reduction:    " << trade_reduction() * 100 << "%\n"
                  << "  AS reduction:       " << as_reduction() * 100 << "%\n";
    }
};

/**
 * @brief OFI validation framework.
 * 
 * Runs parallel simulations with and without OFI signal gating
 * to measure true value of the signal.
 */
class OFIValidator {
public:
    /**
     * @brief Run A/B test comparing OFI vs no OFI.
     * 
     * @param zScores Z-score signal series
     * @param ofiValues OFI signal series (normalized)
     * @param prices Mid prices for mark-to-market
     * @param entryThreshold Z-score entry threshold
     * @param ofiThreshold OFI confirmation threshold
     */
    OFIValidationResult validate(const std::vector<double>& zScores,
                                  const std::vector<double>& ofiValues,
                                  const std::vector<double>& prices,
                                  double entryThreshold = 2.0,
                                  double ofiThreshold = 0.3) {
        OFIValidationResult result;
        
        // Run with OFI
        auto [pnlWithOfi, tradesWithOfi, asWithOfi] = 
            runSimulation(zScores, ofiValues, prices, entryThreshold, ofiThreshold, true);
        
        // Run without OFI
        auto [pnlWithoutOfi, tradesWithoutOfi, asWithoutOfi] = 
            runSimulation(zScores, ofiValues, prices, entryThreshold, ofiThreshold, false);
        
        result.pnl_with_ofi = pnlWithOfi.back();
        result.trades_with_ofi = tradesWithOfi;
        result.adverse_selection_with_ofi = asWithOfi;
        result.sharpe_with_ofi = computeSharpe(pnlWithOfi);
        
        result.pnl_without_ofi = pnlWithoutOfi.back();
        result.trades_without_ofi = tradesWithoutOfi;
        result.adverse_selection_without_ofi = asWithoutOfi;
        result.sharpe_without_ofi = computeSharpe(pnlWithoutOfi);
        
        return result;
    }

private:
    struct SimResult {
        std::vector<double> pnlCurve;
        int trades;
        double adverseSelection;
    };

    SimResult runSimulation(const std::vector<double>& zScores,
                            const std::vector<double>& ofiValues,
                            const std::vector<double>& prices,
                            double entryThreshold,
                            double ofiThreshold,
                            bool useOfi) {
        SimResult result;
        result.pnlCurve.push_back(0);
        result.trades = 0;
        result.adverseSelection = 0;
        
        int position = 0;  // -1, 0, 1
        double entryPrice = 0;
        double exitThreshold = entryThreshold * 0.25;  // Exit when z reverts
        
        for (size_t i = 0; i < zScores.size(); ++i) {
            double z = zScores[i];
            double ofi = i < ofiValues.size() ? ofiValues[i] : 0;
            double price = prices[i];
            
            if (position == 0) {
                // Entry conditions
                bool enterLong = (z < -entryThreshold);
                bool enterShort = (z > entryThreshold);
                
                // OFI confirmation (if enabled)
                if (useOfi) {
                    enterLong = enterLong && (ofi > -ofiThreshold);  // Not too bearish
                    enterShort = enterShort && (ofi < ofiThreshold);  // Not too bullish
                }
                
                if (enterLong) {
                    position = 1;
                    entryPrice = price;
                    result.trades++;
                } else if (enterShort) {
                    position = -1;
                    entryPrice = price;
                    result.trades++;
                }
            } else {
                // Exit conditions
                bool exitLong = (position == 1 && z > -exitThreshold);
                bool exitShort = (position == -1 && z < exitThreshold);
                
                if (exitLong || exitShort) {
                    double pnl = position * (price - entryPrice);
                    result.pnlCurve.push_back(result.pnlCurve.back() + pnl);
                    
                    // Track adverse selection (price moved against us)
                    if (pnl < 0) {
                        result.adverseSelection += std::abs(pnl);
                    }
                    
                    position = 0;
                    result.trades++;
                } else {
                    // Mark-to-market
                    double mtm = position * (price - entryPrice);
                    result.pnlCurve.push_back(result.pnlCurve.back() + mtm - 
                                              (result.pnlCurve.size() > 1 ? 
                                               result.pnlCurve[result.pnlCurve.size()-2] : 0));
                }
            }
        }
        
        return result;
    }

    double computeSharpe(const std::vector<double>& pnlCurve) {
        if (pnlCurve.size() < 20) return 0;
        
        std::vector<double> returns;
        for (size_t i = 1; i < pnlCurve.size(); ++i) {
            returns.push_back(pnlCurve[i] - pnlCurve[i-1]);
        }
        
        double mean = std::accumulate(returns.begin(), returns.end(), 0.0) / returns.size();
        double sq_sum = 0;
        for (double r : returns) sq_sum += (r - mean) * (r - mean);
        double std = std::sqrt(sq_sum / returns.size());
        
        if (std < 1e-10) return 0;
        return (mean / std) * std::sqrt(252 * 6.5 * 60);  // Annualized (1-min ticks)
    }
};

} // namespace analytics
