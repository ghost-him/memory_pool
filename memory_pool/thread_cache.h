//
// Created by ghost-him on 25-4-27.
//

#ifndef THREAD_CACHE_H
#define THREAD_CACHE_H
#include <array>
#include <list>
#include <optional>
#include <set>
#include "utils.h"
#include <span>

namespace memory_pool {

class thread_cache {
    public:
    static thread_cache& get_instance() {
        static thread_local thread_cache instance;
        return instance;
    }

    /// 向内存池申请一块空间
    /// 参数：要申请的大小
    /// 返回值：指向空间的指针，可能会申请失败
    [[nodiscard("不应该忽略这个值，还需要手动归还到内存池中")]] std::optional<void*> allocate(size_t memory_size);

    /// 向内存池归还一片空间
    /// 参数： start_p:内存开始的地址, size_t：这片地址的大小
    void deallocate(void* start_p, size_t memory_size);

    /// 设置最大空闲内存回收块的上限
    void set_max_free_memory_blocks(const size_t max_free_blocks) noexcept { m_max_free_memory_blocks = max_free_blocks; }

    /// 获取当前线程的回收机制
    [[nodiscard("如果不需要该值，则不要调用该函数")]] size_t get_max_free_memory_blocks() const noexcept { return m_max_free_memory_blocks; }

private:

    /// 向高层申请一块空间
    std::optional<memory_span> allocate_from_central_cache(size_t memory_size);

    /// 当前还没有被分配的内存
    std::array<std::list<memory_span>, size_utils::CACHE_LINE_SIZE> m_free_cache;

    /// 动态分配内存
    size_t compute_allocate_count(size_t memory_size);

    /// 设置回收的上限
    size_t m_max_free_memory_blocks = 256;

};

} // memory_pool

#endif //THREAD_CACHE_H
