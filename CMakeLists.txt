cmake_minimum_required(VERSION 3.14)
project(Statistical-Arb-MM LANGUAGES CXX)

# ============================================================================
# C++ Standard
# ============================================================================
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ============================================================================
# Compiler Options
# ============================================================================

if(MSVC)
    add_compile_options(/W4 /permissive-)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# Performance flags for Release
if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    if(MSVC)
        add_compile_options(/O2 /Ob3 /GL /arch:AVX2 /fp:fast)
        add_link_options(/LTCG)
    else()
        add_compile_options(-O3 -march=native -mtune=native -flto -ffast-math)
        add_compile_options(-funroll-loops -fomit-frame-pointer)
        add_link_options(-flto)
    endif()
endif()

# ============================================================================
# Dependencies (FetchContent)
# ============================================================================

include(FetchContent)

# GoogleTest
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# Disable LTO on GoogleTest (LTO + visibility attributes causes warnings on MinGW)
if(NOT MSVC)
    set_target_properties(gtest PROPERTIES INTERPROCEDURAL_OPTIMIZATION FALSE)
    set_target_properties(gtest_main PROPERTIES INTERPROCEDURAL_OPTIMIZATION FALSE)
endif()

# Google Benchmark
FetchContent_Declare(
    benchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG        v1.8.3
)
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(benchmark)

