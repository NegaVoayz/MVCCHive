cmake_minimum_required(VERSION 3.28)

project(mvcc_hive VERSION 2.0.0 LANGUAGES CXX)

add_library(mvcc_hive INTERFACE)

target_compile_features(mvcc_hive INTERFACE cxx_std_23)
target_include_directories(
    mvcc_hive
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

option(MVCC_HIVE_BUILD_TESTS "Build MVCCHive tests" ON)
option(MVCC_HIVE_BUILD_BENCHMARKS "Build MVCCHive microbenchmarks" OFF)

if(MVCC_HIVE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(MVCC_HIVE_BUILD_BENCHMARKS)
    add_subdirectory(bench)
endif()
