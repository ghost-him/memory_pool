//
// Created by ghost-him on 25-4-27.
//

#ifndef MEMORY_POOL_H
#define MEMORY_POOL_H
#include <optional>

#include "thread_cache.h"

namespace memory_pool {

class memory_pool {
public:
    /// 向内存池申请一块空间
    /// 参数：要申请的大小
    /// 返回值：指向空间的指针，可能会申请失败
    static std::optional<void*> allocate(size_t memory_size) {
        return thread_cache::get_instance().allocate(memory_size);
    }

    /// 向内存池归还一片空间
    /// 参数： start_p:内存开始的地址, size_t：这片地址的大小
    static void deallocate(void* start_p, size_t memory_size) {
        thread_cache::get_instance().deallocate(start_p, memory_size);
    }
};

} // memory_pool

#endif //MEMORY_POOL_H
