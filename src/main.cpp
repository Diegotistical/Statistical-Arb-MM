#include <iostream>
#include <vector>
#include "src/core/matching_engine.hpp" 

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "   Stat-Arb-MM Standalone Runner" << std::endl;
    std::cout << "========================================" << std::endl;

    MatchingEngine engine;
    engine.print_status();

    std::vector<double> prices_a(1000, 100.0);
    std::vector<double> prices_b(1000, 101.5);

    std::cout << "[Runner] Benchmarking spread calculation..." << std::endl;
    double spread = engine.calculate_spread(prices_a, prices_b);

    std::cout << "[Runner] Result: Spread is " << spread << std::endl;
    
    // Example of template instantiation usage
    engine.log_execution(100.5, 50.0);

    std::cout << "[Runner] Done." << std::endl;
    return 0;
}