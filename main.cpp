#include <iostream>

#include "memory_pool.h"
#include "memory_pool/utils.h"

int main() {
    auto ptr = memory_pool::memory_pool::allocate(446);

    return 0;
}
