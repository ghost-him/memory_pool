#include <iostream>
#include <vector>

// 包含对应版本的头文件，在cmake的选项中可以看到有 memory_pool_v2_exe 与 memory_pool_v1_exe，这两个链接的内存池不一样，要选择正确了
#include "memory_pool_v2/memory_pool.h"

struct MyData {
    int id;
    double value;
    char buffer[100];
};

int main() {
    // 分配内存
    size_t size_to_alloc = sizeof(MyData);
    std::optional<void*> mem_opt = memory_pool_v2::memory_pool::allocate(size_to_alloc);

    if (mem_opt) {
        void* mem = *mem_opt;
        std::cout << "Allocated " << size_to_alloc << " bytes at " << mem << std::endl;

        // 使用内存 (Placement new)
        MyData* data_ptr = new (mem) MyData{1, 3.14, "Hello"};
        std::cout << "Data ID: " << data_ptr->id << ", Value: " << data_ptr->value << std::endl;

        // 使用完需要显式调用析构函数 (如果对象有非平凡析构)
        data_ptr->~MyData();

        // 归还内存
        memory_pool_v2::memory_pool::deallocate(mem, size_to_alloc);
        std::cout << "Deallocated memory at " << mem << std::endl;

    } else {
        std::cerr << "Memory allocation failed!" << std::endl;
    }

    // 分配大内存 (超过 16KB)
    size_t large_size = 20 * 1024;
    std::optional<void*> large_mem_opt = memory_pool_v2::memory_pool::allocate(large_size);
    if (large_mem_opt) {
        void* large_mem = *large_mem_opt;
        std::cout << "Allocated large memory (" << large_size << " bytes) at " << large_mem << std::endl;
        // ... 使用 ...
        memory_pool_v2::memory_pool::deallocate(large_mem, large_size);
        std::cout << "Deallocated large memory at " << large_mem << std::endl;
    }


    // 注意：内存池对象通常是单例，在程序结束时会自动清理（归还向系统申请的内存）
    // page_cache 的析构函数会调用 stop() 来释放 page_vector 中的内存

    return 0;
}