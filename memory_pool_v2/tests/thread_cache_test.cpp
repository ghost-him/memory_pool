#include <gtest/gtest.h>
#include <vector>
#include <numeric> // For std::iota
#include <algorithm> // For std::max
#include <cstddef> // For std::byte

// 包含被测试代码的头文件
#include "thread_cache.h" // 包含 thread_cache 类定义
#include "utils.h"       // 包含 size_utils, check_ptr_length 等
#include "central_cache.h" // 包含 central_cache 声明 (需要链接其实现)
// 该内容由 gemini 2.5 pro preview 03-25 生成，https://aistudio.google.com/prompts/new_chat
// 测试 Fixture

// 运行这个测试代码时，需要将成员变量公开

class ThreadCacheTest : public ::testing::Test {
protected:
    using byte = std::byte;
    memory_pool_v2::thread_cache* tc = nullptr; // 指向当前线程的 thread_cache 实例

    // Helper to get aligned size and index
    static size_t get_aligned_size(size_t size) {
        return memory_pool_v2::size_utils::align(size);
    }
    static size_t get_index_for_size(size_t size) {
        return memory_pool_v2::size_utils::get_index(get_aligned_size(size));
    }

    void SetUp() override {
        // 1. 获取当前线程的 thread_cache 实例
        tc = &memory_pool_v2::thread_cache::get_instance();

        // 2. 重置 thread_cache 的状态
        // 非常重要：由于 thread_local，状态会在同一线程的测试间保持。
        // 我们需要清理缓存，将内存归还给 Central Cache 以避免内存泄漏
        // 并重置计数器，确保每个测试从干净的状态开始。
        for (size_t i = 0; i < memory_pool_v2::size_utils::CACHE_LINE_SIZE; ++i) {
            if (tc->m_free_cache[i] != nullptr) {
                // 计算这个列表对应的内存大小
                size_t block_size = (i + 1) * memory_pool_v2::size_utils::ALIGNMENT;
                // 调用 central_cache 的 deallocate 来归还整个链表
                // 注意：这依赖于一个可工作的 central_cache 实现
                memory_pool_v2::central_cache::get_instance().deallocate(tc->m_free_cache[i], block_size);

                // 清空 thread_cache 内部记录
                tc->m_free_cache[i] = nullptr;
                tc->m_free_cache_size[i] = 0;
            }
             // 重置下一次分配计数器
            tc->m_next_allocate_count[i] = 0; // 确保 compute_allocate_count 从基线开始
        }
    }

    void TearDown() override {
        // 可选：可以在这里执行额外的清理，但 SetUp 应该足以隔离测试。
        // 如果 SetUp 中归还内存失败或 central_cache 实现有问题，这里可能也无法补救。
    }
};

// --- 测试用例 ---

TEST_F(ThreadCacheTest, AllocateSizeZero) {
    auto result = tc->allocate(0);
    EXPECT_FALSE(result.has_value());
}

TEST_F(ThreadCacheTest, DeallocateNullOrSizeZero) {
    // 准备一个指针（从 central_cache 获取，确保它是一个“有效”指针，即使我们不立即使用）
    // 注意：这需要 central_cache 能分配少量内存
    size_t dummy_size = 8;
    size_t count = 1;
    auto mem_opt = memory_pool_v2::central_cache::get_instance().allocate(dummy_size, count);
    // 如果 central_cache 无法分配，则无法进行此测试的部分内容
    ASSERT_TRUE(mem_opt.has_value() && mem_opt.value() != nullptr);
    void* dummy_ptr = mem_opt.value();

    // 获取初始状态（例如，某个索引的计数）
    size_t index_to_check = get_index_for_size(16); // 随便选一个索引
    size_t initial_count = tc->m_free_cache_size[index_to_check];
    byte* initial_ptr = tc->m_free_cache[index_to_check];

    // 调用 deallocate
    tc->deallocate(nullptr, 100); // 释放 null 指针
    tc->deallocate(dummy_ptr, 0);   // 释放大小为 0

    // 验证内部状态没有改变
    EXPECT_EQ(tc->m_free_cache_size[index_to_check], initial_count);
    EXPECT_EQ(tc->m_free_cache[index_to_check], initial_ptr);

    // 清理：手动释放 dummy_ptr 回 central_cache，因为它没有被 thread_cache 处理
    memory_pool_v2::central_cache::get_instance().deallocate(static_cast<byte*>(dummy_ptr), dummy_size);
}


