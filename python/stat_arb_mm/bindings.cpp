#include <pybind11/pybind11.h>
#include <pybind11/stl.h> 
#include "../../src/core/matching_engine.cpp"

namespace py = pybind11;

PYBIND11_MODULE(stat_arb_mm, m) {
    m.doc() = "C++20 Backend for Statistical Arbitrage";

    py::class_<MatchingEngine>(m, "MatchingEngine")
        .def(py::init<>())
        .def("calculate_spread", &MatchingEngine::calculate_spread)
        .def("log_execution", &MatchingEngine::log_execution<double>) // Explicit instantiation for binding
        .def("print_status", &MatchingEngine::print_status);
}
