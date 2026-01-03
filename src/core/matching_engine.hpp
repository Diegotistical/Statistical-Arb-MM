#pragma once
#include <vector>
#include <iostream>
#include <concepts>

// C++20 Concept: Restrict templates to floating point types only
template<typename T>
concept Decimal = std::floating_point<T>;

class MatchingEngine {
public:
    MatchingEngine();

    // Template methods must be implemented in the header
    void log_execution(Decimal auto price, Decimal auto qty) {
        std::cout << "[Engine] Executed " << qty << " @ " << price << std::endl;
    }

    double calculate_spread(const std::vector<double>& prices_a, const std::vector<double>& prices_b);
    void print_status();
};