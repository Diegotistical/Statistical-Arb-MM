#include "Exchange.hpp"

#include <iostream>
#include <stdexcept>

#ifdef _WIN32
    #include <windows.h>
#else
    #include <pthread.h>
    #include <sched.h>
#endif

// ============================================================================
// Constructor - Initialize shards and worker threads
// ============================================================================
Exchange::Exchange(int numWorkers) {
    // Default to hardware concurrency if not specified
    if (numWorkers <= 0) {
        numWorkers = static_cast<int>(std::thread::hardware_concurrency());
        if (numWorkers <= 0) numWorkers = 4; // Fallback
    }

    // Create shards
    shards_.reserve(numWorkers);
    for (int i = 0; i < numWorkers; ++i) {
        shards_.push_back(std::make_unique<Shard>());
    }

    // Start worker threads
    workers_.reserve(numWorkers);
    for (int i = 0; i < numWorkers; ++i) {
        workers_.emplace_back([this, i]() {
            workerLoop(i);
        });
    }
}

// ============================================================================
// Destructor - Stop all workers
// ============================================================================
Exchange::~Exchange() {
    stop();
}

// ============================================================================
// submitOrder - Route order to appropriate shard
// ============================================================================
void Exchange::submitOrder(const Order& order, int shardHint) {
    // Determine shard (by symbol or hint)
    int shardId = shardHint;
    if (shardId < 0 || shardId >= static_cast<int>(shards_.size())) {
        if (order.symbolId >= 0 && 
            static_cast<size_t>(order.symbolId) < symbolIdToShardId_.size()) {
            shardId = symbolIdToShardId_[order.symbolId];
        } else {
            // Hash-based distribution
            shardId = static_cast<int>(order.symbolId % shards_.size());
        }
    }

    Command cmd;
    cmd.type = Command::Add;
    cmd.add.order = order;
    
    shards_[shardId]->queue.push_blocking(cmd);
}

// ============================================================================
// submitOrders - Batch order submission
// ============================================================================
void Exchange::submitOrders(const std::vector<Order>& orders, int shardHint) {
    for (const auto& order : orders) {
        submitOrder(order, shardHint);
    }
}

// ============================================================================
// cancelOrder - Send cancel command
// ============================================================================
void Exchange::cancelOrder(int32_t symbolId, OrderId orderId) {
    int shardId = 0;
    if (symbolId >= 0 && 
        static_cast<size_t>(symbolId) < symbolIdToShardId_.size()) {
        shardId = symbolIdToShardId_[symbolId];
    }

    Command cmd;
    cmd.type = Command::Cancel;
    cmd.cancel.orderId = orderId;
    cmd.cancel.symbolId = symbolId;
    
    shards_[shardId]->queue.push_blocking(cmd);
}

// ============================================================================
// stop - Signal all workers to stop
// ============================================================================
void Exchange::stop() {
    Command cmd;
    cmd.type = Command::Stop;
    
    for (auto& shard : shards_) {
        shard->queue.push_blocking(cmd);
    }

    // jthreads automatically join on destruction
    workers_.clear();
}

// ============================================================================
// flush - Wait for queues to drain
// ============================================================================
void Exchange::flush() {
    bool allEmpty = false;
    while (!allEmpty) {
        allEmpty = true;
        for (auto& shard : shards_) {
            if (shard->queue.size() > 0) {
                allEmpty = false;
                std::this_thread::yield();
                break;
            }
        }
    }
}

// ============================================================================
// drain - Alias for flush
// ============================================================================
void Exchange::drain() {
    flush();
}

// ============================================================================
// reset - Reset all order books
// ============================================================================
void Exchange::reset() {
    Command cmd;
    cmd.type = Command::Reset;
    
    for (auto& shard : shards_) {
        shard->queue.push_blocking(cmd);
    }
    
    flush();
}

