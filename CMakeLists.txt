cmake_minimum_required(VERSION 3.20)

project(stateguard VERSION 0.1.0 LANGUAGES CXX)

option(STATEGUARD_BUILD_TESTS "Build the CPU checker tests" ON)
option(STATEGUARD_BUILD_EXAMPLES "Build the instrumented CPU example" ON)

add_library(stateguard
  src/checker.cpp
  src/generator.cpp
  src/reducer.cpp
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

foreach(tool check generate reduce)
  add_executable(stateguard-${tool} tools/stateguard_${tool}.cpp)
  target_link_libraries(stateguard-${tool} PRIVATE stateguard::stateguard)
endforeach()

if(STATEGUARD_BUILD_EXAMPLES)
  add_executable(stateguard-instrumented-demo examples/instrumented_demo.cpp)
  target_link_libraries(stateguard-instrumented-demo PRIVATE stateguard::stateguard)
endif()

if(STATEGUARD_BUILD_TESTS)
  enable_testing()
  function(stateguard_add_test_suite target source)
    add_executable(${target} ${source})
    target_link_libraries(${target} PRIVATE stateguard::stateguard)
    target_compile_definitions(${target} PRIVATE
      STATEGUARD_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME ${target} COMMAND ${target})
  endfunction()
  stateguard_add_test_suite(stateguard-tests tests/test_main.cpp)
  stateguard_add_test_suite(stateguard-generate-reduce-tests tests/test_generate_reduce.cpp)

  # End-to-end CLI workflow: generate a faulted trace, check it, reduce it, and
  # check that the reduced witness still reports the same failure.
  set(cli_dir ${CMAKE_CURRENT_BINARY_DIR}/cli-tests)
  file(MAKE_DIRECTORY ${cli_dir})
  add_test(NAME cli-generate
    COMMAND stateguard-generate --seed 7 --fault premature_reclaim
            --output ${cli_dir}/generated.sgtrace)
  set_tests_properties(cli-generate PROPERTIES FIXTURES_SETUP generated_trace)
  add_test(NAME cli-check-generated
    COMMAND stateguard-check ${cli_dir}/generated.sgtrace)
  set_tests_properties(cli-check-generated PROPERTIES
    FIXTURES_REQUIRED generated_trace
    PASS_REGULAR_EXPRESSION "^violation\n.*premature_reclaim")
  add_test(NAME cli-reduce
    COMMAND stateguard-reduce --output ${cli_dir}/reduced.sgtrace
            ${cli_dir}/generated.sgtrace)
  set_tests_properties(cli-reduce PROPERTIES
    FIXTURES_REQUIRED generated_trace
    FIXTURES_SETUP reduced_trace)
  add_test(NAME cli-check-reduced
    COMMAND stateguard-check ${cli_dir}/reduced.sgtrace)
  set_tests_properties(cli-check-reduced PROPERTIES
    FIXTURES_REQUIRED reduced_trace
    PASS_REGULAR_EXPRESSION "^violation\n[0-9]+ premature_reclaim")
  add_test(NAME cli-reduce-clean-trace
    COMMAND stateguard-reduce ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/safe_deferred_reclaim.sgtrace)
  set_tests_properties(cli-reduce-clean-trace PROPERTIES WILL_FAIL TRUE)
endif()
