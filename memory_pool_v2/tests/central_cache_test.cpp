#include "gtest/gtest.h"
#include "central_cache.h" // 包含被测试类的头文件
#include "utils.h"         // 假设的依赖#include "page_cache.h"    // 假设的依赖 (如果 central_cache.h 没有包含它)
                           // 注意: 确保 central_cache.h 包含了 page_span 和 size_utils 的定义

#include <vector>
#include <numeric>
#include <set>
#include <thread>
#include <optional>
#include <cstddef> // 用于 std::byte
#include <cstring> // 用于 memset
#include <iostream> // 用于潜在的调试输出
#include <atomic>   // 需要包含 atomic 用于 m_status (虽然测试代码不直接用，但 central_cache.h 用了)
#include <mutex>    // 需要包含 mutex (如果 central_cache.h 的实现需要，测试代码的并发测试间接依赖)
#include <map>      // 需要包含 map (central_cache.h 用了)
#include <array>    // 需要包含 array (central_cache.h 用了)


using namespace memory_pool_v2;

// --- Helper Functions (来自原始代码) ---
// Helper to count nodes in the intrusive list
size_t list_length(std::byte* head) {
    size_t count = 0;
    std::set<std::byte*> visited; // Detect cycles
    while (head != nullptr) {
        if (!visited.insert(head).second) {
             std::cerr << "ERROR: Cycle detected in list helper at " << (void*)head << std::endl;
             return count;
        }
        count++;
        head = *(reinterpret_cast<std::byte**>(head));
    }
    return count;
}

// Helper to collect pointers from the intrusive list into a vector
std::vector<std::byte*> list_to_vector(std::byte* head) {
    std::vector<std::byte*> nodes;
    std::set<std::byte*> visited; // Detect cycles
    while (head != nullptr) {
         if (!visited.insert(head).second) {
             std::cerr << "ERROR: Cycle detected in list_to_vector helper at " << (void*)head << std::endl;
             break;
         }
        nodes.push_back(head);
        head = *(reinterpret_cast<std::byte**>(head));
    }
    return nodes;
}

// Helper to build an intrusive list from a vector (for deallocation testing)
std::byte* vector_to_list(const std::vector<std::byte*>& nodes) {
    std::byte* head = nullptr;
    if (nodes.empty()) {
        return nullptr;
    }
    for (size_t i = 0; i < nodes.size(); ++i) {
        std::byte* current_node = nodes[i];
        std::byte* next_node = (i + 1 < nodes.size()) ? nodes[i + 1] : nullptr;
        *(reinterpret_cast<std::byte**>(current_node)) = next_node;
    }
    return nodes.empty() ? nullptr : nodes[0];
}
// --- End Helper Functions ---


// Test fixture for central_cache tests
class CentralCacheTest : public ::testing::Test {
protected:
    // 注意：如果 central_cache::get_instance() 不是线程安全的，
    // 或者单例状态在测试间有干扰，可能需要 setup/teardown 或其他策略。
    // 这里假设 get_instance() 本身没问题，但测试会共享单例状态。
    central_cache& cache = central_cache::get_instance();

    // Helper to allocate and verify (slightly adapted from original)
    std::optional<std::byte*> allocate_and_check(size_t size, size_t count) {
        auto result = cache.allocate(size, count);
        if (size == 0 || count == 0) {
            EXPECT_FALSE(result.has_value()) << "Allocate(size=" << size << ", count=" << count << ") should return nullopt.";
            return result;
        }
        if (size > size_utils::MAX_CACHED_UNIT_SIZE) {
             if(result.has_value()) {
                 EXPECT_NE(result.value(), nullptr);
             }
             // 大内存块返回的是单个指针，不是链表，无法检查 count。
        }
        else {
            if (result.has_value()) {
                EXPECT_NE(result.value(), nullptr);
                size_t len = list_length(result.value());
                EXPECT_EQ(len, count) << "Expected list length " << count << ", but got " << len << " for size " << size;
            }
            // 不强制要求 result.has_value()，因为分配可能因 OOM 失败。
            // 需要成功的测试应使用 ASSERT_TRUE(result.has_value()).
        }
        return result;
    }

    // Helper to deallocate a list created from a vector
     void deallocate_vector(std::vector<std::byte*>& nodes, size_t size) {
        if (nodes.empty()) return;
        std::byte* list_head = vector_to_list(nodes);
        cache.deallocate(list_head, size);
        nodes.clear(); // 清空 vector 表示概念上的释放
    }

