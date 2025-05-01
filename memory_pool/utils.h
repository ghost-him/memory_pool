//
// Created by ghost-him on 25-4-27.
//

#ifndef UTILS_H
#define UTILS_H
#include <bitset>
#include <cassert>
#include <cstddef>
#include <span>

namespace memory_pool {

    class memory_span {
    public:
        memory_span(std::byte* data, const std::size_t size) :m_data(data), m_size(size) {}
        memory_span(const memory_span& other) = default;
        memory_span& operator=(const memory_span& other) = default;
        std::byte* data() const { return m_data; }
        std::size_t size() const { return m_size; }
        auto operator<=>(const memory_span & other) const { return m_data <=> other.m_data; }
        auto operator==(const memory_span & other) const { return m_data == other.m_data && m_size == other.m_size; }
        memory_span subspan(const std::size_t offset, const std::size_t size) const { return memory_span {m_data + offset, size}; }
        memory_span subspan(const std::size_t offset) const { return memory_span {m_data + offset, m_size - offset}; }
    private:
        // 当前管理的内存的起始地址
        std::byte* m_data;
        // 当前管理的内存的长度
        std::size_t m_size;
    };

    class size_utils {
    public:
        // 一个指标的大小
        static constexpr size_t ALIGNMENT = sizeof(void *);
        static constexpr size_t PAGE_SIZE = 4096;
        //最大可以接受 64 * 8 = 512B 的对象
        static constexpr size_t CACHE_LINE_SIZE = 64;
        // 这个值就是缓存的最大的内容
        static constexpr size_t MAX_CACHED_UNIT_SIZE = (CACHE_LINE_SIZE) * size_utils::ALIGNMENT;
        /// 内存字节数对齐，对齐成8的倍数，8字节也是内存池最小的分配大小
        static size_t align(const size_t memory_size, const size_t alignment = ALIGNMENT) {
            return (memory_size + alignment - 1) & ~(alignment - 1);
        }

        static size_t get_index(const size_t memory_size) {
            return align(memory_size) / ALIGNMENT - 1;
        }
    };

    // 这个类用于管理从page_cache中分配下来的内存
    class page_span {
    public:
        // 4096 / 8 = 512。考虑到32位的系统，这里就使用了静态变量。
        static constexpr size_t MAX_UNIT_COUNT = size_utils::PAGE_SIZE / size_utils::ALIGNMENT;
        /// 初始化这个page_span
        /// 参数：span:这个page_span管理的空间，unit_size
        page_span(const memory_span span, const size_t unit_size): m_memory(span), m_unit_size(unit_size) {};

        // 根据内存地址的起始位置进行相比
        auto operator<=>(const page_span& other) const {
            return m_memory.data() <=> other.m_memory.data();
        }

        // 当前的页面是不是全都没有被分配
        bool is_empty() {
            return m_allocated_map.none();
        }

        // 申请一块内存
        void allocate(memory_span memory);

        // 把这一块内存归还给页面
        void deallocate(memory_span memory);

        // 判断某一段空间是不是被这个管理的
        bool is_valid_unit_span(memory_span memory);

        // 管理的内存长度
        size_t size() { return m_memory.size(); }

        // 起始地址
        std::byte* data() {
            return m_memory.data();
        }

        // 维护的长度
        size_t unit_size() { return m_unit_size; }

        // 获得这个所维护的地址
        memory_span get_memory_span() { return m_memory; }

    private:
        // 这个page_span管理的空间大小
        const memory_span m_memory;
        // 一个分配单位的大小
        const size_t m_unit_size;
        // 用于管理目前页面的分配情况（4096 / 8 = 512）
        // 这个是可以管理多个page合并的情况的，但是由于bitset是不可以动态分配的
        // 所以这里的值决定了整体的分配情况
        std::bitset<MAX_UNIT_COUNT> m_allocated_map;
    };


} // memory_pool

#endif //UTILS_H
