cmake_minimum_required(VERSION 4.0)
project(memory_pool)

set(CMAKE_CXX_STANDARD 23 REQUIRED)

add_subdirectory(memory_pool)

enable_testing()

include(FetchContent)
FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip # Or specific commit/tag
)

FetchContent_MakeAvailable(googletest)

find_package(Threads REQUIRED)


add_executable(memory_pool_exe main.cpp)

target_link_libraries(memory_pool_exe PRIVATE
        memory_pool_lib
)


add_executable(page_cache_test tests/page_cache_test.cpp)

target_link_libraries(page_cache_test PRIVATE
        memory_pool_lib
        GTest::gtest_main
)

add_executable(page_span_test tests/page_span_test.cpp)

target_link_libraries(page_span_test PRIVATE
        memory_pool_lib
        GTest::gtest_main
)

add_executable(central_cache_test tests/central_cache_test.cpp)
target_link_libraries(central_cache_test PRIVATE
        memory_pool_lib
        GTest::gtest_main
        Threads::Threads
)

add_executable(thread_cache_test tests/thread_cache_test.cpp)
target_link_libraries(thread_cache_test PRIVATE
        memory_pool_lib
        GTest::gtest_main
        Threads::Threads
)

add_executable(memory_pool_test tests/memory_pool_test.cpp)
target_link_libraries(memory_pool_test PRIVATE
        memory_pool_lib
        GTest::gtest_main
        Threads::Threads # Needed for multi-threading tests
)


# Discover tests using CTest
include(GoogleTest)
gtest_discover_tests(page_cache_test page_span_test central_cache_test memory_pool_test)