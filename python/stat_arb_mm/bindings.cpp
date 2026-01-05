/**
 * @file bindings.cpp
 * @brief pybind11 bindings for Stat-Arb MM engine.
 * 
 * Exposes to Python:
 *   - OrderBook, Order, Trade
 *   - MatchingStrategy
 *   - SpreadModel, OFI
 *   - TickNormalizer
 */

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/numpy.h>

#include "../../src/core/OrderBook.hpp"
#include "../../src/core/MatchingStrategy.hpp"
#include "../../src/core/TickNormalizer.hpp"
#include "../../src/signals/SpreadModel.hpp"
#include "../../src/signals/OFI.hpp"

namespace py = pybind11;

PYBIND11_MODULE(stat_arb_mm, m) {
    m.doc() = "Statistical Arbitrage Market Making Simulator";

    // ========================================================================
    // Enums
    // ========================================================================
    
    py::enum_<OrderSide>(m, "Side")
        .value("Buy", OrderSide::Buy)
        .value("Sell", OrderSide::Sell)
        .export_values();

    py::enum_<OrderType>(m, "Type")
        .value("Limit", OrderType::Limit)
        .value("Market", OrderType::Market)
        .export_values();

    py::enum_<TimeInForce>(m, "TIF")
        .value("GTC", TimeInForce::GTC)
        .value("IOC", TimeInForce::IOC)
        .value("FOK", TimeInForce::FOK)
        .export_values();

    // ========================================================================
    // Order
    // ========================================================================
    
    py::class_<Order>(m, "Order")
        .def(py::init<OrderId, uint32_t, int32_t, OrderSide, OrderType, Price, Quantity>(),
             py::arg("id"), py::arg("client_order_id"), py::arg("symbol_id"),
             py::arg("side"), py::arg("type"), py::arg("price"), py::arg("quantity"))
        .def_readwrite("id", &Order::id)
        .def_readwrite("price", &Order::price)
        .def_readwrite("quantity", &Order::quantity)
        .def_readwrite("side", &Order::side)
        .def_readwrite("type", &Order::type)
        .def_readonly("active", &Order::active)
        .def("is_active", &Order::isActive)
        .def("is_buy", &Order::isBuy)
        .def("__repr__", [](const Order& o) {
            return "<Order id=" + std::to_string(o.id) + 
                   " price=" + std::to_string(o.price) +
                   " qty=" + std::to_string(o.quantity) +
                   (o.isBuy() ? " BUY" : " SELL") + ">";
        });

    // ========================================================================
    // Trade
    // ========================================================================
    
    py::class_<Trade>(m, "Trade")
        .def_readonly("symbol_id", &Trade::symbolId)
        .def_readonly("price", &Trade::price)
        .def_readonly("quantity", &Trade::quantity)
        .def_readonly("maker_order_id", &Trade::makerOrderId)
        .def_readonly("taker_order_id", &Trade::takerOrderId)
        .def("__repr__", [](const Trade& t) {
            return "<Trade " + std::to_string(t.quantity) + 
                   " @ " + std::to_string(t.price) + ">";
        });

    // ========================================================================
    // OrderBook
    // ========================================================================
    
    py::class_<OrderBook>(m, "OrderBook")
        .def(py::init<>())
        .def("add_order", py::overload_cast<const Order&>(&OrderBook::addOrder),
             py::arg("order"))
        .def("cancel_order", &OrderBook::cancelOrder, py::arg("order_id"))
        .def("modify_order", &OrderBook::modifyOrder, 
             py::arg("order_id"), py::arg("new_quantity"))
        .def("reset", &OrderBook::reset)
        .def_property_readonly("best_bid", &OrderBook::getBestBid)
        .def_property_readonly("best_ask", &OrderBook::getBestAsk)
        .def_property_readonly("has_bids", &OrderBook::hasBids)
        .def_property_readonly("has_asks", &OrderBook::hasAsks)
        .def_property_readonly("generation", &OrderBook::generation)
        .def("get_queue_position", &OrderBook::getQueuePosition, py::arg("order_id"))
        .def("mid_price", [](const OrderBook& b) {
            if (!b.hasBids() || !b.hasAsks()) return -1.0;
            return (static_cast<double>(b.getBestBid()) + b.getBestAsk()) / 2.0;
        })
        .def("spread", [](const OrderBook& b) {
            if (!b.hasBids() || !b.hasAsks()) return -1.0;
            return static_cast<double>(b.getBestAsk() - b.getBestBid());
        })
        .def("__repr__", [](const OrderBook& b) {
            return "<OrderBook bid=" + std::to_string(b.getBestBid()) +
                   " ask=" + std::to_string(b.getBestAsk()) + ">";
        });

    // ========================================================================
    // MatchingStrategy
    // ========================================================================
    
    py::class_<StandardMatchingStrategy>(m, "MatchingStrategy")
        .def(py::init<>())
        .def("match", [](StandardMatchingStrategy& s, OrderBook& book, Order& order) {
            std::vector<Trade> trades;
            s.match(book, order, trades);
            return trades;
        }, py::arg("book"), py::arg("order"));

    // ========================================================================
    // TickNormalizer
    // ========================================================================
    
    py::class_<TickNormalizer>(m, "TickNormalizer")
        .def(py::init<double>(), py::arg("tick_size") = 0.01)
        .def("to_ticks", &TickNormalizer::toTicks, py::arg("price_usd"))
        .def("to_price", &TickNormalizer::toPrice, py::arg("ticks"))
        .def_property_readonly("tick_size", &TickNormalizer::tickSize);

    // ========================================================================
    // SpreadModel
    // ========================================================================
    
    py::class_<signals::SpreadModel>(m, "SpreadModel")
        .def(py::init<size_t, double>(), 
             py::arg("lookback") = 100, py::arg("beta") = 1.0)
        .def("update", &signals::SpreadModel::update,
             py::arg("price_a"), py::arg("price_b"))
        .def_property_readonly("z_score", &signals::SpreadModel::zScore)
        .def_property_readonly("spread", &signals::SpreadModel::spread)
        .def_property_readonly("mean", &signals::SpreadModel::mean)
        .def_property_readonly("std", &signals::SpreadModel::stdDev)
        .def("reset", &signals::SpreadModel::reset);

    // ========================================================================
    // OFI (Order Flow Imbalance)
    // ========================================================================
    
    py::class_<signals::OFI>(m, "OFI")
        .def(py::init<size_t>(), py::arg("window") = 100)
        .def("update", &signals::OFI::update,
             py::arg("bid_size"), py::arg("ask_size"))
        .def_property_readonly("value", &signals::OFI::value)
        .def_property_readonly("normalized", &signals::OFI::normalized)
        .def("reset", &signals::OFI::reset);

    // ========================================================================
    // Constants
    // ========================================================================
    
    m.attr("MAX_PRICE") = OrderBook::MAX_PRICE;
    m.attr("MAX_ORDER_ID") = OrderBook::MAX_ORDER_ID;
    m.attr("PRICE_INVALID") = PRICE_INVALID;
}