# Eigen (header-only, analytics layer only)
FetchContent_Declare(
    eigen
    GIT_REPOSITORY https://gitlab.com/libeigen/eigen.git
    GIT_TAG        3.4.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(eigen)

# ============================================================================
# Source Files
# ============================================================================

set(CORE_SOURCES
    src/core/OrderBook.cpp
)

# ============================================================================
# Target: Main Executable (Equity Demo)
# ============================================================================

add_executable(stat_arb_runner
    src/main.cpp
    ${CORE_SOURCES}
)

set_target_properties(stat_arb_runner PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin"
)

# ============================================================================
# Target: Unit Tests (GoogleTest)
# ============================================================================

add_executable(orderbook_tests
    tests/cpp/orderbook_tests.cpp
    ${CORE_SOURCES}
)
# Note: orderbook_tests uses custom test macros, not GTest

add_executable(strategy_tests
    tests/cpp/strategy_tests.cpp
    ${CORE_SOURCES}
)
target_link_libraries(strategy_tests PRIVATE GTest::gtest GTest::gtest_main)

add_executable(kalman_tests
    tests/cpp/kalman_tests.cpp
    ${CORE_SOURCES}
)
target_link_libraries(kalman_tests PRIVATE GTest::gtest GTest::gtest_main)

add_executable(cointegration_gtest
    tests/cpp/cointegration_gtest.cpp
    ${CORE_SOURCES}
)
target_link_libraries(cointegration_gtest PRIVATE GTest::gtest GTest::gtest_main)

add_executable(risk_tests
    tests/cpp/risk_tests.cpp
    ${CORE_SOURCES}
)
target_link_libraries(risk_tests PRIVATE GTest::gtest GTest::gtest_main)

add_executable(integration_tests
    tests/cpp/integration_tests.cpp
    ${CORE_SOURCES}
)
target_link_libraries(integration_tests PRIVATE GTest::gtest GTest::gtest_main)

set_target_properties(
    orderbook_tests strategy_tests kalman_tests
    cointegration_gtest risk_tests integration_tests
    PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin"
)

# ============================================================================
# Target: Latency Tests (Legacy)
# ============================================================================

add_executable(latency_tests
    tests/cpp/latency_tests.cpp
    ${CORE_SOURCES}
)

set_target_properties(latency_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin"
)

# ============================================================================
# Target: Benchmarks (Google Benchmark)
# ============================================================================

add_executable(orderbook_bench
    tests/cpp/orderbook_benchmark.cpp
    ${CORE_SOURCES}
)

add_executable(strategy_benchmark
    tests/cpp/strategy_benchmark.cpp
    ${CORE_SOURCES}
)
target_link_libraries(strategy_benchmark PRIVATE benchmark::benchmark benchmark::benchmark_main)

set_target_properties(orderbook_bench strategy_benchmark PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin"
)

# ============================================================================
# Target: Replay Tests
# ============================================================================

add_executable(replay_tests
    tests/cpp/replay_tests.cpp
    ${CORE_SOURCES}
)

set_target_properties(replay_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin"
)

# ============================================================================
# Threading
# ============================================================================

find_package(Threads REQUIRED)
target_link_libraries(stat_arb_runner PRIVATE Threads::Threads)
target_link_libraries(orderbook_tests PRIVATE Threads::Threads)
target_link_libraries(strategy_tests PRIVATE Threads::Threads)
target_link_libraries(kalman_tests PRIVATE Threads::Threads)
target_link_libraries(cointegration_gtest PRIVATE Threads::Threads)
target_link_libraries(risk_tests PRIVATE Threads::Threads)
target_link_libraries(integration_tests PRIVATE Threads::Threads)
target_link_libraries(latency_tests PRIVATE Threads::Threads)
target_link_libraries(orderbook_bench PRIVATE Threads::Threads)
target_link_libraries(strategy_benchmark PRIVATE Threads::Threads)
target_link_libraries(replay_tests PRIVATE Threads::Threads)

# ============================================================================
# Eigen (analytics layer only)
# ============================================================================

target_link_libraries(cointegration_gtest PRIVATE Eigen3::Eigen)

# ============================================================================
# CTest Integration
# ============================================================================

enable_testing()
add_test(NAME OrderBookTests COMMAND orderbook_tests)
add_test(NAME StrategyTests COMMAND strategy_tests)
add_test(NAME KalmanTests COMMAND kalman_tests)
add_test(NAME CointegrationTests COMMAND cointegration_gtest)
add_test(NAME RiskTests COMMAND risk_tests)
add_test(NAME IntegrationTests COMMAND integration_tests)
add_test(NAME ReplayTests COMMAND replay_tests)

# ============================================================================
# Python Bindings (Optional)
# ============================================================================

find_package(Python3 COMPONENTS Interpreter Development QUIET)
find_package(pybind11 QUIET)

if(pybind11_FOUND AND Python3_FOUND)
    pybind11_add_module(stat_arb_mm python/stat_arb_mm/bindings.cpp ${CORE_SOURCES})

    set_target_properties(stat_arb_mm PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/python/stat_arb_mm"
    )

    if(MSVC)
        set_target_properties(stat_arb_mm PROPERTIES SUFFIX ".pyd")
    endif()

    message(STATUS "Python bindings: ENABLED")
else()
    message(STATUS "Python bindings: DISABLED")
endif()

# ============================================================================
# Build Info
# ============================================================================

message(STATUS "")
message(STATUS "=== Build Configuration ===")
message(STATUS "Build Type:    ${CMAKE_BUILD_TYPE}")
message(STATUS "C++ Standard:  ${CMAKE_CXX_STANDARD}")
message(STATUS "Compiler:      ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "")
message(STATUS "Targets:")
message(STATUS "  - stat_arb_runner      (main demo)")
message(STATUS "  - orderbook_tests      (OrderBook unit tests)")
message(STATUS "  - strategy_tests       (StatArbMM tests)")
message(STATUS "  - kalman_tests         (Kalman filter tests)")
message(STATUS "  - cointegration_gtest  (Cointegration stat tests)")
message(STATUS "  - risk_tests           (Risk management tests)")
message(STATUS "  - integration_tests    (Full pipeline tests)")
message(STATUS "  - latency_tests        (Latency verification)")
message(STATUS "  - orderbook_bench      (Throughput benchmark)")
message(STATUS "  - strategy_benchmark   (Strategy latency benchmark)")
message(STATUS "  - replay_tests         (LOBSTER replay validation)")
message(STATUS "")
