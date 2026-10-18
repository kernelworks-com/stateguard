cmake_minimum_required(VERSION 3.20)

project(stateguard VERSION 0.1.0 LANGUAGES CXX)

option(STATEGUARD_BUILD_TESTS "Build the CPU checker tests" ON)
option(STATEGUARD_BUILD_EXAMPLES "Build the instrumented CPU example" ON)
option(STATEGUARD_ENABLE_CUDA "Build the optional CUDA copy harness and demo" OFF)

add_library(stateguard
  src/capture.cpp
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

# Instrumented allocator and async-copy harness used by the demo and tests.
if(STATEGUARD_BUILD_EXAMPLES OR STATEGUARD_BUILD_TESTS)
  add_library(stateguard-async-copy STATIC harness/async_copy.cpp)
  target_include_directories(stateguard-async-copy PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/harness)
  target_link_libraries(stateguard-async-copy PUBLIC stateguard::stateguard)
  target_compile_options(stateguard-async-copy PRIVATE
    $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall;-Wextra;-Wpedantic>
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
  )
endif()

# Optional CUDA variant of the harness. It needs the CUDA toolkit and runtime
# but no CUDA compiler: it calls the runtime API from ordinary C++. The core
# library never includes CUDA headers.
if(STATEGUARD_ENABLE_CUDA)
  if(NOT TARGET stateguard-async-copy)
    message(FATAL_ERROR "STATEGUARD_ENABLE_CUDA needs examples or tests enabled")
  endif()
  find_package(CUDAToolkit REQUIRED)
  add_library(stateguard-cuda-copy STATIC harness/cuda_copy.cpp)
  target_link_libraries(stateguard-cuda-copy PUBLIC stateguard-async-copy PRIVATE CUDA::cudart)
  target_compile_options(stateguard-cuda-copy PRIVATE
    $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall;-Wextra;-Wpedantic>
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
  )
  add_executable(stateguard-cuda-copy-demo examples/cuda_copy_demo.cpp)
  target_link_libraries(stateguard-cuda-copy-demo PRIVATE stateguard-cuda-copy)
endif()

if(STATEGUARD_BUILD_EXAMPLES)
  add_executable(stateguard-instrumented-demo examples/instrumented_demo.cpp)
  target_link_libraries(stateguard-instrumented-demo PRIVATE stateguard::stateguard)
  add_executable(stateguard-async-copy-demo examples/async_copy_demo.cpp)
  target_link_libraries(stateguard-async-copy-demo PRIVATE stateguard-async-copy)
endif()

if(STATEGUARD_BUILD_TESTS)
  enable_testing()
  # Links the core library, or the libraries listed after the source.
  function(stateguard_add_test_suite target source)
    add_executable(${target} ${source})
    if(ARGN)
      target_link_libraries(${target} PRIVATE ${ARGN})
    else()
      target_link_libraries(${target} PRIVATE stateguard::stateguard)
    endif()
    target_compile_definitions(${target} PRIVATE
      STATEGUARD_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME ${target} COMMAND ${target})
  endfunction()
  stateguard_add_test_suite(stateguard-tests tests/test_main.cpp)
  stateguard_add_test_suite(stateguard-generate-reduce-tests tests/test_generate_reduce.cpp)
  if(TARGET stateguard-async-copy)
    stateguard_add_test_suite(stateguard-async-copy-tests tests/test_async_copy.cpp
      stateguard-async-copy)
  endif()

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

  # Harness workflow: capture traces from the instrumented async-copy demo,
  # then check and reduce them with the offline tools.
  if(STATEGUARD_BUILD_EXAMPLES)
    add_test(NAME async-copy-demo
      COMMAND stateguard-async-copy-demo --trace-dir ${cli_dir})
    set_tests_properties(async-copy-demo PROPERTIES FIXTURES_SETUP harness_traces)
    add_test(NAME cli-check-harness-safe
      COMMAND stateguard-check ${cli_dir}/safe.sgtrace)
    set_tests_properties(cli-check-harness-safe PROPERTIES
      FIXTURES_REQUIRED harness_traces
      PASS_REGULAR_EXPRESSION "^clean\n$")
    add_test(NAME cli-check-harness-immediate
      COMMAND stateguard-check ${cli_dir}/immediate.sgtrace)
    set_tests_properties(cli-check-harness-immediate PROPERTIES
      FIXTURES_REQUIRED harness_traces
      PASS_REGULAR_EXPRESSION "^violation\n[0-9]+ premature_reclaim")
    add_test(NAME cli-check-harness-failed-retry
      COMMAND stateguard-check ${cli_dir}/failed-retry.sgtrace)
    set_tests_properties(cli-check-harness-failed-retry PROPERTIES
      FIXTURES_REQUIRED harness_traces
      PASS_REGULAR_EXPRESSION "^clean\n$")
    add_test(NAME cli-check-harness-failed-publish
      COMMAND stateguard-check ${cli_dir}/failed-publish.sgtrace)
    set_tests_properties(cli-check-harness-failed-publish PROPERTIES
      FIXTURES_REQUIRED harness_traces
      PASS_REGULAR_EXPRESSION "^violation\n[0-9]+ failed_write_publication")
    add_test(NAME cli-reduce-harness-immediate
      COMMAND stateguard-reduce ${cli_dir}/immediate.sgtrace)
    set_tests_properties(cli-reduce-harness-immediate PROPERTIES
      FIXTURES_REQUIRED harness_traces
      PASS_REGULAR_EXPRESSION "premature_reclaim")
  endif()

  # GPU workflow: these tests need a CUDA device and fail without one.
  if(TARGET stateguard-cuda-copy-demo)
    add_test(NAME cuda-copy-demo
      COMMAND stateguard-cuda-copy-demo --repeat 5 --trace-dir ${cli_dir})
    set_tests_properties(cuda-copy-demo PROPERTIES
      FIXTURES_SETUP cuda_traces TIMEOUT 120 LABELS gpu)
    foreach(name safe safe-delayed)
      add_test(NAME cli-check-cuda-${name}
        COMMAND stateguard-check ${cli_dir}/cuda-${name}.sgtrace)
      set_tests_properties(cli-check-cuda-${name} PROPERTIES
        FIXTURES_REQUIRED cuda_traces LABELS gpu
        PASS_REGULAR_EXPRESSION "^clean\n$")
    endforeach()
    foreach(name immediate immediate-delayed)
      add_test(NAME cli-check-cuda-${name}
        COMMAND stateguard-check ${cli_dir}/cuda-${name}.sgtrace)
      set_tests_properties(cli-check-cuda-${name} PROPERTIES
        FIXTURES_REQUIRED cuda_traces LABELS gpu
        PASS_REGULAR_EXPRESSION "^violation\n[0-9]+ premature_reclaim")
    endforeach()
  endif()
endif()
