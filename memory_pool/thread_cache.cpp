//
// Created by ghost-him on 25-4-27.
//

#include "thread_cache.h"

#include <assert.h>

#include "central_cache.h"
#include "utils.h"

namespace memory_pool {
    std::optional<void *> thread_cache::allocate(size_t memory_size) {
        if (memory_size == 0) {
            return std::nullopt; // 对于大小为0的情况立即返回nullopt
        }

        // 将memory_size的大小对齐到8字节
        memory_size = size_utils::align(memory_size);
        if (memory_size > size_utils::MAX_CACHED_UNIT_SIZE) {
            return allocate_from_central_cache(memory_size).and_then([](memory_span && span) { return std::optional<void*>(span.data()); });
        }

        const size_t index = size_utils::get_index(memory_size);
        if (!m_free_cache[index].empty()) {
            auto result = m_free_cache[index].front();
            m_free_cache[index].pop_front();
            // 在release模式下会被移除，这个只用于检测代码是否有问题
            assert(result.size() == memory_size);
            return result.data();
        }
        return allocate_from_central_cache(memory_size).and_then([](memory_span && span) { return std::optional<void*>(span.data()); });
    }

    void thread_cache::deallocate(void *start_p, size_t memory_size) {
        if (memory_size == 0) {
            return ;
        }
        memory_size = size_utils::align(memory_size);
        memory_span memory(static_cast<std::byte*>(start_p), memory_size);
        // 如果大于了最大缓存值了，说明是直接从中心缓存区申请的，可以直接返还给中心缓存区
        if (memory_size > size_utils::MAX_CACHED_UNIT_SIZE) {
            std::list<memory_span> free_list;
            free_list.push_back(memory);
            central_cache::get_instance().deallocate(std::move(free_list));
            return;
        }
        const size_t index = size_utils::get_index(memory_size);
        m_free_cache[index].push_front(memory);

        // 检测一下需不需要回收
        // 如果当前的列表所维护的大小已经超过了阈值，则触发资源回收
        // 维护的大小 = 个数 × 单个空间的大小
        if (m_free_cache[index].size() * memory_size > MAX_FREE_BYTES_PER_LISTS) {
            // 如果超过了，则回收一半的多余的内存块
            size_t deallocate_block_size = m_free_cache[index].size() / 2;
            std::list<memory_span> memory_to_deallocate;
            auto middle = m_free_cache[index].begin();
            std::advance(middle, deallocate_block_size);
            memory_to_deallocate.splice(memory_to_deallocate.begin(), m_free_cache[index], middle, m_free_cache[index].end());
            central_cache::get_instance().deallocate(std::move(memory_to_deallocate));
            // 在回收工作完成以后，还要调整这个空间大小的申请的个数
            // 减半下一次申请的个数
            m_next_allocate_count[index] /= 2;
        }
    }

    std::optional<memory_span> thread_cache::allocate_from_central_cache(size_t memory_size) {
        size_t block_count = compute_allocate_count(memory_size);
        return central_cache::get_instance().allocate(memory_size, block_count).transform([this, memory_size](std::list<memory_span>&& memory_list) {
            memory_span result = memory_list.front();
            assert(result.size() == memory_size);
            memory_list.pop_front();
            const size_t index = size_utils::get_index(memory_size);
            m_free_cache[index].splice(m_free_cache[index].end(), memory_list);
            return result;
        });
    }

    size_t thread_cache::compute_allocate_count(size_t memory_size) {
        // 获取其下标
        size_t index = size_utils::get_index(memory_size);

        if (index >= size_utils::CACHE_LINE_SIZE) {
            return 1;
        }

        // 最少申请4个块
        size_t result = std::max(m_next_allocate_count[index], static_cast<size_t>(4));


        // 计算下一次要申请的个数，默认乘2
        size_t next_allocate_count = result * 2;
        // 要确保不会超过center_cache一次申请的最大个数
        next_allocate_count = std::min(next_allocate_count, page_span::MAX_UNIT_COUNT);
        // 同时也要确保不会超过一个列表维护的最大容量
        // 比如16KB的内存块，不能一次性申请128个吧
        // 256 * 1024 B / 16 * 1024 B / 2 = 8个（这里就将16KB的内存一次性最多申请8个，要给点冗余(除2)，不然可能会反复申请）
        next_allocate_count = std::min(next_allocate_count, MAX_FREE_BYTES_PER_LISTS / memory_size / 2);
        // 更新下一次要申请的个数
        m_next_allocate_count[index] = next_allocate_count;
        // 返回这一次申请的个数
        return result;
    }
} // memory_pool