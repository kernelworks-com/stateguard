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
  src/timeline.cpp
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

foreach(tool check generate reduce timeline)
  add_executable(stateguard-${tool} tools/stateguard_${tool}.cpp)
  target_link_libraries(stateguard-${tool} PRIVATE stateguard::stateguard)
  target_compile_definitions(stateguard-${tool} PRIVATE
    STATEGUARD_VERSION="${PROJECT_VERSION}")
endforeach()

# Instrumented allocator and async-copy harness used by the demo and tests.
if(STATEGUARD_BUILD_EXAMPLES OR STATEGUARD_BUILD_TESTS)
  add_library(stateguard-async-copy STATIC harness/async_copy.cpp harness/cancellation.cpp)
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
  add_executable(stateguard-cancellation-demo examples/cancellation_timeline_demo.cpp)
  target_link_libraries(stateguard-cancellation-demo PRIVATE stateguard-async-copy)
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
  find_package(Threads REQUIRED)
  stateguard_add_test_suite(stateguard-capture-tests tests/test_capture.cpp
    stateguard::stateguard Threads::Threads)
  if(TARGET stateguard-async-copy)
    stateguard_add_test_suite(stateguard-async-copy-tests tests/test_async_copy.cpp
      stateguard-async-copy)
    stateguard_add_test_suite(stateguard-timeline-tests tests/test_timeline.cpp
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
    PASS_REGULAR_EXPRESSION "^violation\r?\n.*premature_reclaim")
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
    PASS_REGULAR_EXPRESSION "^violation\r?\n[0-9]+ premature_reclaim")
  add_test(NAME cli-reduce-clean-trace
    COMMAND stateguard-reduce ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/safe_deferred_reclaim.sgtrace)
  set_tests_properties(cli-reduce-clean-trace PROPERTIES WILL_FAIL TRUE)

  # Every example trace in the format specification must parse.
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    add_test(NAME docs-trace-format-examples
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_trace_format_doc.py
              $<TARGET_FILE:stateguard-check> ${CMAKE_CURRENT_SOURCE_DIR}/docs/TRACE-FORMAT.md)
  endif()

  # Machine-readable output, standard input, --help and --version.
  set(fixtures ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures)
  if(Python3_Interpreter_FOUND)
    add_test(NAME cli-check-json-violation
      COMMAND sh -c "\"$1\" --json \"$2\"; test $? -eq 1 || exit 9" sh
              $<TARGET_FILE:stateguard-check> ${fixtures}/premature_reclaim.sgtrace)
    add_test(NAME cli-check-json-parse
      COMMAND sh -c "\"$1\" --json \"$2\" | \"$3\" -c \"import json,sys; d=json.load(sys.stdin); assert d['status']=='violation' and d['exit_code']==1, d; assert d['diagnostics'][0]['category']=='premature_reclaim', d\"" sh
              $<TARGET_FILE:stateguard-check> ${fixtures}/premature_reclaim.sgtrace
              ${Python3_EXECUTABLE})
    add_test(NAME cli-check-json-clean
      COMMAND sh -c "\"$1\" --json \"$2\" | \"$3\" -c \"import json,sys; d=json.load(sys.stdin); assert d['status']=='clean' and d['exit_code']==0 and d['diagnostics']==[], d\"" sh
              $<TARGET_FILE:stateguard-check> ${fixtures}/safe_deferred_reclaim.sgtrace
              ${Python3_EXECUTABLE})
    add_test(NAME cli-check-json-unconfirmed
      COMMAND sh -c "\"$1\" --json \"$2\" | \"$3\" -c \"import json,sys; d=json.load(sys.stdin); assert d['status']=='inconclusive', d; assert d['diagnostics'][0]['category']=='incomplete_evidence' and not d['diagnostics'][1]['confirmed'], d\"" sh
              $<TARGET_FILE:stateguard-check> ${fixtures}/coverage_gap_unconfirmed.sgtrace
              ${Python3_EXECUTABLE})
    add_test(NAME cli-check-json-follow-on
      COMMAND sh -c "\"$1\" --json \"$2\" | \"$3\" -c \"import json,sys; d=json.load(sys.stdin); f=[x['follow_on'] for x in d['diagnostics']]; assert f==[False,True,False,True,False], d\"" sh
              $<TARGET_FILE:stateguard-check> ${fixtures}/retry_acknowledges_other_attempt.sgtrace
              ${Python3_EXECUTABLE})
    add_test(NAME cli-check-json-primary-only
      COMMAND sh -c "\"$1\" --json --primary-only \"$2\" | \"$3\" -c \"import json,sys; d=json.load(sys.stdin); assert d['status']=='violation' and d['exit_code']==1, d; assert len(d['diagnostics'])==3 and not any(x['follow_on'] for x in d['diagnostics']), d\"" sh
              $<TARGET_FILE:stateguard-check> ${fixtures}/retry_acknowledges_other_attempt.sgtrace
              ${Python3_EXECUTABLE})
    add_test(NAME cli-timeline-json
      COMMAND sh -c "\"$1\" --json - < \"$2\" | \"$3\" -c \"import json,sys; d=json.load(sys.stdin); assert d['status']=='complete', d; assert d['first_ns'] is not None and d['requests'] and d['allocations'], d; assert d['peak_cancelled_only_bytes'] >= 0, d\"" sh
              $<TARGET_FILE:stateguard-timeline> ${fixtures}/cancellation_timeline.sgtrace
              ${Python3_EXECUTABLE})
    add_test(NAME cli-timeline-json-untimed
      COMMAND sh -c "\"$1\" --json \"$2\" | \"$3\" -c \"import json,sys; d=json.load(sys.stdin); assert d['first_ns'] is None and d['last_ns'] is None, d\"" sh
              $<TARGET_FILE:stateguard-timeline> ${fixtures}/premature_reclaim.sgtrace
              ${Python3_EXECUTABLE})
    # LLM-assisted explanation script, against a local stub server.
    add_test(NAME explain-trace-script
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_explain_trace.py)
    set_tests_properties(explain-trace-script PROPERTIES
      ENVIRONMENT "STATEGUARD_BUILD_DIR=$<TARGET_FILE_DIR:stateguard-check>")
  endif()
  add_test(NAME cli-check-stdin
    COMMAND sh -c "cat \"$2\" | \"$1\" -" sh
            $<TARGET_FILE:stateguard-check> ${fixtures}/premature_reclaim.sgtrace)
  set_tests_properties(cli-check-stdin PROPERTIES
    PASS_REGULAR_EXPRESSION "^violation\r?\n[0-9]+ premature_reclaim")
  add_test(NAME cli-reduce-stdin
    COMMAND sh -c "cat \"$2\" | \"$1\" -" sh
            $<TARGET_FILE:stateguard-reduce> ${fixtures}/premature_reclaim.sgtrace)
  set_tests_properties(cli-reduce-stdin PROPERTIES PASS_REGULAR_EXPRESSION "premature_reclaim")
  add_test(NAME cli-check-quiet
    COMMAND stateguard-check --quiet ${fixtures}/premature_reclaim.sgtrace)
  set_tests_properties(cli-check-quiet PROPERTIES PASS_REGULAR_EXPRESSION "^violation\r?\n$")
  foreach(tool check generate reduce timeline)
    add_test(NAME cli-${tool}-version COMMAND stateguard-${tool} --version)
    set_tests_properties(cli-${tool}-version PROPERTIES
      PASS_REGULAR_EXPRESSION "^stateguard-${tool} ${PROJECT_VERSION}\r?\n$")
    add_test(NAME cli-${tool}-help COMMAND stateguard-${tool} --help)
    set_tests_properties(cli-${tool}-help PROPERTIES PASS_REGULAR_EXPRESSION "exit codes:")
  endforeach()

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
      PASS_REGULAR_EXPRESSION "^clean\r?\n$")
    add_test(NAME cli-check-harness-immediate
      COMMAND stateguard-check ${cli_dir}/immediate.sgtrace)
    set_tests_properties(cli-check-harness-immediate PROPERTIES
      FIXTURES_REQUIRED harness_traces
      PASS_REGULAR_EXPRESSION "^violation\r?\n[0-9]+ premature_reclaim")
    add_test(NAME cli-check-harness-failed-retry
      COMMAND stateguard-check ${cli_dir}/failed-retry.sgtrace)
    set_tests_properties(cli-check-harness-failed-retry PROPERTIES
      FIXTURES_REQUIRED harness_traces
      PASS_REGULAR_EXPRESSION "^clean\r?\n$")
    add_test(NAME cli-check-harness-failed-publish
      COMMAND stateguard-check ${cli_dir}/failed-publish.sgtrace)
    set_tests_properties(cli-check-harness-failed-publish PROPERTIES
      FIXTURES_REQUIRED harness_traces
      PASS_REGULAR_EXPRESSION "^violation\r?\n[0-9]+ failed_write_publication")
    add_test(NAME cli-check-harness-failed-publish-follow-on
      COMMAND stateguard-check ${cli_dir}/failed-publish.sgtrace)
    set_tests_properties(cli-check-harness-failed-publish-follow-on PROPERTIES
      FIXTURES_REQUIRED harness_traces
      PASS_REGULAR_EXPRESSION "\r?\n[0-9]+ invalid_state_transition \\[follow-on\\]: consumption requires")
    add_test(NAME cli-check-harness-primary-only
      COMMAND stateguard-check --primary-only ${cli_dir}/immediate.sgtrace)
    set_tests_properties(cli-check-harness-primary-only PROPERTIES
      FIXTURES_REQUIRED harness_traces
      PASS_REGULAR_EXPRESSION "^violation\r?\n[0-9]+ premature_reclaim"
      FAIL_REGULAR_EXPRESSION "follow-on|stale_generation|orphaned_operation")
    # Cancellation timeline on the steady clock with a 20 ms injected delay.
    add_test(NAME cancellation-demo
      COMMAND stateguard-cancellation-demo --delay-ms 20 --trace-dir ${cli_dir})
    set_tests_properties(cancellation-demo PROPERTIES FIXTURES_SETUP cancel_traces)
    foreach(name delayed shared)
      add_test(NAME cli-check-cancel-${name}
        COMMAND stateguard-check ${cli_dir}/cancel-${name}.sgtrace)
      set_tests_properties(cli-check-cancel-${name} PROPERTIES
        FIXTURES_REQUIRED cancel_traces
        PASS_REGULAR_EXPRESSION "^clean\r?\n$")
    endforeach()
    add_test(NAME cli-timeline-cancel-delayed
      COMMAND stateguard-timeline ${cli_dir}/cancel-delayed.sgtrace)
    set_tests_properties(cli-timeline-cancel-delayed PROPERTIES
      FIXTURES_REQUIRED cancel_traces
      PASS_REGULAR_EXPRESSION "^complete\r?\nclock steady\r?\n.*cancellation_to_reuse [0-9]+ ns")
    add_test(NAME cli-timeline-cancel-shared
      COMMAND stateguard-timeline ${cli_dir}/cancel-shared.sgtrace)
    set_tests_properties(cli-timeline-cancel-shared PROPERTIES
      FIXTURES_REQUIRED cancel_traces
      PASS_REGULAR_EXPRESSION "not_attributed slot-1#1")
    add_test(NAME cli-timeline-cancel-missing
      COMMAND stateguard-timeline ${cli_dir}/cancel-missing.sgtrace)
    set_tests_properties(cli-timeline-cancel-missing PROPERTIES
      FIXTURES_REQUIRED cancel_traces
      PASS_REGULAR_EXPRESSION "^incomplete\r?\n.*incomplete: operation copy-1 of request request-1 has no observed access completion")
    add_test(NAME cli-check-cancel-missing
      COMMAND stateguard-check ${cli_dir}/cancel-missing.sgtrace)
    set_tests_properties(cli-check-cancel-missing PROPERTIES
      FIXTURES_REQUIRED cancel_traces
      PASS_REGULAR_EXPRESSION "^inconclusive\r?\n")
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
        PASS_REGULAR_EXPRESSION "^clean\r?\n$")
    endforeach()
    foreach(name immediate immediate-delayed)
      add_test(NAME cli-check-cuda-${name}
        COMMAND stateguard-check ${cli_dir}/cuda-${name}.sgtrace)
      set_tests_properties(cli-check-cuda-${name} PROPERTIES
        FIXTURES_REQUIRED cuda_traces LABELS gpu
        PASS_REGULAR_EXPRESSION "^violation\r?\n[0-9]+ premature_reclaim")
    endforeach()
  endif()