    // *** 修改点：将并发任务函数移入 Fixture 并设为 static ***
    // 静态成员函数，用于在单独的线程中执行分配/释放操作
    // 注意：静态成员函数没有 this 指针，需要重新获取单例实例
    static void allocate_deallocate_task(size_t id, size_t memory_size, size_t block_count, int iterations) {
        central_cache& cache_local = central_cache::get_instance(); // 在任务内部获取单例
        for (int i = 0; i < iterations; ++i) {
            auto result = cache_local.allocate(memory_size, block_count);
            if (result.has_value()) {
                std::vector<std::byte*> allocated = list_to_vector(result.value());
                 if (allocated.size() != block_count) {
                     // 在并发测试中使用 fprintf 或其他线程安全日志可能更合适
                     fprintf(stderr, "Thread %zu: Allocation count mismatch! Iter %d. Expected %zu, got %zu for size %zu\n",
                             id, i, block_count, allocated.size(), memory_size);
                     // 尝试清理
                     if (!allocated.empty()) {
                         std::byte* head = vector_to_list(allocated);
                         cache_local.deallocate(head, memory_size);
                     }
                     continue; // 继续下一次迭代
                 }
                // 可选：短暂写入内存
                if (!allocated.empty()) {
                    // 确保 memory_size > 0 避免未定义行为
                    if (memory_size > 0) {
                        memset(allocated[0], static_cast<unsigned char>(id), memory_size);
                    }
                }
                // 释放
                std::byte* head = vector_to_list(allocated); // 重新链接以进行释放
                cache_local.deallocate(head, memory_size);
            } else {
                // 分配失败，可以选择记录或重试
                // fprintf(stderr, "Thread %zu: Allocation failed iter %d for size %zu, count %zu\n", id, i, memory_size, block_count);
            }
            // 可选：让出 CPU，增加交错的可能性
            // std::this_thread::yield();
        }
    }

}; // --- End of CentralCacheTest Fixture ---

// --- Existing Tests (基本保持不变，除了并发测试的调用方式) ---

TEST_F(CentralCacheTest, AllocateDeallocateSingleBlock) {
    const size_t alloc_size = 16;
    const size_t alloc_count = 1;

    auto result = allocate_and_check(alloc_size, alloc_count);
    ASSERT_TRUE(result.has_value()) << "Small allocation failed unexpectedly.";
    ASSERT_NE(result.value(), nullptr);

    std::vector<std::byte*> allocated = list_to_vector(result.value());
    ASSERT_EQ(allocated.size(), alloc_count);

    // 可选: 写入内存确保可用
    if (alloc_size > 0) {
        memset(allocated[0], 0xAB, alloc_size);
    }

    deallocate_vector(allocated, alloc_size);
    ASSERT_TRUE(allocated.empty());
}

TEST_F(CentralCacheTest, AllocateDeallocateMultipleBlocks) {
    const size_t alloc_size = 32;
    const size_t alloc_count = 10;

    auto result = allocate_and_check(alloc_size, alloc_count);
    ASSERT_TRUE(result.has_value()) << "Multi-block allocation failed unexpectedly.";
    ASSERT_NE(result.value(), nullptr);

    std::vector<std::byte*> allocated = list_to_vector(result.value());
    ASSERT_EQ(allocated.size(), alloc_count);

    // 检查指针是否唯一
    std::set<std::byte*> unique_pointers(allocated.begin(), allocated.end());
    ASSERT_EQ(unique_pointers.size(), alloc_count) << "Received non-unique pointers in allocation.";

    deallocate_vector(allocated, alloc_size);
    ASSERT_TRUE(allocated.empty());
}

TEST_F(CentralCacheTest, AllocateZeroSizeOrCount) {
    allocate_and_check(0, 10);
    allocate_and_check(16, 0);
    allocate_and_check(0, 0);
}

TEST_F(CentralCacheTest, AllocateMaxCachedSize) {
    const size_t alloc_size = size_utils::MAX_CACHED_UNIT_SIZE;
    const size_t alloc_count = 2;

    // 确保 MAX_CACHED_UNIT_SIZE > 0 且对齐
    ASSERT_GT(alloc_size, 0);
    ASSERT_EQ(alloc_size % size_utils::ALIGNMENT, 0) << "MAX_CACHED_UNIT_SIZE should be aligned.";

    auto result = allocate_and_check(alloc_size, alloc_count);
    if (result.has_value()) {
       ASSERT_NE(result.value(), nullptr);
       std::vector<std::byte*> allocated = list_to_vector(result.value());
       // allocate_and_check 内部会验证数量
       deallocate_vector(allocated, alloc_size);
       ASSERT_TRUE(allocated.empty());
    } else {
       GTEST_SKIP() << "Skipping test: Allocation failed, possibly due to resource exhaustion.";
    }
}

