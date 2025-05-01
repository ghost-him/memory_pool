//
// Created by ghost-him on 25-4-27.
//

#include "utils.h"

#include <cassert>
#include <cstdint>

namespace memory_pool {
    void page_span::allocate(memory_span memory) {
        assert(is_valid_unit_span(memory));
        uint64_t address_offset = memory.data() - m_memory.data();
        uint64_t index = address_offset / m_unit_size;
        assert(m_allocated_map[index] == 0);
        m_allocated_map[index] = true;
    }

    void page_span::deallocate(memory_span memory) {
        // 判断这个空间是不是被管理的
        assert(is_valid_unit_span(memory));
        uint64_t address_offset = memory.data() - m_memory.data();
        uint64_t index = address_offset / m_unit_size;
        assert(m_allocated_map[index] == 1);
        m_allocated_map[index] = false;
    }

    bool page_span::is_valid_unit_span(memory_span memory)  {
        // 如果归还的空间的大小与这个页面管理的大小不一样，则报错
        if (memory.size() != m_unit_size)
            return false;
        // 如果这个地址的起始地址小于了管理的地址，则报错
        if (memory.data() < m_memory.data())
            return false;
        // 如果内存的地址对不上，则报错
        // 地址之差，一定为正数
        uint64_t address_offset = memory.data() - m_memory.data();
        // 内存都没对齐，一定是错的
        if (address_offset % m_unit_size != 0)
            return false;
        // 对比结束的地址是不是小于或等于管理区域的结束地址
        // 单元的结束地址 = memory.data() + memory.size()
        // m_memory 的结束地址 = m_memory.data() + m_memory.size()
        // memory.data() + memory.size() <= m_memory.data() + m_memory.size()
        // memory.data() = m_memory.data() + address_offset
        // memory.size() = m_unit_size
        // m_memory.data() + address_offset + m_unit_size <= m_memory.data() + m_memory.size()
        // address_offset + m_unit_size <= m_memory.size()
        return address_offset + m_unit_size <= m_memory.size();
    }
} // memory_pool
