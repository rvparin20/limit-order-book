cmake_minimum_required(VERSION 3.16)
project(limit_order_book CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

add_executable(lob_tests tests/test_order_book.cpp)
target_include_directories(lob_tests PRIVATE include)

add_executable(lob_bench bench/bench.cpp)
target_include_directories(lob_bench PRIVATE include)
target_compile_options(lob_bench PRIVATE -O3)

enable_testing()
add_test(NAME lob_tests COMMAND lob_tests)