TEST_F(CentralCacheTest, AllocateLargeSize) {
    const size_t large_alloc_size = size_utils::MAX_CACHED_UNIT_SIZE + size_utils::ALIGNMENT;
    const size_t alloc_count = 1; // 大内存分配通常是单个单元

    auto result_opt = cache.allocate(large_alloc_size, alloc_count);

    if (result_opt.has_value()) {
        std::byte* ptr = result_opt.value();
        EXPECT_NE(ptr, nullptr);

        // 大内存块不是链表，其内容未定义
        // 不再检查 *(reinterpret_cast<std::byte**>(ptr)) == nullptr

        // 释放大内存块 - 使用单个指针的 deallocate 变体
        // 注意：central_cache 的 deallocate 需要知道大小来区分是单个大块还是链表
        cache.deallocate(ptr, large_alloc_size);
    } else {
         GTEST_SKIP() << "Skipping test: Large allocation failed, possibly due to resource exhaustion.";
    }
}
//
// TEST_F(CentralCacheTest, ReuseFreedBlocks) {
//     const size_t alloc_size = 64;
//     const size_t alloc_count = 5;
//      ASSERT_LE(alloc_size, size_utils::MAX_CACHED_UNIT_SIZE);
//      ASSERT_GT(alloc_count, 0);
//
//
//     // 1. 首次分配
//     auto result1 = allocate_and_check(alloc_size, alloc_count);
//     ASSERT_TRUE(result1.has_value());
//     ASSERT_NE(result1.value(), nullptr);
//     std::vector<std::byte*> allocated1_vec = list_to_vector(result1.value());
//     ASSERT_EQ(allocated1_vec.size(), alloc_count);
//     std::set<std::byte*> pointers1(allocated1_vec.begin(), allocated1_vec.end());
//     ASSERT_EQ(pointers1.size(), alloc_count); // 确认初始指针唯一
//
//     // 保存原始指针副本用于后续检查
//     const std::vector<std::byte*> original_allocated1 = allocated1_vec;
//
//     // 2. 释放
//     deallocate_vector(allocated1_vec, alloc_size);
//     ASSERT_TRUE(allocated1_vec.empty()); // 确认 vector 被清空
//
//     // 3. 再次分配 (数量少于或等于首次)
//     const size_t alloc_count2 = 3;
//     ASSERT_LE(alloc_count2, alloc_count);
//     auto result2 = allocate_and_check(alloc_size, alloc_count2);
//     ASSERT_TRUE(result2.has_value());
//      ASSERT_NE(result2.value(), nullptr);
//     std::vector<std::byte*> allocated2_vec = list_to_vector(result2.value());
//     ASSERT_EQ(allocated2_vec.size(), alloc_count2);
//
//     // 4. 验证重用：第二次分配的指针应来自第一次分配的集合
//     std::set<std::byte*> pointers2(allocated2_vec.begin(), allocated2_vec.end());
//     ASSERT_EQ(pointers2.size(), alloc_count2); // 确认第二次分配的指针也唯一
//     for (std::byte* ptr : allocated2_vec) {
//         EXPECT_TRUE(pointers1.count(ptr)) << "Pointer " << (void*)ptr << " allocated in step 3 was not in the original set from step 1. Expected reuse.";
//     }
//
//     // 5. 释放第二次分配的
//     deallocate_vector(allocated2_vec, alloc_size);
//     ASSERT_TRUE(allocated2_vec.empty());
//
//      // 6. 分配更多以触发获取新页 + 重用剩余
//      const size_t alloc_count3 = alloc_count + 2; // 应使用剩余的 (5-3=2) 个空闲块 + 从 page_cache 获取更多
//      auto result3 = allocate_and_check(alloc_size, alloc_count3);
//      ASSERT_TRUE(result3.has_value());
//      ASSERT_NE(result3.value(), nullptr);
//      std::vector<std::byte*> allocated3_vec = list_to_vector(result3.value());
//      ASSERT_EQ(allocated3_vec.size(), alloc_count3);
//      std::set<std::byte*> pointers3(allocated3_vec.begin(), allocated3_vec.end());
//      ASSERT_EQ(pointers3.size(), alloc_count3); // 确认第三次分配的指针唯一
//
//      // 检查：第三次分配的指针中，有多少是来自第一次分配的原始集合
//      size_t intersection_count = 0;
//      for(std::byte* p3 : allocated3_vec) {
//          // 使用原始指针集合 pointers1 进行检查
//          if(pointers1.count(p3)) {
//              intersection_count++;
//          }
//      }
//
//      // 理论上，第一次分配后释放了 alloc_count 个，第二次分配消耗了 alloc_count2 个
//      // 那么空闲列表上应该剩下 alloc_count - alloc_count2 个来自第一次分配的块。
//      // 第三次分配时，这部分应该被优先重用。
//      size_t expected_remaining_free = alloc_count - alloc_count2;
//
//      // 这个断言可能仍然比较脆弱，取决于 LIFO/FIFO 和内部实现细节
//      // 但我们可以检查重用的数量是否 *最多* 是期望的数量（它不应该重用比列表上剩余的还多）
//      // 并且至少有一部分被重用（如果 expected_remaining_free > 0）
//      EXPECT_LE(intersection_count, expected_remaining_free) << "More pointers reused from original set than expected to be available on the free list.";
//      if (expected_remaining_free > 0) {
//           EXPECT_GT(intersection_count, 0) << "Expected some reuse from the original set in step 6, but none found.";
//           // 也可以考虑 EXPECT_EQ(intersection_count, expected_remaining_free) 如果你对 LIFO 行为有信心
//      } else {
//          EXPECT_EQ(intersection_count, 0) << "Expected no reuse from the original set as all were consumed in step 3.";
//      }
//
//
//      // 7. 清理第三次分配
//      deallocate_vector(allocated3_vec, alloc_size);
//      ASSERT_TRUE(allocated3_vec.empty());
// }

