/**
 * @file bindings.cpp
 * @brief pybind11 bindings for Stat-Arb MM engine.
 *
 * Exposes to Python:
 *   - Core: OrderBook, Order, Trade, MatchingStrategy
 *   - Signals: SpreadModel, KalmanHedgeRatio, OFI, MultiLevelOFI, VPIN, KyleLambda
 *   - Strategy: StatArbMM
 *   - Risk: RiskManager
 *   - Analytics: CointegrationAnalyzer, PnLAnalytics
 *   - Execution: TransactionCostModel
 *   - Backtest: Simulator
 */

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/numpy.h>

#include "../../src/core/OrderBook.hpp"
#include "../../src/core/MatchingStrategy.hpp"
#include "../../src/core/TickNormalizer.hpp"
#include "../../src/signals/SpreadModel.hpp"
#include "../../src/signals/KalmanFilter.hpp"
#include "../../src/signals/OFI.hpp"
#include "../../src/signals/VPIN.hpp"
#include "../../src/signals/KyleLambda.hpp"
#include "../../src/strategy/StatArbMM.hpp"
#include "../../src/risk/RiskManager.hpp"
#include "../../src/analytics/CointegrationTests.hpp"
#include "../../src/analytics/PnLAnalytics.hpp"
#include "../../src/execution/TransactionCosts.hpp"
#include "../../src/backtest/Simulator.hpp"

namespace py = pybind11;

