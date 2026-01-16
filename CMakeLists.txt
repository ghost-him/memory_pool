cmake_minimum_required(VERSION 3.30)
project(memory_pool)

set(CMAKE_CXX_STANDARD 23 REQUIRED)

add_subdirectory(memory_pool)
add_subdirectory(memory_pool_v2)

enable_testing()

include(FetchContent)
FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip # Or specific commit/tag
)

FetchContent_MakeAvailable(googletest)

find_package(Threads REQUIRED)


add_executable(memory_pool_v1_exe main.cpp)
target_link_libraries(memory_pool_v1_exe PRIVATE
        memory_pool_lib
)

add_executable(memory_pool_v2_exe main.cpp)
target_link_libraries(memory_pool_v2_exe PRIVATE
        memory_pool_v2_lib
)

# Convenience targets to run v2 executable, tests and performance binary
add_custom_target(run_v2
        COMMAND $<TARGET_FILE:memory_pool_v2_exe>
        DEPENDS memory_pool_v2_exe
        COMMENT "Run memory_pool v2 executable"
)

add_custom_target(test_v2
        COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure --tests-regex v2
        DEPENDS page_cache_test_v2 page_span_test_v2 central_cache_test_v2 thread_cache_test_v2 memory_pool_test_v2
        COMMENT "Run v2 unit tests (ctest --tests-regex v2)"
)

add_custom_target(perf_v2
        COMMAND $<TARGET_FILE:memory_pool_performance_v2>
        DEPENDS memory_pool_performance_v2
        COMMENT "Run memory_pool v2 performance test"
)

# One-command target to run v2 exe, tests and perf in sequence
add_custom_target(run_all_v2
        DEPENDS run_v2 test_v2 perf_v2
        COMMENT "Run v2 executable, tests and performance tests"
)