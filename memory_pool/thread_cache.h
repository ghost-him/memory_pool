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
#include <unordered_map>

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

private:

    /// 向高层申请一块空间
    std::optional<memory_span> allocate_from_central_cache(size_t memory_size);

    /// 当前还没有被分配的内存
    std::array<std::list<memory_span>, size_utils::CACHE_LINE_SIZE> m_free_cache;

    /// 动态分配内存
    size_t compute_allocate_count(size_t memory_size);

    /// 用于表示下一次再申请指定大小的内存时，会申请几个内存
    std::array<size_t, size_utils::CACHE_LINE_SIZE> m_next_allocate_count;

    /// 设置每个列表缓存的上限为256KB（对于16KB的对象即为缓存 256KB / 16KB = 16个）
    /// 这个阈值的设置需要分析，如果常用的分配的量比较少
    /// 比如只申请几个固定大小的空间，则这个值可以设置的大一些
    /// 而申请的内存空间的大小很复杂，则需要设置的小一些，不然可能会让单个线程的空间占用过多
    static constexpr size_t MAX_FREE_BYTES_PER_LISTS = 256 * 1024;

};

} // memory_pool

#endif //THREAD_CACHE_H
