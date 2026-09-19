cmake_minimum_required(VERSION 3.20)

project(stateguard VERSION 0.1.0 LANGUAGES CXX)

option(STATEGUARD_BUILD_TESTS "Build the CPU checker tests" ON)
option(STATEGUARD_BUILD_EXAMPLES "Build the instrumented CPU example" ON)

add_library(stateguard
  src/checker.cpp
  src/trace.cpp
)
add_library(stateguard::stateguard ALIAS stateguard)
target_compile_features(stateguard PUBLIC cxx_std_20)
target_include_directories(stateguard
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_options(stateguard PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall;-Wextra;-Wpedantic>
  $<$<CXX_COMPILER_ID:MSVC>:/W4>
)

add_executable(stateguard-check tools/stateguard_check.cpp)
target_link_libraries(stateguard-check PRIVATE stateguard::stateguard)

if(STATEGUARD_BUILD_EXAMPLES)
  add_executable(stateguard-instrumented-demo examples/instrumented_demo.cpp)
  target_link_libraries(stateguard-instrumented-demo PRIVATE stateguard::stateguard)
endif()

if(STATEGUARD_BUILD_TESTS)
  enable_testing()
  add_executable(stateguard-tests tests/test_main.cpp)
  target_link_libraries(stateguard-tests PRIVATE stateguard::stateguard)
  target_compile_definitions(stateguard-tests PRIVATE
    STATEGUARD_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
  add_test(NAME stateguard-tests COMMAND stateguard-tests)
endif()

