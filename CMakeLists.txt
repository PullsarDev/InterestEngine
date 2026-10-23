# SPDX-FileCopyrightText: 2026 PullsarDev
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20)

project(InterestEngine VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_EXTENSIONS OFF)

set(interest_top_level OFF)
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(interest_top_level ON)
endif()

option(INTEREST_BUILD_TESTS "Build the Interest Engine tests" ${interest_top_level})
option(INTEREST_BUILD_BENCHMARKS "Build the Step benchmark harness" ${interest_top_level})
option(INTEREST_BUILD_EXAMPLES "Build the basic API example" ${interest_top_level})
option(INTEREST_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(INTEREST_CONSTRAINED_BUILD "Compile without exceptions or RTTI, with shadow warnings" OFF)

if(INTEREST_CONSTRAINED_BUILD AND MSVC)
    # CMake's default MSVC flags enable exceptions; remove them so /EHs-c- is not overridden.
    string(REPLACE "/EHsc" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
endif()

# Project-wide compile settings shared by the kernel and its tests.
add_library(interest_build_settings INTERFACE)
target_compile_features(interest_build_settings INTERFACE cxx_std_20)

if(MSVC)
    target_compile_options(interest_build_settings INTERFACE
        /W4 /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor)
    if(INTEREST_WARNINGS_AS_ERRORS)
        target_compile_options(interest_build_settings INTERFACE /WX)
    endif()
    if(INTEREST_CONSTRAINED_BUILD)
        # C4456-C4459: declaration hides previous local/parameter/member/global.
        target_compile_options(interest_build_settings INTERFACE
            /EHs-c- /GR- /we4456 /we4457 /we4458 /we4459)
        target_compile_definitions(interest_build_settings INTERFACE _HAS_EXCEPTIONS=0)
    endif()
else()
    target_compile_options(interest_build_settings INTERFACE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wdouble-promotion)
    if(INTEREST_WARNINGS_AS_ERRORS)
        target_compile_options(interest_build_settings INTERFACE -Werror)
    endif()
    if(INTEREST_CONSTRAINED_BUILD)
        target_compile_options(interest_build_settings INTERFACE
            -fno-exceptions -fno-rtti -Wshadow)
    endif()
endif()

add_library(interest_core STATIC
    src/step.cpp
    src/version.cpp)
add_library(InterestEngine::Core ALIAS interest_core)
target_include_directories(interest_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(interest_core PRIVATE interest_build_settings)
target_compile_features(interest_core PUBLIC cxx_std_20)

if(INTEREST_BUILD_TESTS)
    enable_testing()

    add_executable(interest_smoke_test tests/smoke_test.cpp)
    target_link_libraries(interest_smoke_test PRIVATE interest_core interest_build_settings)
    if(INTEREST_CONSTRAINED_BUILD)
        # Lets the smoke test confirm the constraints were actually applied.
        target_compile_definitions(interest_smoke_test PRIVATE INTEREST_EXPECT_CONSTRAINED_BUILD=1)
    endif()

    add_test(NAME interest_smoke_test COMMAND interest_smoke_test)

    foreach(suite IN ITEMS scenario scoring allocation c07c temporal baseline rate observers novelty retention c18 property)
        add_executable(interest_${suite}_test tests/step_${suite}_test.cpp)
        target_link_libraries(interest_${suite}_test PRIVATE interest_core interest_build_settings)
        add_test(NAME interest_${suite}_test COMMAND interest_${suite}_test)
    endforeach()

endif()

if(INTEREST_BUILD_BENCHMARKS)
    # CTest checks harness invariants; use optimized manual runs for timing evidence.
    add_executable(interest_step_benchmark benchmarks/step_benchmark.cpp)
    target_include_directories(interest_step_benchmark PRIVATE tests)
    target_link_libraries(interest_step_benchmark PRIVATE interest_core interest_build_settings)
    if(INTEREST_BUILD_TESTS)
        add_test(NAME interest_benchmark_smoke COMMAND interest_step_benchmark --smoke)
    endif()
endif()

if(INTEREST_BUILD_EXAMPLES)
    add_executable(interest_basic_example examples/basic.cpp)
    target_link_libraries(interest_basic_example PRIVATE InterestEngine::Core interest_build_settings)
    if(INTEREST_BUILD_TESTS)
        add_test(NAME interest_basic_example COMMAND interest_basic_example)
    endif()
endif()
