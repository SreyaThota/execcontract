cmake_minimum_required(VERSION 3.20)
project(execcontract VERSION 0.1.0 DESCRIPTION "Execution-contract probe for launcher integration tests" LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "ExecContract targets Linux only (procfs, getrlimit, Linux getcwd semantics).")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(EXECCONTRACT_BUILD_TESTS "Build unit tests and register component tests" ON)
option(EXECCONTRACT_WERROR "Treat project warnings as errors" OFF)
option(EXECCONTRACT_SANITIZE "Build everything with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)

# Third-party code comes from system packages only; no network access at
# configure or build time.
find_package(nlohmann_json 3.9 REQUIRED)

if(EXECCONTRACT_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

# Warning policy for project code only. Imported third-party targets expose
# their include directories as SYSTEM, so their headers are not affected.
set(EXECCONTRACT_TEST_WARNINGS -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow
    -Wnon-virtual-dtor -Wold-style-cast -Wcast-align -Wformat=2)
if(EXECCONTRACT_WERROR)
  list(APPEND EXECCONTRACT_TEST_WARNINGS -Werror)
endif()
# -Wnull-dereference only for production code: with -O2 it reports false
# positives inside std::optional copies in heavily inlined test code.
set(EXECCONTRACT_WARNINGS ${EXECCONTRACT_TEST_WARNINGS} -Wnull-dereference)

add_library(execcontract_core STATIC
  src/cli.cpp
  src/contract.cpp
  src/evaluate.cpp
  src/model.cpp
  src/observe.cpp
  src/report.cpp
  src/sys.cpp)
target_include_directories(execcontract_core PUBLIC src)
target_link_libraries(execcontract_core PUBLIC nlohmann_json::nlohmann_json)
target_compile_definitions(execcontract_core PRIVATE EXECCONTRACT_VERSION="${PROJECT_VERSION}")
target_compile_options(execcontract_core PRIVATE ${EXECCONTRACT_WARNINGS})

add_executable(execcontract src/main.cpp)
target_link_libraries(execcontract PRIVATE execcontract_core)
target_compile_options(execcontract PRIVATE ${EXECCONTRACT_WARNINGS})

include(GNUInstallDirs)
install(TARGETS execcontract RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(EXECCONTRACT_BUILD_TESTS)
  enable_testing()
  find_package(GTest REQUIRED)
  include(GoogleTest)

  add_executable(execcontract_unit_tests
    tests/unit/test_cli.cpp
    tests/unit/test_contract.cpp
    tests/unit/test_evaluate.cpp
    tests/unit/test_model.cpp
    tests/unit/test_observe.cpp
    tests/unit/test_report.cpp
    tests/unit/test_sys.cpp)
  target_link_libraries(execcontract_unit_tests PRIVATE execcontract_core GTest::gtest_main)
  target_compile_options(execcontract_unit_tests PRIVATE ${EXECCONTRACT_TEST_WARNINGS})
  # Bounded per-test timeout: a hung unit test must fail, not stall the run.
  gtest_discover_tests(execcontract_unit_tests DISCOVERY_MODE PRE_TEST PROPERTIES LABELS unit TIMEOUT 30)

  find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
  # Exact descriptor-inventory assertions are only meaningful for the normal
  # (uninstrumented) build; sanitizer runtimes may hold extra descriptors.
  set(EXECCONTRACT_PYTEST_ARGS -q -rs)
  set(EXECCONTRACT_TEST_ENV
    "EXECCONTRACT_BIN=$<TARGET_FILE:execcontract>"
    "EXECCONTRACT_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}")
  if(EXECCONTRACT_SANITIZE)
    list(APPEND EXECCONTRACT_PYTEST_ARGS -m "not exact_fds")
    # The ASan runtime lowers RLIMIT_CORE before main() unless told not to,
    # which would contaminate the observation under test.
    list(APPEND EXECCONTRACT_TEST_ENV "ASAN_OPTIONS=disable_coredump=0:detect_leaks=1:abort_on_error=1"
      "UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1")
  endif()
  add_test(NAME component
    COMMAND Python3::Interpreter -m pytest ${EXECCONTRACT_PYTEST_ARGS} -p no:cacheprovider
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/component)
  set_tests_properties(component PROPERTIES
    LABELS component
    ENVIRONMENT "${EXECCONTRACT_TEST_ENV}"
    TIMEOUT 300)
endif()
