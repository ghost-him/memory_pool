#include <iostream>

#include "memory_pool/utils.h"

int main() {
    std::cout << memory_pool::size_utils::get_index(2) << std::endl;
    return 0;
}