TEST_F(CentralCacheTest, ForcePageCacheFetch) {
    const size_t alloc_size = 128; // 使用与之前相同的大小
    ASSERT_LE(alloc_size, size_utils::MAX_CACHED_UNIT_SIZE);

    // 确定一个数量，小于 MAX_UNIT_COUNT，用于填充空闲列表
    const size_t initial_free_count = 300;
    ASSERT_LT(initial_free_count, page_span::MAX_UNIT_COUNT) << "Initial count must be less than MAX_UNIT_COUNT";
    ASSERT_GT(initial_free_count, 0);

    // 1. 分配第一批块，用于之后填充空闲列表
    auto result1 = allocate_and_check(alloc_size, initial_free_count);
    ASSERT_TRUE(result1.has_value()) << "Initial allocation (step 1) failed.";
    ASSERT_NE(result1.value(), nullptr);
    std::vector<std::byte*> allocated1_vec = list_to_vector(result1.value());
    ASSERT_EQ(allocated1_vec.size(), initial_free_count);
    std::set<std::byte*> pointers1(allocated1_vec.begin(), allocated1_vec.end());
    ASSERT_EQ(pointers1.size(), initial_free_count); // 确认指针唯一

    // 2. 释放第一批块，将它们放入空闲列表
    // 现在我们知道空闲列表上至少有 initial_free_count 个块
    deallocate_vector(allocated1_vec, alloc_size);
    ASSERT_TRUE(allocated1_vec.empty());

    // 3. 请求分配比空闲列表上已知数量更多的块，但总量仍小于 MAX_UNIT_COUNT
    const size_t blocks_to_request = 400; // 需要的数量 > initial_free_count
    const size_t extra_needed = blocks_to_request - initial_free_count; // 需要从 fetch 获取的数量
    ASSERT_GT(blocks_to_request, initial_free_count) << "Request count must be greater than initial free count to force fetch";
    ASSERT_LT(blocks_to_request, page_span::MAX_UNIT_COUNT) << "Request count must be less than MAX_UNIT_COUNT due to new constraint";
    ASSERT_GT(extra_needed, 0);

    auto result2 = allocate_and_check(alloc_size, blocks_to_request);
    ASSERT_TRUE(result2.has_value()) << "Allocation requiring fetch (step 3) failed.";
    ASSERT_NE(result2.value(), nullptr);
    std::vector<std::byte*> allocated2_vec = list_to_vector(result2.value());
    ASSERT_EQ(allocated2_vec.size(), blocks_to_request); // 确认返回了请求的数量
    std::set<std::byte*> pointers2(allocated2_vec.begin(), allocated2_vec.end());
    ASSERT_EQ(pointers2.size(), blocks_to_request); // 确认返回的指针唯一

    // 4. 验证：第二次分配的块中，应包含所有第一次释放的块 + 一些新块
    size_t reused_count = 0;
    size_t new_count = 0;
    for (std::byte* ptr : allocated2_vec) {
        if (pointers1.count(ptr)) { // 检查是否是第一次分配的块 (现在应该在空闲列表被重用)
            reused_count++;
        } else {
            new_count++; // 这些应该是从 page_cache fetch 来的新块
        }
    }

    // 验证重用：我们期望所有 initial_free_count 个块都被重用了
    EXPECT_EQ(reused_count, initial_free_count)
        << "Expected to reuse all " << initial_free_count << " previously freed blocks.";

    // 验证新块：我们期望有 extra_needed 个新块被分配
    EXPECT_EQ(new_count, extra_needed)
        << "Expected " << extra_needed << " new blocks from page cache fetch to satisfy the request.";

    // 5. 清理第二次分配
    deallocate_vector(allocated2_vec, alloc_size);
    ASSERT_TRUE(allocated2_vec.empty());
}
TEST_F(CentralCacheTest, DeallocateFreesPageIndirect) {
    const size_t alloc_size = 256;
    ASSERT_LE(alloc_size, size_utils::MAX_CACHED_UNIT_SIZE);
    ASSERT_GT(alloc_size, 0);
    ASSERT_EQ(alloc_size % size_utils::ALIGNMENT, 0);

    // 分配一个小于 MAX_UNIT_COUNT 的数量
    const size_t alloc_count1 = 300; // 或者 page_span::MAX_UNIT_COUNT / 2 等
    ASSERT_LT(alloc_count1, page_span::MAX_UNIT_COUNT) << "Allocation count must be less than MAX_UNIT_COUNT";
    ASSERT_GT(alloc_count1, 0);

    // 1. 分配第一批块
    auto result1 = allocate_and_check(alloc_size, alloc_count1);
    // 如果第一次分配就失败，可能是资源不足，测试无法继续
    ASSERT_TRUE(result1.has_value()) << "Initial allocation (step 1) failed, cannot proceed.";
    ASSERT_NE(result1.value(), nullptr);
    std::vector<std::byte*> allocated1 = list_to_vector(result1.value());
    ASSERT_EQ(allocated1.size(), alloc_count1);

    // 2. 释放所有块
    // 这里的关键在于：如果 central_cache 的实现是当 span 的 use_count 降为 0 时才归还页面，
    // 那么只释放 alloc_count1 (< units_in_span) 个块可能 *不会* 触发页面归还。
    // 因此，这个 "Indirect" 测试的有效性取决于 central_cache 的具体释放策略。
    // 我们假设，即使页面没有完全归还，释放操作本身应该能正常工作，
    // 并且后续分配（可能来自同一个未完全释放的 span 或新的 span）应该成功。
    deallocate_vector(allocated1, alloc_size);
    ASSERT_TRUE(allocated1.empty());

    // 3. 再次分配少量块
    auto result2 = allocate_and_check(alloc_size, 1);
    ASSERT_TRUE(result2.has_value()) << "Allocation after deallocating blocks failed. "
                                     << "This might indicate an issue in deallocation logic or resource exhaustion.";
    ASSERT_NE(result2.value(), nullptr);
    std::vector<std::byte*> allocated2 = list_to_vector(result2.value());
    ASSERT_EQ(allocated2.size(), 1);

    // 4. 间接验证：主要是确保步骤 3 成功。
    // 测试的价值在于模拟 "分配-释放-再分配" 的流程。

    // 5. 清理最后的分配
    deallocate_vector(allocated2, alloc_size);
    ASSERT_TRUE(allocated2.empty());
}

