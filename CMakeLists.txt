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

execute_process(
    COMMAND "${Python3_EXECUTABLE}" -m pybind11 --cmakedir
    OUTPUT_VARIABLE PYBIND11_CMAKE_DIR
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
list(APPEND CMAKE_PREFIX_PATH "${PYBIND11_CMAKE_DIR}")
find_package(pybind11 REQUIRED)

# --- Target 1: Python Library (The 'import stat_arb_mm') ---
pybind11_add_module(stat_arb_mm python/stat_arb_mm/bindings.cpp)
set_target_properties(stat_arb_mm PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/python/stat_arb_mm")

if(MSVC)
    set_target_properties(stat_arb_mm PROPERTIES SUFFIX ".pyd")
endif()

# --- Target 2: Standalone Executable (The 'Runner') ---
add_executable(stat_arb_runner src/main.cpp)

# This allows the runner to run correctly on Windows
if(MSVC)
    set_target_properties(stat_arb_runner PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin"
    )
else()
    set_target_properties(stat_arb_runner PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin"
    )
endif()
