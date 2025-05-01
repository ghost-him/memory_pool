add_library(memory_pool_lib
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

target_include_directories(memory_pool_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

find_package(Threads REQUIRED)
target_link_libraries(memory_pool_lib PRIVATE Threads::Threads)