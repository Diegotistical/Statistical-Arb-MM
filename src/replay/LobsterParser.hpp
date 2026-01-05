#pragma once
/**
 * @file LobsterParser.hpp
 * @brief Parser for LOBSTER L3 market data format.
 * 
 * LOBSTER format (CSV):
 *   Time,EventType,OrderID,Size,Price,Direction
 * 
 * Event types:
 *   1 = Limit order submission
 *   2 = Partial cancellation
 *   3 = Total cancellation  
 *   4 = Visible limit order execution
 *   5 = Hidden limit order execution
 *   6 = Cross trade
 *   7 = Trading halt
 * 
 * Direction:
 *   1 = Buy
 *  -1 = Sell
 * 
 * @see https://lobsterdata.com/info/DataStructure.php
 */

#include <cstdint>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace replay {

/**
 * @brief LOBSTER message types.
 */
enum class LobsterEventType : uint8_t {
    LimitOrderSubmission = 1,
    PartialCancellation = 2,
    TotalCancellation = 3,
    VisibleExecution = 4,
    HiddenExecution = 5,
    CrossTrade = 6,
    TradingHalt = 7
};

/**
 * @brief Parsed LOBSTER message.
 */
struct LobsterMessage {
    int64_t timestamp_ns;      ///< Time since midnight in nanoseconds
    LobsterEventType type;     ///< Event type
    int64_t order_id;          ///< Order ID
    int32_t size;              ///< Size (shares)
    int64_t price;             ///< Price (in price ticks, e.g., cents)
    int8_t direction;          ///< 1 = Buy, -1 = Sell
    
    [[nodiscard]] bool isBuy() const noexcept { return direction == 1; }
    [[nodiscard]] bool isSell() const noexcept { return direction == -1; }
};

/**
 * @brief Parser for LOBSTER L3 data files.
 * 
 * Usage:
 * @code
 *   LobsterParser parser("AAPL_2019-01-02_message.csv");
 *   while (auto msg = parser.next()) {
 *       processMessage(*msg);
 *   }
 * @endcode
 */
class LobsterParser {
public:
    /**
     * @brief Construct parser with file path.
     * @param filepath Path to LOBSTER message file (CSV)
     */
    explicit LobsterParser(const std::string& filepath)
        : file_(filepath), lineNumber_(0) {
        if (!file_.is_open()) {
            throw std::runtime_error("Cannot open file: " + filepath);
        }
    }

    /**
     * @brief Parse next message from file.
     * @return Parsed message, or nullopt if EOF or error
     */
    [[nodiscard]] std::optional<LobsterMessage> next() {
        std::string line;
        if (!std::getline(file_, line)) {
            return std::nullopt;
        }
        lineNumber_++;
        return parseLine(line);
    }

    /**
     * @brief Parse all messages into vector.
     * @return Vector of all messages
     */
    [[nodiscard]] std::vector<LobsterMessage> parseAll() {
        std::vector<LobsterMessage> messages;
        messages.reserve(1'000'000);  // Typical day ~1M messages
        
        while (auto msg = next()) {
            messages.push_back(*msg);
        }
        return messages;
    }

    /**
     * @brief Get current line number (for error reporting).
     */
    [[nodiscard]] size_t lineNumber() const noexcept { return lineNumber_; }

    /**
     * @brief Reset parser to beginning of file.
     */
    void reset() {
        file_.clear();
        file_.seekg(0, std::ios::beg);
        lineNumber_ = 0;
    }

private:
    std::ifstream file_;
    size_t lineNumber_;

    [[nodiscard]] std::optional<LobsterMessage> parseLine(const std::string& line) {
        // Format: Time,Type,OrderID,Size,Price,Direction
        std::istringstream ss(line);
        std::string token;
        
        LobsterMessage msg{};
        
        try {
            // Time (seconds with 9 decimal places → nanoseconds)
            if (!std::getline(ss, token, ',')) return std::nullopt;
            double time_sec = std::stod(token);
            msg.timestamp_ns = static_cast<int64_t>(time_sec * 1e9);
            
            // Event type
            if (!std::getline(ss, token, ',')) return std::nullopt;
            msg.type = static_cast<LobsterEventType>(std::stoi(token));
            
            // Order ID
            if (!std::getline(ss, token, ',')) return std::nullopt;
            msg.order_id = std::stoll(token);
            
            // Size
            if (!std::getline(ss, token, ',')) return std::nullopt;
            msg.size = std::stoi(token);
            
            // Price (in hundredths of cents, divide by 10000 for dollars)
            if (!std::getline(ss, token, ',')) return std::nullopt;
            msg.price = std::stoll(token);
            
            // Direction
            if (!std::getline(ss, token, ',')) return std::nullopt;
            msg.direction = static_cast<int8_t>(std::stoi(token));
            
        } catch (...) {
            return std::nullopt;
        }
        
        return msg;
    }
};

} // namespace replay
