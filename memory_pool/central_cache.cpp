//
// Created by ghost-him on 25-4-28.
//

#include "central_cache.h"

#include <cassert>
#include <cstdlib>
#include <sys/mman.h>
#include <cstring>
#include <iostream>
#include <thread>

#include "page_cache.h"

namespace memory_pool {
    std::optional<std::list<memory_span>> central_cache::allocate(size_t memory_size, size_t block_count) {
        // 内存的传入应该一定是8的倍数
        assert(memory_size % 8 == 0);
        // 一次性申请的空间只可以小于512，如果出错了，则一定是代码写错了，所以使用assert
        assert(block_count < page_span::MAX_UNIT_COUNT);

        if (memory_size == 0 || block_count == 0) {
            return std::nullopt;
        }

        if (memory_size > size_utils::MAX_CACHED_UNIT_SIZE) {
            return page_cache::get_instance().allocate_unit(memory_size).transform([this](memory_span&& memory) {
                return std::list<memory_span> {memory};
            });
        }

        size_t index = size_utils::get_index(memory_size);
        std::list<memory_span> result;
        while (m_status[index].test_and_set(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        try {
            if (m_free_array[index].size() < block_count) {
                // 如果当前缓存的个数小于申请的块数，则向页分配器申请

                // 一共要申请的大小
                //size_t total_size = block_count * memory_size;
                // 要申请的页面的个数
                // 原本使用这个，只分配适量的空间
                //size_t allocate_page_count = size_utils::align(total_size, size_utils::PAGE_SIZE) / size_utils::PAGE_SIZE;
                // 现在改成直接分配能分配的最大的大小
                size_t allocate_page_count = size_utils::align(memory_size * size_utils::MAX_CACHED_UNIT_SIZE, size_utils::PAGE_SIZE) / size_utils::PAGE_SIZE;
                auto ret = get_page_from_page_cache(allocate_page_count);
                if (!ret.has_value()) {
                    m_status[index].clear(std::memory_order_release);
                    return std::nullopt;
                }
                memory_span memory = ret.value();

                // 用于管理这个页面
                page_span page_span(memory, memory_size);

                // 根据分配出来的内存，实际可以划分出的内存
                size_t split_size = memory.size() / memory_size;

                for (size_t i = 0; i < block_count; i++) {
                    memory_span split_memory = memory.subspan(0, memory_size);
                    memory = memory.subspan(memory_size);
                    result.push_back(split_memory);
                    // 这个页面已经被分配出去了
                    page_span.allocate(split_memory);
                }

                // 完成页面分配的管理
                auto start_addr = page_span.data();
                auto [_, succeed] = m_page_set[index].emplace(start_addr, std::move(page_span));
                // 如果插入失败了，说明代码写的有问题
                assert(succeed == true);

                // 多余的值存到空闲列表中
                split_size -= block_count;
                for (size_t i = 0; i < split_size; i++) {
                    memory_span split_memory = memory.subspan(0, memory_size);
                    memory = memory.subspan(memory_size);
                    m_free_array[index].push_back(split_memory);
                }
            } else {
                // 直接从中心缓存区中分配内存
                for (size_t i = 0; i < block_count; i++) {
                    memory_span memory = m_free_array[index].front();
                    m_free_array[index].pop_front();
                    result.push_back(memory);
                    record_allocated_memory_span(memory);
                }
            }
        } catch (...) {
            m_status[index].clear(std::memory_order_release);
            throw std::runtime_error("Memory allocation failed");
            return std::nullopt;
        }
        m_status[index].clear(std::memory_order_release);

        for (auto i = result.begin(); i != result.end(); i++) {
            assert(i->size() == memory_size);
        }

        assert(result.size() == block_count);

        return result;
    }

    void central_cache::deallocate(std::list<memory_span> memories) {
        if (memories.empty()) {
            return;
        }

        if (memories.begin()->size() > page_span::MAX_UNIT_COUNT) {
            // 如果是超大内存块，则直接返回给page_cache管理
            page_cache::get_instance().deallocate_unit(*memories.begin());
            return;
        }
        size_t index = size_utils::get_index(memories.begin()->size());
        while (m_status[index].test_and_set(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        for (const auto & memory : memories) {
            // 先归还到数组中
            m_free_array[index].push_back(memory);
            // 然后再还给页面管理器中
            auto memory_data = memory.data();
            auto it = m_page_set[index].upper_bound(memory_data);
            assert(it != m_page_set[index].begin());
            -- it;
            it->second.deallocate(memory);
            // 同时判断需不需要返回给页面管理器
            if (it->second.is_empty()) {
                // 如果已经还清内存了，则将这块内存还给页面管理器(page_cache)
                auto page_start_addr = it->second.data();
                auto page_end_addr = page_start_addr + it->second.size();
                assert(it->second.unit_size() == memory.size());
                auto mem_iter = m_free_array[index].begin();
                // 遍历这个数组
                while (mem_iter != m_free_array[index].end()) {
                    auto memory_start_addr = mem_iter->data();
                    auto memory_end_addr = memory_start_addr + mem_iter->size();
                    if (memory_start_addr >= page_start_addr && memory_end_addr <= page_end_addr) {
                        // 如果这个内存在这个范围内，则说明是正确的
                        // 一定是满足要求的，如果不满足，则说明代码写错了
                        assert(it->second.is_valid_unit_span(*mem_iter));
                        // 指向下一个
                        mem_iter = m_free_array[index].erase(mem_iter);
                    } else {
                        ++ mem_iter;
                    }
                }
                memory_span page_memory = it->second.get_memory_span();
                m_page_set[index].erase(it);
                page_cache::get_instance().deallocate_page(page_memory);
            }
        }

        m_status[index].clear(std::memory_order_release);
    }

    void central_cache::record_allocated_memory_span(memory_span memory) {
        size_t index = size_utils::get_index(memory.size());
        auto it = m_page_set[index].upper_bound(memory.data());
        assert(it != m_page_set[index].begin());
        -- it;
        it->second.allocate(memory);
    }

    std::optional<memory_span> central_cache::get_page_from_page_cache(size_t page_allocate_count) {
        return page_cache::get_instance().allocate_page(page_allocate_count);
    }
}