TEST_F(ThreadCacheTest, AllocateSmallBlockCacheMiss) {
    using namespace memory_pool_v2;
    const size_t alloc_size = 16;
    const size_t aligned_size = get_aligned_size(alloc_size);
    const size_t index = get_index_for_size(alloc_size);

    // 初始状态检查
    ASSERT_EQ(tc->m_free_cache[index], nullptr);
    ASSERT_EQ(tc->m_free_cache_size[index], 0);
    size_t initial_next_count = tc->m_next_allocate_count[index]; // 可能为 0

    // 执行分配
    auto result = tc->allocate(alloc_size);

    // 验证结果
    ASSERT_TRUE(result.has_value());
    EXPECT_NE(result.value(), nullptr); // 应成功分配

    // 验证 thread_cache 内部状态 (Cache Miss 后)
    // 1. free list 现在应该非空（除非 central_cache 只返回了1个块）
    // 2. free list 的 size 应该是 N-1 (N 是从 central_cache 获取的数量)
    // 3. next_allocate_count 应该增加了

    // compute_allocate_count 逻辑: 至少申请4个，或 m_next_allocate_count[index]
    size_t expected_fetch_count = std::max(initial_next_count, static_cast<size_t>(4));
    // compute_allocate_count 还会限制数量，但我们假设这里不会触发上限
    // 实际从 central_cache 获取的数量可能因其内部逻辑而变化，我们检查 size > 0
    EXPECT_GT(tc->m_free_cache_size[index], 0); // 应该至少有 N-1 个在缓存中
    EXPECT_NE(tc->m_free_cache[index], nullptr); // 链表头不应为空
    EXPECT_GT(tc->m_next_allocate_count[index], initial_next_count); // 下次分配计数应该增加了

    // 检查分配的块数（需要 central_cache 返回正确的 block_count）
    // 这部分比较难验证，因为我们无法直接知道 central_cache 返回了多少
    // 但可以检查 m_free_cache_size[index] > 0
}

TEST_F(ThreadCacheTest, AllocateSmallBlockCacheHit) {
    using namespace memory_pool_v2;
    const size_t alloc_size = 32;
    const size_t aligned_size = get_aligned_size(alloc_size);
    const size_t index = get_index_for_size(alloc_size);

    // 1. 先分配一次，触发 Cache Miss 以填充缓存
    auto ptr1_opt = tc->allocate(alloc_size);
    ASSERT_TRUE(ptr1_opt.has_value());
    void* ptr1 = ptr1_opt.value();
    size_t count_after_alloc1 = tc->m_free_cache_size[index];
    byte* list_head_after_alloc1 = tc->m_free_cache[index];
    ASSERT_GT(count_after_alloc1, 0); // 确认缓存中有东西了

    // 2. 释放刚刚分配的块，它应该回到 free list 的头部
    tc->deallocate(ptr1, alloc_size);
    EXPECT_EQ(tc->m_free_cache_size[index], count_after_alloc1 + 1); // 数量增加 1
    EXPECT_EQ(tc->m_free_cache[index], reinterpret_cast<byte*>(ptr1)); // ptr1 现在是列表头
    // 验证 ptr1 内部现在指向之前的链表头
    if (list_head_after_alloc1 != nullptr) { // 如果之前链表非空
       EXPECT_EQ(*(reinterpret_cast<byte**>(ptr1)), list_head_after_alloc1);
    } else { // 如果之前链表为空 (即 central_cache 只给了1个块)
       EXPECT_EQ(*(reinterpret_cast<byte**>(ptr1)), nullptr);
    }


    // 3. 再次分配相同大小的块 (Cache Hit)
    auto ptr2_opt = tc->allocate(alloc_size);

    // 验证结果
    ASSERT_TRUE(ptr2_opt.has_value());
    EXPECT_EQ(ptr2_opt.value(), ptr1); // 应返回刚刚释放的那个块 (LIFO)

    // 验证内部状态
    EXPECT_EQ(tc->m_free_cache_size[index], count_after_alloc1); // 数量恢复
    EXPECT_EQ(tc->m_free_cache[index], list_head_after_alloc1); // 链表头恢复
}

