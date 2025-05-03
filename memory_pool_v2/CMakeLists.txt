add_library(memory_pool_v2_lib
        thread_cache.cpp
        thread_cache.h
        memory_pool.cpp
        memory_pool.h
        page_cache.cpp
        page_cache.h
        utils.cpp
        utils.h
        central_cache.cpp
        central_cache.h
        utils.cpp
)

target_include_directories(memory_pool_v2_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

find_package(Threads REQUIRED)
target_link_libraries(memory_pool_v2_lib PRIVATE Threads::Threads)

add_executable(memory_pool_performance_v2 performance_test.cpp)

target_link_libraries(memory_pool_performance_v2 PRIVATE
        memory_pool_v2_lib
)

add_executable(page_cache_test_v2 tests/page_cache_test.cpp)

target_link_libraries(page_cache_test_v2 PRIVATE
        memory_pool_v2_lib
        GTest::gtest_main
)

add_executable(page_span_test_v2 tests/page_span_test.cpp)

target_link_libraries(page_span_test_v2 PRIVATE
        memory_pool_v2_lib
        GTest::gtest_main
)

add_executable(central_cache_test_v2 tests/central_cache_test.cpp)
target_link_libraries(central_cache_test_v2 PRIVATE
        memory_pool_v2_lib
        GTest::gtest_main
        Threads::Threads
)

add_executable(thread_cache_test_v2 tests/thread_cache_test.cpp)
target_link_libraries(thread_cache_test_v2 PRIVATE
        memory_pool_v2_lib
        GTest::gtest_main
        Threads::Threads
)

add_executable(memory_pool_test_v2 tests/memory_pool_test.cpp)
target_link_libraries(memory_pool_test_v2 PRIVATE
        memory_pool_v2_lib
        GTest::gtest_main
        Threads::Threads # Needed for multi-threading tests
)

# Discover tests using CTest
include(GoogleTest)
gtest_discover_tests(page_cache_test_v2 page_span_test central_cache_test memory_pool_test)