#pragma once

#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "MatchingStrategy.hpp"
#include "OrderBook.hpp"
#include "RingBuffer.hpp"

// ============================================================================
// Exchange - Shard-per-core architecture for lock-free trading
// ============================================================================
class Exchange {
public:
    using TradeCallback = std::function<void(const std::vector<Trade>&)>;

    // Command types for the ring buffer
    struct Command {
        enum Type : uint8_t { Add, Cancel, Stop, Reset } type;
        
        union {
            struct {
                Order order;
            } add;
            struct {
                OrderId orderId;
                int32_t symbolId;
            } cancel;
        };

        Command() : type(Add) { 
            std::memset(&add, 0, sizeof(add)); 
        }
    };

    // ========================================================================
    // Construction / Destruction
    // ========================================================================
    
    explicit Exchange(int numWorkers = 0);
    ~Exchange();

    // No copying or moving
    Exchange(const Exchange&) = delete;
    Exchange& operator=(const Exchange&) = delete;
    Exchange(Exchange&&) = delete;
    Exchange& operator=(Exchange&&) = delete;

    // ========================================================================
    // Order Submission
    // ========================================================================
    
    void submitOrder(const Order& order, int shardHint = -1);
    void submitOrders(const std::vector<Order>& orders, int shardHint = -1);
    void cancelOrder(int32_t symbolId, OrderId orderId);

    // ========================================================================
    // Control
    // ========================================================================
    
    void stop();
    void flush();
    void drain();
    void reset();

    // ========================================================================
    // Symbol Management
    // ========================================================================
    
    int32_t registerSymbol(const std::string& symbol, int shardId);
    [[nodiscard]] std::string getSymbolName(int32_t symbolId) const;

    // ========================================================================
    // Callbacks & Monitoring
    // ========================================================================
    
    void setTradeCallback(TradeCallback cb);
    void printOrderBook(int32_t symbolId) const;
    void printAllOrderBooks() const;
    [[nodiscard]] const OrderBook* getOrderBook(int32_t symbolId) const;

    // ========================================================================
    // Thread Affinity
    // ========================================================================
    
    static void pinThread(int coreId);

private:
    // Shard structure - each core owns one shard
    struct Shard {
        RingBuffer<Command> queue{65536};
        std::vector<std::unique_ptr<OrderBook>> books;
        StandardMatchingStrategy matchingStrategy;
        std::vector<Trade> tradeBuffer;
    };

    void workerLoop(int shardId);

    std::vector<std::unique_ptr<Shard>> shards_;
    std::vector<std::jthread> workers_;
    TradeCallback onTrade_;

    std::unordered_map<std::string, int32_t> symbolNameToId_;
    std::vector<std::string> symbolIdToName_;
    std::vector<int> symbolIdToShardId_;
};
