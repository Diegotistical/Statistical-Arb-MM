/**
 * @file replay_tests.cpp
 * @brief Tests for LOBSTER replay functionality.
 * 
 * Tests:
 *   - Parser correctness
 *   - Replay determinism
 *   - Book invariant validation
 *   - Synthetic data replay
 */

#include <cassert>
#include <fstream>
#include <iostream>
#include <sstream>

#include "../../src/replay/LobsterParser.hpp"
#include "../../src/replay/ReplayEngine.hpp"

// ============================================================================
// Test Framework
// ============================================================================

static int testsRun = 0;
static int testsPassed = 0;

#define TEST(name) void name()
#define RUN_TEST(name) do { \
    testsRun++; \
    std::cout << "  Running: " << #name << "... "; \
    try { name(); testsPassed++; std::cout << "PASS\n"; } \
    catch (const std::exception& e) { std::cout << "FAIL: " << e.what() << "\n"; } \
} while(0)

#define ASSERT(cond) do { if (!(cond)) throw std::runtime_error("Assertion failed: " #cond); } while(0)
#define ASSERT_EQ(a, b) do { if ((a) != (b)) throw std::runtime_error("Assertion failed: " #a " == " #b); } while(0)

// ============================================================================
// Helper: Create temporary LOBSTER file
// ============================================================================

std::string createTempLobsterFile(const std::string& content) {
    std::string path = "temp_lobster_test.csv";
    std::ofstream file(path);
    file << content;
    file.close();
    return path;
}

void removeTempFile(const std::string& path) {
    std::remove(path.c_str());
}

// ============================================================================
// Tests
// ============================================================================

TEST(test_parser_basic) {
    // Create test data (LOBSTER format)
    // Time, Type, OrderID, Size, Price, Direction
    std::string data = 
        "34200.000000001,1,12345,100,1502500,1\n"   // Buy limit @ $150.25
        "34200.000000002,1,12346,200,1502600,-1\n"  // Sell limit @ $150.26
        "34200.000000003,3,12345,100,1502500,1\n";  // Cancel order 12345
    
    auto path = createTempLobsterFile(data);
    
    replay::LobsterParser parser(path);
    auto messages = parser.parseAll();
    
    ASSERT_EQ(messages.size(), 3u);
    
    // Check first message
    ASSERT_EQ(static_cast<int>(messages[0].type), 1);
    ASSERT_EQ(messages[0].order_id, 12345);
    ASSERT_EQ(messages[0].size, 100);
    ASSERT_EQ(messages[0].price, 1502500);
    ASSERT_EQ(messages[0].direction, 1);
    ASSERT(messages[0].isBuy());
    
    // Check second message
    ASSERT(messages[1].isSell());
    
    // Check third message (cancellation)
    ASSERT_EQ(static_cast<int>(messages[2].type), 3);
    
    removeTempFile(path);
}

TEST(test_replay_basic) {
    std::string data = 
        "34200.000000001,1,100,100,5000000,1\n"    // Buy @ $500.00
        "34200.000000002,1,101,100,5001000,-1\n"   // Sell @ $500.10
        "34200.000000003,1,102,50,5000500,1\n";    // Buy @ $500.05
    
    auto path = createTempLobsterFile(data);
    
    replay::LobsterParser parser(path);
    replay::ReplayEngine engine;
    
    auto stats = engine.replay(parser);
    
    ASSERT_EQ(stats.messages_processed, 3u);
    ASSERT_EQ(stats.orders_added, 3u);
    ASSERT_EQ(stats.invariant_violations, 0u);
    
    // Check book state
    const auto& book = engine.book();
    ASSERT(book.hasBids());
    ASSERT(book.hasAsks());
    
    removeTempFile(path);
}

TEST(test_replay_determinism) {
    std::string data = 
        "34200.000000001,1,100,100,5000000,1\n"
        "34200.000000002,1,101,100,5001000,-1\n"
        "34200.000000003,1,102,50,5000500,1\n"
        "34200.000000004,3,100,100,5000000,1\n";  // Cancel
    
    auto path = createTempLobsterFile(data);
    
    // Run 1
    replay::LobsterParser parser1(path);
    replay::ReplayEngine engine1;
    engine1.replay(parser1);
    auto hash1 = engine1.getStateHash();
    
    // Run 2
    replay::LobsterParser parser2(path);
    replay::ReplayEngine engine2;
    engine2.replay(parser2);
    auto hash2 = engine2.getStateHash();
    
    // Hashes must match
    ASSERT(hash1 == hash2);
    
    removeTempFile(path);
}

TEST(test_invariant_no_crossed_book) {
    // After replay, book should not be crossed
    std::string data = 
        "34200.000000001,1,100,100,5000000,1\n"    // Bid @ $500.00
        "34200.000000002,1,101,100,5001000,-1\n";  // Ask @ $500.10
    
    auto path = createTempLobsterFile(data);
    
    replay::LobsterParser parser(path);
    replay::ReplayEngine engine;
    auto stats = engine.replay(parser);
    
    const auto& book = engine.book();
    
    // Best bid should be less than best ask
    if (book.hasBids() && book.hasAsks()) {
        ASSERT(book.getBestBid() < book.getBestAsk());
    }
    
    ASSERT_EQ(stats.invariant_violations, 0u);
    
    removeTempFile(path);
}

TEST(test_cancellation_flow) {
    std::string data = 
        "34200.000000001,1,100,100,5000000,1\n"    // Add
        "34200.000000002,3,100,100,5000000,1\n";   // Cancel (total)
    
    auto path = createTempLobsterFile(data);
    
    replay::LobsterParser parser(path);
    replay::ReplayEngine engine;
    auto stats = engine.replay(parser);
    
    ASSERT_EQ(stats.orders_added, 1u);
    ASSERT_EQ(stats.orders_cancelled, 1u);
    
    // Book should be empty after cancel
    const auto& book = engine.book();
    ASSERT(!book.hasBids());
    
    removeTempFile(path);
}

// ============================================================================
// Main
// ============================================================================

int main() {
    std::cout << "============================================\n";
    std::cout << "   LOBSTER Replay Tests\n";
    std::cout << "============================================\n\n";

    RUN_TEST(test_parser_basic);
    RUN_TEST(test_replay_basic);
    RUN_TEST(test_replay_determinism);
    RUN_TEST(test_invariant_no_crossed_book);
    RUN_TEST(test_cancellation_flow);

    std::cout << "\n============================================\n";
    std::cout << "   Results: " << testsPassed << "/" << testsRun << " passed\n";
    std::cout << "============================================\n";

    return (testsPassed == testsRun) ? 0 : 1;
}
