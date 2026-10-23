# SPDX-FileCopyrightText: 2026 PullsarDev
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20)
project(InterestConsumer LANGUAGES CXX)

add_subdirectory(../.. interest-engine)
if(INTEREST_BUILD_TESTS OR INTEREST_BUILD_BENCHMARKS OR INTEREST_BUILD_EXAMPLES)
    message(FATAL_ERROR "Embedded library utilities must default to OFF")
endif()

add_executable(interest_consumer ../basic.cpp)
target_link_libraries(interest_consumer PRIVATE InterestEngine::Core)

enable_testing()
add_test(NAME interest_consumer COMMAND interest_consumer)
