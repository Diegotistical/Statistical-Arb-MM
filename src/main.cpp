#include <iostream>
#include <vector>
#include "core/matching_engine.cpp" // Direct include for MVP (in prod, use headers)

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "   Stat-Arb-MM Standalone Runner" << std::endl;
    std::cout << "========================================" << std::endl;

    // Instantiate the engine
    MatchingEngine engine;
    engine.print_status();

    // Create dummy data (1000 data points)
    std::vector<double> prices_a(1000, 100.0);
    std::vector<double> prices_b(1000, 101.5);

    // Run the calculation
    std::cout << "[Runner] Benchmarking spread calculation..." << std::endl;
    double spread = engine.calculate_spread(prices_a, prices_b);

    std::cout << "[Runner] Result: Spread is " << spread << std::endl;
    std::cout << "[Runner] Done." << std::endl;

    return 0;
}
