cmake_minimum_required(VERSION 3.14)
project(Statistical-Arb-MM)

# Enforce C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Compiler Warnings
if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# --- Dependencies ---
find_package(Python3 COMPONENTS Interpreter Development REQUIRED)
find_package(pybind11 REQUIRED)

# --- Sources ---
# Define core sources to reuse between targets
set(CORE_SOURCES src/core/matching_engine.cpp)

# --- Target 1: Python Library (The 'import stat_arb_mm') ---
# Add CORE_SOURCES here so the module has the implementation
pybind11_add_module(stat_arb_mm python/stat_arb_mm/bindings.cpp ${CORE_SOURCES})

set_target_properties(stat_arb_mm PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/python/stat_arb_mm")

if(MSVC)
    set_target_properties(stat_arb_mm PROPERTIES SUFFIX ".pyd")
endif()

# --- Target 2: Standalone Executable (The 'Runner') ---
# Add CORE_SOURCES here so the runner links correctly
add_executable(stat_arb_runner src/main.cpp ${CORE_SOURCES})

if(MSVC)
    set_target_properties(stat_arb_runner PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin")
else()
    set_target_properties(stat_arb_runner PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin")
endif()