TEST_F(ThreadCacheTest, AllocateLargeBlockDirect) {
    using namespace memory_pool_v2;
    const size_t large_size = size_utils::MAX_CACHED_UNIT_SIZE + 8;

    // 初始状态检查（所有缓存列表应为空）
    for (size_t i = 0; i < size_utils::CACHE_LINE_SIZE; ++i) {
        ASSERT_EQ(tc->m_free_cache[i], nullptr);
        ASSERT_EQ(tc->m_free_cache_size[i], 0);
    }

    // 执行大内存分配
    auto result = tc->allocate(large_size);

    // 验证结果
    ASSERT_TRUE(result.has_value()); // 假设 central_cache 能分配
    EXPECT_NE(result.value(), nullptr);

    // 验证 thread_cache 内部状态（不应缓存大块内存）
    for (size_t i = 0; i < size_utils::CACHE_LINE_SIZE; ++i) {
        EXPECT_EQ(tc->m_free_cache[i], nullptr);
        EXPECT_EQ(tc->m_free_cache_size[i], 0);
    }

    // 清理：需要手动释放，因为它没经过 thread cache 缓存
     if (result.has_value()) {
        tc->deallocate(result.value(), large_size); // 使用 deallocate 也能处理大内存
        // 或者直接调用 central_cache::get_instance().deallocate(...)
     }
}

TEST_F(ThreadCacheTest, DeallocateLargeBlockDirect) {
     using namespace memory_pool_v2;
    const size_t large_size = size_utils::MAX_CACHED_UNIT_SIZE + 16;

    // 1. 先分配一个大块内存 (直接从 central_cache)
    auto ptr_opt = tc->allocate(large_size);
    ASSERT_TRUE(ptr_opt.has_value());
    void* large_ptr = ptr_opt.value();

    // 检查分配后缓存仍为空
     for (size_t i = 0; i < size_utils::CACHE_LINE_SIZE; ++i) {
        ASSERT_EQ(tc->m_free_cache[i], nullptr);
        ASSERT_EQ(tc->m_free_cache_size[i], 0);
    }

    // 2. 释放这个大块内存
    tc->deallocate(large_ptr, large_size);

    // 验证 thread_cache 内部状态（应无变化，内存直接还给 central_cache）
     for (size_t i = 0; i < size_utils::CACHE_LINE_SIZE; ++i) {
        EXPECT_EQ(tc->m_free_cache[i], nullptr);
        EXPECT_EQ(tc->m_free_cache_size[i], 0);
    }
     // 注意：我们无法验证 central_cache 是否真的收到了内存，除非 central_cache 提供查询接口
}

TEST_F(ThreadCacheTest, AlignmentTest) {
    using namespace memory_pool_v2;
    const size_t unaligned_size = 10; // 小于 ALIGNMENT
    const size_t aligned_size = get_aligned_size(unaligned_size); // 应该是 ALIGNMENT (e.g., 8) or 16 depending on ALIGNMENT
    const size_t index = get_index_for_size(unaligned_size); // 索引基于对齐后的大小

    // 初始状态
    ASSERT_EQ(tc->m_free_cache[index], nullptr);
    ASSERT_EQ(tc->m_free_cache_size[index], 0);

    // 分配 (使用未对齐大小)
    auto ptr_opt = tc->allocate(unaligned_size);
    ASSERT_TRUE(ptr_opt.has_value());
    void* ptr = ptr_opt.value();

    // 分配后，检查对应 *对齐大小* 的列表状态
    size_t count_after_alloc = tc->m_free_cache_size[index];
    EXPECT_GT(count_after_alloc, 0); // 应该填充了缓存

    // 释放 (使用未对齐大小)
    tc->deallocate(ptr, unaligned_size);

    // 释放后，检查对应 *对齐大小* 的列表状态
    EXPECT_EQ(tc->m_free_cache_size[index], count_after_alloc + 1); // 数量应增加
    EXPECT_EQ(tc->m_free_cache[index], reinterpret_cast<byte*>(ptr)); // 指针应在列表头
}

