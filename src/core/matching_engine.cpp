#include <iostream>
#include <vector>
#include <numeric>
#include <concepts> // C++20 Feature

// C++20 Concept: Restrict templates to floating point types only
template<typename T>
concept Decimal = std::floating_point<T>;

class MatchingEngine {
public:
    MatchingEngine() {}

    // Example of C++20 'auto' syntax with concepts
    void log_execution(Decimal auto price, Decimal auto qty) {
        std::cout << "[Engine] Executed " << qty << " @ " << price << std::endl;
    }

    double calculate_spread(const std::vector<double>& prices_a, const std::vector<double>& prices_b) {
        if (prices_a.size() != prices_b.size()) return 0.0;
        
        // Simple mean spread calc
        double sum_diff = 0.0;
        for (size_t i = 0; i < prices_a.size(); ++i) {
            sum_diff += (prices_a[i] - prices_b[i]);
        }
        return sum_diff / prices_a.size();
    }
    
    void print_status() {
        std::cout << "[C++20 Engine] Core is online." << std::endl;
    }
};
