#include <iostream>

#include "memory_pool_v2/thread_cache.h"

int main() {
    size_t size = 16392;
    std::cout << memory_pool::thread_cache::get_instance().compute_allocate_count(size);
    // size_t allocate_page_count = memory_pool::size_utils::align(7456, memory_pool::size_utils::PAGE_SIZE) / memory_pool::size_utils::PAGE_SIZE;
    // std::cout << allocate_page_count << std::endl;
    return 0;
}