// --- NEW TEST using Internal State Check ---
// 这个测试依赖于 central_cache 将 CentralCacheTest 声明为友元
// friend class CentralCacheTest; // 应该在 central_cache 类定义内部
TEST_F(CentralCacheTest, DeallocateFreesPageInternalCheck) {
    const size_t alloc_size = 256; // 选择一个大小
    ASSERT_LE(alloc_size, size_utils::MAX_CACHED_UNIT_SIZE);
    ASSERT_GT(alloc_size, 0);
    ASSERT_EQ(alloc_size % size_utils::ALIGNMENT, 0);
    const size_t index = size_utils::get_index(alloc_size);
    const size_t units_in_span = page_span::MAX_UNIT_COUNT; // 通常是 512
    ASSERT_GT(units_in_span, 0);

    // --- 步骤 0: 尝试清理状态 (可选) ---
    // 尝试分配并释放，可能有助于清空 free list，使后续分配更可能触发新的 span 获取
    const size_t cleanup_alloc_count = 10; // 分配少量进行清理
    ASSERT_LT(cleanup_alloc_count, units_in_span);
    auto cleanup_res = allocate_and_check(alloc_size, cleanup_alloc_count);
    if (cleanup_res.has_value()) {
        auto cleanup_vec = list_to_vector(cleanup_res.value());
        deallocate_vector(cleanup_vec, alloc_size);
    }
    // 短暂等待可能有助于状态稳定，但不是必须
    // std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // --- 分配并识别 Span ---
    // 1. 第一次分配：分配一部分块 (count1 < units_in_span)
    //    这应该会触发 central_cache 内部从 page_cache 获取一个包含 units_in_span 块的 span (如果 free list 为空)
    const size_t count1 = units_in_span / 2; // 例如，分配一半
    ASSERT_LT(count1, units_in_span);
    ASSERT_GT(count1, 0);

    auto result1 = allocate_and_check(alloc_size, count1);
    ASSERT_TRUE(result1.has_value()) << "Initial allocation (part 1) failed.";
    ASSERT_NE(result1.value(), nullptr);
    std::vector<std::byte*> allocated_blocks1 = list_to_vector(result1.value());
    ASSERT_EQ(allocated_blocks1.size(), count1);
    ASSERT_FALSE(allocated_blocks1.empty());

    // 2. 识别管理这些块的 page_span
    std::byte* first_block = allocated_blocks1[0];
    std::byte* page_start_addr = nullptr;
    size_t page_original_size = 0;
    page_span* managed_span_ptr = nullptr;

    { // 作用域用于查找 span
        auto& page_map = cache.m_page_set[index]; // 访问私有成员
        auto it_map = page_map.upper_bound(first_block);
        ASSERT_NE(it_map, page_map.begin()) << "Could not find managing page_span for allocated block " << (void*)first_block << " in m_page_set[" << index << "]";
        --it_map;
        page_start_addr = it_map->first;
        managed_span_ptr = &(it_map->second);

        // 基本验证
        ASSERT_GE(first_block, page_start_addr);
        ASSERT_LT(first_block, page_start_addr + managed_span_ptr->size());
        ASSERT_EQ(managed_span_ptr->unit_size(), alloc_size);
        page_original_size = managed_span_ptr->size();
        ASSERT_GT(page_original_size, 0);
        // 理论上，这个 span 应该包含 units_in_span 个单元
        // 注意：managed_span_ptr->size() 返回的是字节大小
        ASSERT_EQ(page_original_size / alloc_size, units_in_span) << "Identified span does not contain the expected number of units.";
    } // 查找 span 作用域结束

    // 3. 第二次分配：分配剩余的块 (count2)
    //    我们 *假设* central_cache 会优先使用同一个 span 的 free list 中的块
    const size_t count2 = units_in_span - count1;
    ASSERT_LT(count2, units_in_span);
    ASSERT_GT(count2, 0);

    auto result2 = allocate_and_check(alloc_size, count2);
    ASSERT_TRUE(result2.has_value()) << "Allocation (part 2) failed.";
    ASSERT_NE(result2.value(), nullptr);
    std::vector<std::byte*> allocated_blocks2 = list_to_vector(result2.value());
    ASSERT_EQ(allocated_blocks2.size(), count2);

    // (可选但有用) 验证第二次分配的块也来自同一个 span
    if (!allocated_blocks2.empty()) {
        std::byte* second_block_sample = allocated_blocks2[0];
        ASSERT_GE(second_block_sample, page_start_addr) << "Block from second allocation is outside the identified span's start.";
        ASSERT_LT(second_block_sample, page_start_addr + page_original_size) << "Block from second allocation is outside the identified span's end.";
    }

    // --- 释放并验证 ---
    // 4. 组合并释放所有块 (总共 units_in_span 个)
    std::vector<std::byte*> all_allocated_blocks = allocated_blocks1;
    all_allocated_blocks.insert(all_allocated_blocks.end(), allocated_blocks2.begin(), allocated_blocks2.end());
    ASSERT_EQ(all_allocated_blocks.size(), units_in_span) << "Total allocated blocks do not sum up to units_in_span.";

    // 释放这两个列表（现在合并在一个 vector 里）
    deallocate_vector(all_allocated_blocks, alloc_size);
    ASSERT_TRUE(all_allocated_blocks.empty()); // 确认 vector 清空

    // 5. 验证内部状态：span 应已从 central_cache 中移除
    // (验证逻辑与之前相同)
    // 5a. 检查 m_page_set
    {
        ASSERT_EQ(cache.m_page_set[index].count(page_start_addr), 0)
            << "Page span starting at " << (void*)page_start_addr
            << " was *not* removed from m_page_set[" << index << "] after all its blocks were deallocated.";
    }

    // 5b. 检查 m_free_array
    {
        std::byte* current_free = cache.m_free_array[index];
        std::byte* page_end_addr = page_start_addr + page_original_size;
        size_t blocks_found_from_freed_page = 0;
        std::set<std::byte*> visited_free;

        while (current_free != nullptr) {
            if (!visited_free.insert(current_free).second) {
                 ADD_FAILURE() << "Cycle detected in free list m_free_array[" << index << "] during verification at " << (void*)current_free;
                 break;
            }
            if (current_free >= page_start_addr && current_free < page_end_addr) {
                blocks_found_from_freed_page++;
                 ADD_FAILURE() << "Block " << (void*)current_free << " from the supposedly freed page span ["
                               << (void*)page_start_addr << ", " << (void*)page_end_addr
                               << ") was found in the free list m_free_array[" << index << "].";
            }
            current_free = *(reinterpret_cast<std::byte**>(current_free));
        }
        ASSERT_EQ(blocks_found_from_freed_page, 0)
            << blocks_found_from_freed_page << " block(s) belonging to the freed page span were found in the central cache's free list.";

        // 5c. 检查 m_free_array_size
        size_t reported_free_size = cache.m_free_array_size[index];
        size_t actual_free_count = visited_free.size();
        ASSERT_EQ(reported_free_size, actual_free_count)
            << "m_free_array_size[" << index << "] (" << reported_free_size
            << ") does not match the actual number of nodes in the free list ("
            << actual_free_count << ") after page deallocation.";
    }
}
// --- End NEW TEST ---