TEST_F(ThreadCacheTest, DeallocateTriggersReturnToCentralCache) {
    using namespace memory_pool_v2;
    const size_t alloc_size = 128; // 选择一个大小
    const size_t aligned_size = get_aligned_size(alloc_size);
    const size_t index = get_index_for_size(alloc_size);

    // 计算刚好超过阈值的块数
    const size_t max_bytes = thread_cache::MAX_FREE_BYTES_PER_LISTS;
    const size_t max_blocks_before_exceeding = max_bytes / aligned_size;
    const size_t trigger_count = max_blocks_before_exceeding + 1; // 第 trigger_count 个块会触发回收

    // 预期回收的块数 = trigger_count / 2
    const size_t expected_recycle_count = trigger_count / 2;
    ASSERT_GT(expected_recycle_count, 0); // 确保计算合理

    // --- 阶段 1: 分配足够多的块 ---
    // 为了确保我们能控制缓存中的块数，我们先分配 trigger_count 个块，
    // 然后把它们全部释放回 thread_cache。
    std::vector<void*> allocated_pointers;
    allocated_pointers.reserve(trigger_count);

    // 分配 trigger_count 块
    for (size_t i = 0; i < trigger_count; ++i) {
        auto ptr_opt = tc->allocate(alloc_size);
        // 如果中途分配失败，测试无法继续
        ASSERT_TRUE(ptr_opt.has_value()) << "Allocation failed at iteration " << i;
        allocated_pointers.push_back(ptr_opt.value());
    }

    // 记录分配完 trigger_count 块后缓存的状态（可能非空）
    size_t count_after_allocs = tc->m_free_cache_size[index];
    byte* list_head_after_allocs = tc->m_free_cache[index];
    size_t initial_next_alloc_count = tc->m_next_allocate_count[index]; // 记录触发前的计数

    // --- 阶段 2: 释放所有块，直到触发回收 ---
    // 逐个释放，监控 free_cache_size 的变化
    size_t current_cache_size = count_after_allocs;
    for (size_t i = 0; i < trigger_count; ++i) {
        void* ptr_to_deallocate = allocated_pointers[i];
        size_t size_before_dealloc = tc->m_free_cache_size[index];

        tc->deallocate(ptr_to_deallocate, alloc_size);

        size_t size_after_dealloc = tc->m_free_cache_size[index];

        // 检查是否触发了回收
        // 如果触发，size_after_dealloc 应该远小于 size_before_dealloc + 1
        // 并且等于 (size_before_dealloc + 1) - expected_recycle_count
        if ((size_before_dealloc + 1) * aligned_size > max_bytes) {
            // 这次 deallocate 应该触发了回收
            size_t expected_size_after_recycle = (size_before_dealloc + 1) - expected_recycle_count;
            EXPECT_EQ(size_after_dealloc, expected_size_after_recycle)
                << "Recycle did not reduce size correctly at iteration " << i;
            // 验证 m_next_allocate_count 减少了
            EXPECT_LT(tc->m_next_allocate_count[index], initial_next_alloc_count)
                << "Next allocate count did not decrease after recycle at iteration " << i;
            // 我们可以停止循环，因为回收已经发生
             break;
        } else {
            // 未触发回收，大小应该只增加 1
            EXPECT_EQ(size_after_dealloc, size_before_dealloc + 1)
                << "Size did not increment correctly before trigger at iteration " << i;
            // 检查 next_allocate_count 在回收前不应改变（或按其自身逻辑改变，但不因回收减少）
            // 这个断言可能太强，因为next_allocate_count可能在allocate时改变
            // EXPECT_EQ(tc->m_next_allocate_count[index], initial_next_alloc_count);
        }
         // 记录下一次迭代前的 next_allocate_count，以防它在非回收 deallocate 中被修改（虽然代码里看似不会）
         initial_next_alloc_count = tc->m_next_allocate_count[index];
    }

     // 最终检查：确认回收确实发生了（如果我们没有在循环中 break）
     // 可以检查最终的 m_free_cache_size[index] 是否小于等于 trigger_count - expected_recycle_count
     ASSERT_LE(tc->m_free_cache_size[index], trigger_count - expected_recycle_count + count_after_allocs)
        << "Final cache size seems too large, maybe recycle didn't happen.";
}


// 可以添加更多测试用例，例如：
// - 测试 compute_allocate_count 在多次 cache miss 后的增长行为
// - 测试分配接近 MAX_CACHED_UNIT_SIZE 的边界情况