// ============================================================================
// registerSymbol - Register a trading symbol
// ============================================================================
int32_t Exchange::registerSymbol(const std::string& symbol, int shardId) {
    auto it = symbolNameToId_.find(symbol);
    if (it != symbolNameToId_.end()) {
        return it->second;
    }

    int32_t id = static_cast<int32_t>(symbolIdToName_.size());
    symbolNameToId_[symbol] = id;
    symbolIdToName_.push_back(symbol);
    
    // Ensure shard has an order book for this symbol
    if (shardId < 0 || shardId >= static_cast<int>(shards_.size())) {
        shardId = id % static_cast<int>(shards_.size());
    }
    symbolIdToShardId_.push_back(shardId);
    
    // Create order book if needed
    while (shards_[shardId]->books.size() <= static_cast<size_t>(id)) {
        shards_[shardId]->books.push_back(std::make_unique<OrderBook>());
    }

    return id;
}

// ============================================================================
// getSymbolName - Get symbol name by ID
// ============================================================================
std::string Exchange::getSymbolName(int32_t symbolId) const {
    if (symbolId >= 0 && 
        static_cast<size_t>(symbolId) < symbolIdToName_.size()) {
        return symbolIdToName_[symbolId];
    }
    return "";
}

// ============================================================================
// setTradeCallback - Set callback for trade events
// ============================================================================
void Exchange::setTradeCallback(TradeCallback cb) {
    onTrade_ = std::move(cb);
}

// ============================================================================
// printOrderBook - Print order book for symbol
// ============================================================================
void Exchange::printOrderBook(int32_t symbolId) const {
    const OrderBook* book = getOrderBook(symbolId);
    if (book) {
        book->printBook();
    }
}

// ============================================================================
// printAllOrderBooks - Print all order books
// ============================================================================
void Exchange::printAllOrderBooks() const {
    for (size_t i = 0; i < symbolIdToName_.size(); ++i) {
        std::cout << "Symbol: " << symbolIdToName_[i] << "\n";
        printOrderBook(static_cast<int32_t>(i));
    }
}

// ============================================================================
// getOrderBook - Get order book for symbol
// ============================================================================
const OrderBook* Exchange::getOrderBook(int32_t symbolId) const {
    if (symbolId < 0 || 
        static_cast<size_t>(symbolId) >= symbolIdToShardId_.size()) {
        return nullptr;
    }
    
    int shardId = symbolIdToShardId_[symbolId];
    if (shardId < 0 || 
        static_cast<size_t>(shardId) >= shards_.size()) {
        return nullptr;
    }
    
    if (static_cast<size_t>(symbolId) >= shards_[shardId]->books.size()) {
        return nullptr;
    }
    
    return shards_[shardId]->books[symbolId].get();
}

// ============================================================================
// pinThread - Pin thread to specific CPU core
// ============================================================================
void Exchange::pinThread(int coreId) {
#ifdef _WIN32
    DWORD_PTR mask = 1ULL << coreId;
    SetThreadAffinityMask(GetCurrentThread(), mask);
#elif defined(__linux__)
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(coreId, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
#elif defined(__APPLE__)
    // macOS doesn't support thread affinity
    (void)coreId;
#endif
}

// ============================================================================
// workerLoop - Main processing loop for each shard
// ============================================================================
void Exchange::workerLoop(int shardId) {
    // Pin thread to core for cache locality
    pinThread(shardId);

    Shard& shard = *shards_[shardId];
    Command cmd;

    while (true) {
        // Pop command from queue
        if (!shard.queue.pop(cmd)) {
            // Queue empty - spin/yield
            std::this_thread::yield();
            continue;
        }

        switch (cmd.type) {
            case Command::Add: {
                Order& order = cmd.add.order;
                
                // Ensure order book exists
                while (shard.books.size() <= static_cast<size_t>(order.symbolId)) {
                    shard.books.push_back(std::make_unique<OrderBook>());
                }

                OrderBook& book = *shard.books[order.symbolId];
                shard.tradeBuffer.clear();
                
                // Match order
                shard.matchingStrategy.match(book, order, shard.tradeBuffer);
                
                // Fire trade callback
                if (!shard.tradeBuffer.empty() && onTrade_) {
                    onTrade_(shard.tradeBuffer);
                }
                break;
            }

            case Command::Cancel: {
                if (static_cast<size_t>(cmd.cancel.symbolId) < shard.books.size()) {
                    shard.books[cmd.cancel.symbolId]->cancelOrder(cmd.cancel.orderId);
                }
                break;
            }

            case Command::Reset: {
                for (auto& book : shard.books) {
                    book->reset();
                }
                break;
            }

            case Command::Stop:
                return;
        }
    }
}