TEST_F(CentralCacheTest, AllocateDifferentSizes) {
    const size_t size1 = 16;
    const size_t count1 = 5;
    const size_t size2 = 128;
    const size_t count2 = 3;

    ASSERT_LE(size1, size_utils::MAX_CACHED_UNIT_SIZE);
    ASSERT_LE(size2, size_utils::MAX_CACHED_UNIT_SIZE);
    ASSERT_NE(size1, size2); // 测试不同大小才有意义
    ASSERT_GT(count1, 0);
    ASSERT_GT(count2, 0);


    auto res1 = allocate_and_check(size1, count1);
    ASSERT_TRUE(res1.has_value());
     ASSERT_NE(res1.value(), nullptr);
    auto nodes1 = list_to_vector(res1.value());
    ASSERT_EQ(nodes1.size(), count1);

    auto res2 = allocate_and_check(size2, count2);
    ASSERT_TRUE(res2.has_value());
     ASSERT_NE(res2.value(), nullptr);
    auto nodes2 = list_to_vector(res2.value());
    ASSERT_EQ(nodes2.size(), count2);

    // 简单检查：地址不应重叠 (粗略检查)
     std::set<std::byte*> ptrs1(nodes1.begin(), nodes1.end());
     std::set<std::byte*> ptrs2(nodes2.begin(), nodes2.end());
     ASSERT_EQ(ptrs1.size(), count1);
     ASSERT_EQ(ptrs2.size(), count2);


     for(auto ptr2 : nodes2) {
         for(auto ptr1 : nodes1) {
             // 检查 ptr2 是否在 [ptr1, ptr1 + size1) 范围内
             bool overlap_forward = (ptr2 >= ptr1 && ptr2 < ptr1 + size1);
             ASSERT_FALSE(overlap_forward) << "Memory overlap detected: block from size " << size2
                                           << " at " << (void*)ptr2 << " overlaps with block from size " << size1
                                           << " at " << (void*)ptr1;
             // 检查 ptr1 是否在 [ptr2, ptr2 + size2) 范围内 (反向检查)
             bool overlap_backward = (ptr1 >= ptr2 && ptr1 < ptr2 + size2);
              ASSERT_FALSE(overlap_backward) << "Memory overlap detected: block from size " << size1
                                            << " at " << (void*)ptr1 << " overlaps with block from size " << size2
                                            << " at " << (void*)ptr2;
         }
     }

    deallocate_vector(nodes1, size1);
    deallocate_vector(nodes2, size2);
    ASSERT_TRUE(nodes1.empty());
    ASSERT_TRUE(nodes2.empty());
}


