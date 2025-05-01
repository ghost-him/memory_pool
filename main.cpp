#include <iostream>

#include "memory_pool.h"
#include "memory_pool/utils.h"

int main() {

    size_t allocate_page_count = memory_pool::size_utils::align(7456, memory_pool::size_utils::PAGE_SIZE) / memory_pool::size_utils::PAGE_SIZE;
    std::cout << allocate_page_count << std::endl;
    return 0;
}