PYBIND11_MODULE(stat_arb_mm, m) {
    m.doc() = "Statistical Arbitrage Market Making Engine";

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

    py::enum_<risk::RiskCheckResult>(m, "RiskCheckResult")
        .value("Passed", risk::RiskCheckResult::Passed)
        .value("KillSwitchActive", risk::RiskCheckResult::KillSwitchActive)
        .value("SymbolPositionLimit", risk::RiskCheckResult::SymbolPositionLimit)
        .value("PortfolioPositionLimit", risk::RiskCheckResult::PortfolioPositionLimit)
        .value("MaxOrderSize", risk::RiskCheckResult::MaxOrderSize)
        .value("MaxNotional", risk::RiskCheckResult::MaxNotional)
        .value("MaxDrawdownBreached", risk::RiskCheckResult::MaxDrawdownBreached)
        .value("RateLimitExceeded", risk::RiskCheckResult::RateLimitExceeded)
        .export_values();

    // ========================================================================
    // Core: Order, Trade, OrderBook
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

    py::class_<Trade>(m, "Trade")
        .def_readonly("symbol_id", &Trade::symbolId)
        .def_readonly("price", &Trade::price)
        .def_readonly("quantity", &Trade::quantity)
        .def_readonly("maker_order_id", &Trade::makerOrderId)
        .def_readonly("taker_order_id", &Trade::takerOrderId);

    py::class_<OrderBook>(m, "OrderBook")
        .def(py::init<>())
        .def("add_order", py::overload_cast<const Order&>(&OrderBook::addOrder))
        .def("cancel_order", &OrderBook::cancelOrder)
        .def("modify_order", &OrderBook::modifyOrder)
        .def("reset", &OrderBook::reset)
        .def_property_readonly("best_bid", &OrderBook::getBestBid)
        .def_property_readonly("best_ask", &OrderBook::getBestAsk)
        .def_property_readonly("has_bids", &OrderBook::hasBids)
        .def_property_readonly("has_asks", &OrderBook::hasAsks)
        .def("mid_price", [](const OrderBook& b) {
            if (!b.hasBids() || !b.hasAsks()) return -1.0;
            return (static_cast<double>(b.getBestBid()) + b.getBestAsk()) / 2.0;
        })
        .def("spread", [](const OrderBook& b) {
            if (!b.hasBids() || !b.hasAsks()) return -1.0;
            return static_cast<double>(b.getBestAsk() - b.getBestBid());
        });

    py::class_<StandardMatchingStrategy>(m, "MatchingStrategy")
        .def(py::init<>())
        .def("match", [](StandardMatchingStrategy& s, OrderBook& book, Order& order) {
            std::vector<Trade> trades;
            s.matchImpl(book, order, trades);
            return trades;
        });

    py::class_<TickNormalizer>(m, "TickNormalizer")
        .def(py::init<double>(), py::arg("tick_size") = 0.01)
        .def("to_ticks", &TickNormalizer::toTicks)
        .def("to_price", &TickNormalizer::toPrice)
        .def_property_readonly("tick_size", &TickNormalizer::tickSize);

    // ========================================================================
    // Signals: KalmanHedgeRatio
    // ========================================================================

    py::class_<signals::KalmanState>(m, "KalmanState")
        .def_readonly("alpha", &signals::KalmanState::alpha)
        .def_readonly("beta", &signals::KalmanState::beta)
        .def_readonly("spread", &signals::KalmanState::spread)
        .def_readonly("spread_variance", &signals::KalmanState::spreadVariance)
        .def("z_score", &signals::KalmanState::zScore)
        .def("beta_std_err", &signals::KalmanState::betaStdErr);

    py::class_<signals::KalmanHedgeRatio>(m, "KalmanHedgeRatio")
        .def(py::init<double, double>(),
             py::arg("delta") = 0.9999, py::arg("Ve") = 1e-3)
        .def("update", &signals::KalmanHedgeRatio::update,
             py::arg("log_price_a"), py::arg("log_price_b"))
        .def_property_readonly("beta", &signals::KalmanHedgeRatio::beta)
        .def_property_readonly("alpha", &signals::KalmanHedgeRatio::alpha)
        .def("reset", &signals::KalmanHedgeRatio::reset);

    // ========================================================================
    // Signals: SpreadModel, OFI, VPIN, KyleLambda
    // ========================================================================

    py::class_<signals::SpreadModel>(m, "SpreadModel")
        .def(py::init<size_t, double>(),
             py::arg("lookback") = 100, py::arg("beta") = 1.0)
        .def("update", &signals::SpreadModel::update)
        .def("set_kalman_filter", &signals::SpreadModel::setKalmanFilter,
             py::keep_alive<1, 2>())  // Keep KalmanHedgeRatio alive
        .def_property_readonly("z_score", &signals::SpreadModel::zScore)
        .def_property_readonly("spread", &signals::SpreadModel::spread)
        .def_property_readonly("beta", &signals::SpreadModel::beta)
        .def_property_readonly("mean", &signals::SpreadModel::mean)
        .def_property_readonly("std", &signals::SpreadModel::stdDev)
        .def_property_readonly("is_kalman_active", &signals::SpreadModel::isKalmanActive)
        .def("reset", &signals::SpreadModel::reset);

    py::class_<signals::OFI>(m, "OFI")
        .def(py::init<size_t>(), py::arg("window") = 100)
        .def("update", &signals::OFI::update)
        .def_property_readonly("value", &signals::OFI::value)
        .def_property_readonly("normalized", &signals::OFI::normalized)
        .def("reset", &signals::OFI::reset);

    py::class_<signals::VPIN>(m, "VPIN")
        .def(py::init<Quantity, size_t>(),
             py::arg("bucket_volume") = 1000, py::arg("num_buckets") = 50)
        .def("on_trade", &signals::VPIN::onTrade)
        .def_property_readonly("value", &signals::VPIN::value)
        .def_property_readonly("is_valid", &signals::VPIN::isValid)
        .def("reset", &signals::VPIN::reset);

    py::class_<signals::KyleLambdaResult>(m, "KyleLambdaResult")
        .def_readonly("lambda_", &signals::KyleLambdaResult::lambda)
        .def_readonly("r_squared", &signals::KyleLambdaResult::rSquared)
        .def_readonly("t_stat", &signals::KyleLambdaResult::tStat)
        .def("is_significant", &signals::KyleLambdaResult::isSignificant);

    py::class_<signals::KyleLambda>(m, "KyleLambda")
        .def(py::init<size_t>(), py::arg("window") = 200)
        .def("update", &signals::KyleLambda::update)
        .def("estimate", &signals::KyleLambda::estimate)
        .def("reset", &signals::KyleLambda::reset);

    // ========================================================================
    // Strategy: StatArbMM
    // ========================================================================

    py::class_<strategy::Quote>(m, "Quote")
        .def_readonly("bid_price", &strategy::Quote::bidPrice)
        .def_readonly("ask_price", &strategy::Quote::askPrice)
        .def_readonly("bid_size", &strategy::Quote::bidSize)
        .def_readonly("ask_size", &strategy::Quote::askSize)
        .def_readonly("valid", &strategy::Quote::valid)
        .def_readonly("risk_check", &strategy::Quote::riskCheck);

    py::class_<strategy::StatArbMM>(m, "StatArbMM")
        .def(py::init<>())
        .def_readwrite("gamma", &strategy::StatArbMM::gamma)
        .def_readwrite("k", &strategy::StatArbMM::k)
        .def_readwrite("min_spread", &strategy::StatArbMM::minSpread)
        .def_readwrite("default_size", &strategy::StatArbMM::defaultSize)
        .def_readwrite("z_entry_threshold", &strategy::StatArbMM::zEntryThreshold)
        .def_readwrite("z_exit_threshold", &strategy::StatArbMM::zExitThreshold)
        .def_readwrite("ofi_threshold", &strategy::StatArbMM::ofiThreshold)
        .def_readwrite("max_inventory", &strategy::StatArbMM::maxInventory)
        .def_readwrite("alpha_vpin", &strategy::StatArbMM::alphaVpin)
        .def_readwrite("alpha_lambda", &strategy::StatArbMM::alphaLambda)
        .def("set_session_times", &strategy::StatArbMM::setSessionTimes)
        .def("update_time", &strategy::StatArbMM::updateTime)
        .def_property_readonly("tau", &strategy::StatArbMM::tau)
        .def_property("inventory",
                      &strategy::StatArbMM::inventory,
                      &strategy::StatArbMM::setInventory)
        .def("reservation_price", &strategy::StatArbMM::reservationPrice)
        .def("optimal_spread", &strategy::StatArbMM::optimalSpread)
        .def("compute_quotes", &strategy::StatArbMM::computeQuotes,
             py::arg("fair_price"), py::arg("sigma"),
             py::arg("z_score") = 0, py::arg("ofi_value") = 0,
             py::arg("vpin_value") = 0, py::arg("kyle_lambda_norm") = 0)
        .def("on_fill", &strategy::StatArbMM::onFill)
        .def("calibrate_intensity", &strategy::StatArbMM::calibrateIntensity)
        .def("reset", &strategy::StatArbMM::reset);

    // ========================================================================
    // Risk: RiskManager
    // ========================================================================

    py::class_<risk::RiskConfig>(m, "RiskConfig")
        .def(py::init<>())
        .def_readwrite("max_position_per_symbol", &risk::RiskConfig::maxPositionPerSymbol)
        .def_readwrite("max_portfolio_position", &risk::RiskConfig::maxPortfolioPosition)
        .def_readwrite("max_loss_per_symbol", &risk::RiskConfig::maxLossPerSymbol)
        .def_readwrite("max_portfolio_loss", &risk::RiskConfig::maxPortfolioLoss)
        .def_readwrite("max_drawdown", &risk::RiskConfig::maxDrawdown)
        .def_readwrite("max_order_size", &risk::RiskConfig::maxOrderSize)
        .def_readwrite("max_notional", &risk::RiskConfig::maxNotional)
        .def_readwrite("max_orders_per_second", &risk::RiskConfig::maxOrdersPerSecond);

    py::class_<risk::RiskSnapshot>(m, "RiskSnapshot")
        .def_readonly("total_pnl", &risk::RiskSnapshot::totalPnL)
        .def_readonly("total_realized", &risk::RiskSnapshot::totalRealizedPnL)
        .def_readonly("total_unrealized", &risk::RiskSnapshot::totalUnrealizedPnL)
        .def_readonly("peak_pnl", &risk::RiskSnapshot::peakPnL)
        .def_readonly("current_drawdown", &risk::RiskSnapshot::currentDrawdown)
        .def_readonly("max_drawdown_seen", &risk::RiskSnapshot::maxDrawdownSeen)
        .def_readonly("total_abs_position", &risk::RiskSnapshot::totalAbsPosition)
        .def_readonly("kill_switch_active", &risk::RiskSnapshot::killSwitchActive);

    py::class_<risk::RiskManager>(m, "RiskManager")
        .def(py::init<>())
        .def("config", py::overload_cast<>(&risk::RiskManager::config),
             py::return_value_policy::reference)
        .def("pre_trade_check", &risk::RiskManager::preTradeCheck,
             py::arg("symbol_id"), py::arg("side"), py::arg("qty"),
             py::arg("price_ticks"), py::arg("current_time_ns") = 0)
        .def("on_fill", &risk::RiskManager::onFill)
        .def("update_mark", &risk::RiskManager::updateMark)
        .def("activate_kill_switch", &risk::RiskManager::activateKillSwitch)
        .def("deactivate_kill_switch", &risk::RiskManager::deactivateKillSwitch)
        .def("snapshot", &risk::RiskManager::snapshot)
        .def("reset", &risk::RiskManager::reset)
        .def_static("rejection_string", &risk::RiskManager::rejectionString);

    // ========================================================================
    // Analytics: Cointegration
    // ========================================================================

    py::class_<analytics::CointegrationResult>(m, "CointegrationResult")
        .def_readonly("beta", &analytics::CointegrationResult::beta)
        .def_readonly("adf_stat", &analytics::CointegrationResult::adf_stat)
        .def_readonly("p_value", &analytics::CointegrationResult::p_value)
        .def_readonly("is_cointegrated", &analytics::CointegrationResult::is_cointegrated)
        .def_readonly("half_life", &analytics::CointegrationResult::half_life)
        .def_readonly("mean_reversion_speed", &analytics::CointegrationResult::mean_reversion_speed)
        .def_readonly("ou_mu", &analytics::CointegrationResult::ou_mu)
        .def_readonly("ou_sigma", &analytics::CointegrationResult::ou_sigma)
        .def_readonly("log_likelihood", &analytics::CointegrationResult::log_likelihood)
        .def_readonly("adf_lags", &analytics::CointegrationResult::adf_lags);

    py::class_<analytics::JohansenResult>(m, "JohansenResult")
        .def_readonly("rank", &analytics::JohansenResult::rank)
        .def_readonly("trace_stat_r0", &analytics::JohansenResult::traceStatR0)
        .def_readonly("max_eig_stat_r0", &analytics::JohansenResult::maxEigStatR0)
        .def_readonly("eigenvalue1", &analytics::JohansenResult::eigenvalue1)
        .def_readonly("eigenvalue2", &analytics::JohansenResult::eigenvalue2)
        .def_readonly("beta1", &analytics::JohansenResult::beta1)
        .def_readonly("beta2", &analytics::JohansenResult::beta2)
        .def_readonly("trace_rejects_r0", &analytics::JohansenResult::traceRejectsR0)
        .def_readonly("max_eig_rejects_r0", &analytics::JohansenResult::maxEigRejectsR0);

    py::class_<analytics::OUMLEResult>(m, "OUMLEResult")
        .def_readonly("theta", &analytics::OUMLEResult::theta)
        .def_readonly("mu", &analytics::OUMLEResult::mu)
        .def_readonly("sigma", &analytics::OUMLEResult::sigma)
        .def_readonly("half_life", &analytics::OUMLEResult::halfLife)
        .def_readonly("log_likelihood", &analytics::OUMLEResult::logLikelihood)
        .def_readonly("aic", &analytics::OUMLEResult::aic)
        .def_readonly("bic", &analytics::OUMLEResult::bic);

    m.def("engle_granger", &analytics::CointegrationAnalyzer::engleGranger,
          "Run Engle-Granger cointegration test");
    m.def("johansen_test", &analytics::CointegrationAnalyzer::johansenTest,
          "Run Johansen cointegration rank test",
          py::arg("prices_a"), py::arg("prices_b"), py::arg("lags") = 1);
    m.def("fit_ou_mle", &analytics::CointegrationAnalyzer::fitOU_MLE,
          "Fit OU process via MLE",
          py::arg("series"), py::arg("dt") = 1.0);

    // ========================================================================
    // Execution: TransactionCostModel
    // ========================================================================

    py::class_<execution::CostBreakdown>(m, "CostBreakdown")
        .def_readonly("spread_cost", &execution::CostBreakdown::spreadCost)
        .def_readonly("fixed_cost", &execution::CostBreakdown::fixedCost)
        .def_readonly("temporary_impact", &execution::CostBreakdown::temporaryImpact)
        .def_readonly("exchange_fee", &execution::CostBreakdown::exchangeFee)
        .def_readonly("total_cost", &execution::CostBreakdown::totalCost);

    py::class_<execution::TransactionCostModel>(m, "TransactionCostModel")
        .def(py::init<>())
        .def("estimate_cost", &execution::TransactionCostModel::estimateCost)
        .def("set_daily_volume", &execution::TransactionCostModel::setDailyVolume)
        .def("set_epsilon", &execution::TransactionCostModel::setEpsilon)
        .def("set_eta", &execution::TransactionCostModel::setEta);

    // ========================================================================
    // Constants
    // ========================================================================

    m.attr("MAX_PRICE") = OrderBook::MAX_PRICE;
    m.attr("MAX_ORDER_ID") = OrderBook::MAX_ORDER_ID;
    m.attr("PRICE_INVALID") = PRICE_INVALID;
}