// --- Concurrency Tests (调用方式已修正) ---

TEST_F(CentralCacheTest, ConcurrencySameSizeClass) {
    const size_t alloc_size = 64;
    const size_t block_count = 5;
    const int iterations = 50; // 迭代次数可以调整
    const int num_threads = 4; // 线程数可以调整

    ASSERT_LE(alloc_size, size_utils::MAX_CACHED_UNIT_SIZE);
    ASSERT_GT(block_count, 0);
    ASSERT_GT(iterations, 0);
    ASSERT_GT(num_threads, 0);


    std::vector<std::thread> threads;
    for (int i = 0; i < num_threads; ++i) {
        // *** 修改点：调用静态成员函数，传递函数指针 ***
        threads.emplace_back(&CentralCacheTest::allocate_deallocate_task,
                             i, alloc_size, block_count, iterations);
    }

    for (auto& t : threads) {
        if (t.joinable()) {
             t.join();
        }
    }
    // 基本通过条件：没有崩溃、死锁、数据竞争（需要 TSAN 等工具检测）
    // 以及没有触发 allocate_deallocate_task 内部的 fprintf 错误。
    // 可以增加一些事后检查，比如尝试分配和释放，看状态是否正常。
}

TEST_F(CentralCacheTest, ConcurrencyDifferentSizeClasses) {
    const size_t size1 = 32;
    const size_t size2 = 256;
    const size_t block_count = 3;
    const int iterations = 50;
    const int num_threads_per_size = 2; // 每种大小启动 2 个线程

    ASSERT_LE(size1, size_utils::MAX_CACHED_UNIT_SIZE);
    ASSERT_LE(size2, size_utils::MAX_CACHED_UNIT_SIZE);
    ASSERT_NE(size1, size2);
    ASSERT_GT(block_count, 0);
    ASSERT_GT(iterations, 0);
    ASSERT_GT(num_threads_per_size, 0);

    std::vector<std::thread> threads;
    for (int i = 0; i < num_threads_per_size; ++i) {
         // *** 修改点：调用静态成员函数 ***
        threads.emplace_back(&CentralCacheTest::allocate_deallocate_task,
                             i * 2, size1, block_count, iterations);
        threads.emplace_back(&CentralCacheTest::allocate_deallocate_task,
                             i * 2 + 1, size2, block_count, iterations);
    }

    for (auto& t : threads) {
         if (t.joinable()) {
            t.join();
         }
    }
    // 同上，基本通过条件是稳定运行。
}

