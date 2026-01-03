#include "matching_engine.hpp"
#include <numeric>

MatchingEngine::MatchingEngine() {}

double MatchingEngine::calculate_spread(const std::vector<double>& prices_a, const std::vector<double>& prices_b) {
    if (prices_a.size() != prices_b.size()) return 0.0;
    
    // Simple mean spread calc
    double sum_diff = 0.0;
    for (size_t i = 0; i < prices_a.size(); ++i) {
        sum_diff += (prices_a[i] - prices_b[i]);
    }
    return sum_diff / prices_a.size();
}

void MatchingEngine::print_status() {
    std::cout << "[C++20 Engine] Core is online." << std::endl;
}