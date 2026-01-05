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
# Target: Unit Tests
# ============================================================================

add_executable(orderbook_tests
    tests/cpp/orderbook_tests.cpp
    ${CORE_SOURCES}
)

set_target_properties(orderbook_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin"
)

# ============================================================================
# Target: Latency Tests
# ============================================================================

add_executable(latency_tests
    tests/cpp/latency_tests.cpp
    ${CORE_SOURCES}
)

set_target_properties(latency_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin"
)

# ============================================================================
# Target: Benchmark
# ============================================================================

add_executable(orderbook_bench
    tests/cpp/orderbook_benchmark.cpp
    ${CORE_SOURCES}
)

set_target_properties(orderbook_bench PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin"
)

# ============================================================================
# Threading
# ============================================================================

find_package(Threads REQUIRED)
target_link_libraries(stat_arb_runner PRIVATE Threads::Threads)
target_link_libraries(orderbook_tests PRIVATE Threads::Threads)
target_link_libraries(latency_tests PRIVATE Threads::Threads)
target_link_libraries(orderbook_bench PRIVATE Threads::Threads)

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
message(STATUS "  - stat_arb_runner  (main demo)")
message(STATUS "  - orderbook_tests  (unit tests)")
message(STATUS "  - latency_tests    (latency verification)")
message(STATUS "  - orderbook_bench  (throughput benchmark)")
message(STATUS "")