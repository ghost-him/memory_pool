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

        size_t index = size_utils::get_index(memory_size);
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
        memory_size = size_utils::align(memory_size);
        memory_span memory(static_cast<std::byte*>(start_p), memory_size);
        // 如果大于了最大缓存值了，说明是直接从中心缓存区申请的，可以直接返还给中心缓存区
        if (memory_size > size_utils::MAX_CACHED_UNIT_SIZE) {
            std::list<memory_span> free_list;
            free_list.push_back(memory);
            central_cache::get_instance().deallocate(std::move(free_list));
            return;
        }
        size_t index = size_utils::get_index(memory_size);
        m_free_cache[index].push_front(memory);

        // 检测一下需不需要回收
        if (m_free_cache[index].size() > m_max_free_memory_blocks) {
            // 如果超过了，则回收一半的多余的内存块
            size_t deallocate_block_size = m_max_free_memory_blocks / 2;
            std::list<memory_span> memory_to_deallocate;
            auto middle = m_free_cache[index].begin();
            std::advance(middle, deallocate_block_size);
            memory_to_deallocate.splice(memory_to_deallocate.begin(), m_free_cache[index], middle, m_free_cache[index].end());
            central_cache::get_instance().deallocate(std::move(memory_to_deallocate));
        }
    }

    std::optional<memory_span> thread_cache::allocate_from_central_cache(size_t memory_size) {
        size_t block_count = compute_allocate_count(memory_size);
        return central_cache::get_instance().allocate(memory_size, block_count).transform([this, memory_size](std::list<memory_span>&& memory_list) {
            memory_span result = memory_list.front();
            assert(result.size() == memory_size);
            memory_list.pop_front();
            size_t index = size_utils::get_index(memory_size);
            m_free_cache[index].splice(m_free_cache[index].end(), memory_list);
            return result;
        });
    }

    size_t thread_cache::compute_allocate_count(size_t memory_size) {
        if (memory_size <= 32)
            return 256;
        if (memory_size <= 64)
            return 128;
        if (memory_size <= 128)
            return 64;
        if (memory_size <= 256)
            return 32;
        if (memory_size <= size_utils::MAX_CACHED_UNIT_SIZE)
            return 16;
        return 1;
    }
} // memory_pool