//
// Created by ghost-him on 25-4-28.
//

#ifndef CENTRAL_CACHE_H
#define CENTRAL_CACHE_H
#include <atomic>
#include <list>
#include <map>
#include <span>
#include <optional>
#include <mutex>
#include <set>
#include <unordered_map>

#include "utils.h"

namespace memory_pool {
    // 中心存储器
    class central_cache {
    public:
        // 一次性申请8页的空间
        static constexpr size_t PAGE_SPAN = 8;
        static central_cache& get_instance() {
            static central_cache instance;
            return instance;
        }

        /// 用于分配指向个数的指向大小的空间
        /// 参数：memory_size: 要申请的大小 block_count: 申请的个数
        /// 返回值：返回一组相同大小的指定个数的内存块
        std::optional<std::list<memory_span>> allocate(size_t memory_size, size_t block_count);

        /// 回收内存块
        /// 参数memory: 从线程缓存池中回收的内存碎片
        /// 注意点：这一个列表中，每一个内存块大小必须是一样的。
        void deallocate(std::list<memory_span> memories);

    private:
        /// 将分配出去的内存块记录下来
        void record_allocated_memory_span(memory_span memory);

        std::optional<memory_span> get_page_from_page_cache(size_t page_allocate_count);

        std::array<std::list<memory_span>, size_utils::CACHE_LINE_SIZE> m_free_array;
        std::array<std::atomic_flag, size_utils::CACHE_LINE_SIZE> m_status;
        // 用于页面的管理
        std::array<std::map<std::byte*, page_span>, size_utils::CACHE_LINE_SIZE> m_page_set;
    };
}


#endif //CENTRAL_CACHE_H
