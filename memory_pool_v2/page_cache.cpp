//
// Created by ghost-him on 25-4-27.
//

#include "page_cache.h"

#include <cassert>
#include <cstring>
#include <iostream>
#include <mutex>
#include <bits/ostream.tcc>
#include <sys/mman.h>

namespace memory_pool_v2 {
    std::optional<memory_span> page_cache::allocate_page(size_t page_count) {
        if (page_count == 0) {
            return std::nullopt;
        }
        std::unique_lock<std::mutex> guard(m_mutex);

        auto it = free_page_store.lower_bound(page_count);
        while (it != free_page_store.end()) {
            if (!it->second.empty()) {
                // 如果存在一个页面，这个页面的大小是大于或等于要分配的页面的
                auto mem_iter = it->second.begin();
                memory_span free_memory = *mem_iter;
                it->second.erase(mem_iter);
                free_page_map.erase(free_memory.data());

                // 开始分割获取出来的空闲的空间
                size_t memory_to_use = page_count * size_utils::PAGE_SIZE;
                memory_span memory = free_memory.subspan(0, memory_to_use);
                free_memory = free_memory.subspan(memory_to_use);
                if (free_memory.size()) {
                    // 如果还有空间，则插回到缓存中
                    free_page_store[free_memory.size() / size_utils::PAGE_SIZE].emplace(free_memory);
                    free_page_map.emplace(free_memory.data(), free_memory);
                }

                return memory;
            }
            ++ it;
        }
        // 如果已经没有足够大的页面了，则向系统申请
        // 一次性分配8MB的大小，为2048个页面，而批量申请的全都取最大是4mb，-> 16KB(缓存最大大小) * 512(一次性管理最大个数) = 4MB
        size_t page_to_allocate = std::max(PAGE_ALLOCATE_COUNT, page_count);
        return system_allocate_memory(page_to_allocate).transform([this, page_count](memory_span memory) {
            // 存入总的内存，用于结尾回收内存
            page_vector.push_back(memory);
            size_t memory_to_use = page_count * size_utils::PAGE_SIZE;
            memory_span result = memory.subspan(0, memory_to_use);
            memory_span free_memory = memory.subspan(memory_to_use);
            if (free_memory.size()) {
                size_t index = free_memory.size() / size_utils::PAGE_SIZE;
                free_page_store[index].emplace(free_memory);
                free_page_map.emplace(free_memory.data(), free_memory);
            }
            return result;
        });
    }

    void page_cache::deallocate_page(memory_span page) {

        // 应该是一页一页的回收的，所以大小一定是会被整除的
        assert(page.size() % size_utils::PAGE_SIZE == 0);
        std::unique_lock<std::mutex> guard(m_mutex);
        while (!free_page_map.empty()) {
            // 只有在集合不空的时候才会考虑合并
            // 这个空间不应该已经被包含了
            assert(!free_page_map.contains(page.data()));
            auto it = free_page_map.upper_bound(page.data());
            if (it != free_page_map.begin()) {
                // 检查前一个span
                -- it;
                const memory_span& memory = it->second;
                if (memory.data() + memory.size() == page.data()) {
                    // 如果前面一段的空间与当前的相邻，则合并
                    page = memory_span(memory.data(), memory.size() + page.size());
                    // 在储存库中也删除
                    free_page_store[memory.size() / size_utils::PAGE_SIZE].erase(memory);
                    free_page_map.erase(it);
                } else {
                    break;
                }
            } else {
                break;
            }
        }

        // 检查后面相邻的span
        while (!free_page_map.empty()) {
            assert(!free_page_map.contains(page.data()));
            if (free_page_map.contains(page.data() + page.size())) {
                auto it = free_page_map.find(page.data() + page.size());
                memory_span next_memory = it->second;
                free_page_store[next_memory.size() / size_utils::PAGE_SIZE].erase(next_memory);
                free_page_map.erase(it);
                page = memory_span(page.data(), page.size() + next_memory.size());
            } else {
                break;
            }
        }
        size_t index = page.size() / size_utils::PAGE_SIZE;
        free_page_store[index].emplace(page);
        free_page_map.emplace(page.data(), page);
    }

    std::optional<memory_span> page_cache::allocate_unit(size_t memory_size) {
        auto ret = malloc(memory_size);
        if (ret != nullptr) {
            return memory_span { static_cast<std::byte*>(ret), memory_size};
        }
        return std::nullopt;
    }

    void page_cache::deallocate_unit(memory_span memories) {
        free(memories.data());
    }

    void page_cache::stop() {
        std::unique_lock<std::mutex> guard(m_mutex);
        if (m_stop == false) {
            m_stop = true;
            for (auto& i : page_vector) {
                system_deallocate_memory(i);
            }
        }
    }

    page_cache::~page_cache() {
        stop();
    }

    std::optional<memory_span> page_cache::system_allocate_memory(size_t page_count) {
        const size_t size = page_count * size_utils::PAGE_SIZE;

        // 使用mmap分配内存
        void* ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (ptr == MAP_FAILED) return std::nullopt;

        // 清零内存
        memset(ptr, 0, size);
        return memory_span{static_cast<std::byte*>(ptr), size};
    }

    void page_cache::system_deallocate_memory(memory_span page) {
        munmap(page.data(), page.size());
    }
} // memory_pool