endif()

# CPU alpha delivery. Optional adapter dependencies and test/harness libraries
# are not part of the installed core package.
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
install(TARGETS stateguard EXPORT stateguardTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
file(RELATIVE_PATH stateguard_lib_from_bin
  "${CMAKE_INSTALL_FULL_BINDIR}" "${CMAKE_INSTALL_FULL_LIBDIR}")
if(APPLE)
  set(stateguard_install_rpath "@loader_path/${stateguard_lib_from_bin}")
elseif(UNIX)
  set(stateguard_install_rpath "$ORIGIN/${stateguard_lib_from_bin}")
endif()
foreach(tool check generate reduce timeline)
  set_target_properties(stateguard-${tool} PROPERTIES INSTALL_RPATH "${stateguard_install_rpath}")
  install(TARGETS stateguard-${tool} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endforeach()
if(STATEGUARD_BUILD_EXAMPLES)
  foreach(demo instrumented async-copy cancellation)
    set_target_properties(stateguard-${demo}-demo PROPERTIES INSTALL_RPATH "${stateguard_install_rpath}")
  endforeach()
  install(TARGETS stateguard-instrumented-demo stateguard-async-copy-demo
    stateguard-cancellation-demo RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
install(DIRECTORY include/stateguard DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY tests/fixtures/ DESTINATION ${CMAKE_INSTALL_DATADIR}/stateguard/traces
  FILES_MATCHING PATTERN "*.sgtrace")
install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/stateguard)
set(package_dir ${CMAKE_INSTALL_LIBDIR}/cmake/stateguard)
configure_package_config_file(cmake/stateguardConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/stateguardConfig.cmake
  INSTALL_DESTINATION ${package_dir})
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/stateguardConfigVersion.cmake
  VERSION ${PROJECT_VERSION} COMPATIBILITY ExactVersion)
install(EXPORT stateguardTargets NAMESPACE stateguard:: DESTINATION ${package_dir})
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/stateguardConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/stateguardConfigVersion.cmake DESTINATION ${package_dir})
configure_file(cmake/uninstall.cmake.in ${CMAKE_CURRENT_BINARY_DIR}/uninstall.cmake @ONLY)
add_custom_target(uninstall
  COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_BINARY_DIR}/uninstall.cmake)