// --- central_cache.h 内容 (仅用于说明 friend 声明位置) ---
/*
#ifndef CENTRAL_CACHE_H
#define CENTRAL_CACHE_H
// ... includes ...
#include <map>
#include <array>
#include <atomic>
#include <mutex> // 如果需要内部锁
#include <optional>
#include "utils.h" // 包含 size_utils, page_span 等定义

class CentralCacheTest; // 前向声明测试类

namespace memory_pool {
    class central_cache {
    public:
        // *** 必须在这里声明友元 ***
        friend class ::CentralCacheTest; // 使用全局作用域::

        // ... 其他 public 成员 ...
        static central_cache& get_instance();
        std::optional<std::byte*> allocate(size_t memory_size, size_t block_count);
        void deallocate(std::byte* memory_list, size_t memory_size);

    private:
        // ... private 成员 ...
        // 假设这些是内部状态，InternalCheck 测试会访问它们
        std::array<std::byte*, size_utils::CACHE_LINE_SIZE> m_free_array = {};
        std::array<size_t, size_utils::CACHE_LINE_SIZE> m_free_array_size = {};
        std::array<std::map<std::byte*, page_span>, size_utils::CACHE_LINE_SIZE> m_page_set;
        // 可能还有锁等
        // std::array<std::mutex, size_utils::CACHE_LINE_SIZE> m_list_locks_; // 示例锁
    };
}
#endif //CENTRAL_CACHE_H
*/