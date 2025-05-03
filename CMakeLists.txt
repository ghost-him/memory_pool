cmake_minimum_required(VERSION 4.0)
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


add_executable(memory_pool_exe main.cpp)

target_link_libraries(memory_pool_exe PRIVATE
        memory_pool_lib
)
