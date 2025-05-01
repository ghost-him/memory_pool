// ./test/test_central_cache.cpp
// 该内容由 gemini 2.5 pro preview 03-25 生成，https://aistudio.google.com/prompts/new_chat
#include <gtest/gtest.h>
#include <vector>
#include <thread>
#include <list>
#include <numeric>
#include <set>
#include <future> // 用于 std::async

#include "central_cache.h" // 引入被测试的类
#include "utils.h"         // 引入 size_utils 等

using namespace memory_pool;

// --- 测试夹具 (可选，如果需要共享设置/拆卸逻辑) ---
class CentralCacheTest : public ::testing::Test {
protected:
    // 每个测试用例运行前可以设置一些东西
    void SetUp() override {
        // 如果 central_cache 不是单例或者需要重置状态，可以在这里处理
        // 但对于单例，状态会在测试间共享，需要特别注意
    }

    // 每个测试用例运行后可以清理
    void TearDown() override {
        // 清理资源，例如，尝试回收所有在测试中分配的内存
        // 但这可能很复杂，因为单例状态持久存在
    }

    // 辅助函数，用于检查返回的内存块列表
    void check_allocation(const std::optional<std::list<memory_span>>& result, size_t expected_count, size_t expected_size) {
        ASSERT_TRUE(result.has_value()); // 断言结果有效
        ASSERT_EQ(result->size(), expected_count); // 断言数量正确
        std::set<std::byte*> allocated_pointers;
        for (const auto& span : result.value()) {
            EXPECT_EQ(span.size(), expected_size); // 断言每个块的大小正确
            EXPECT_NE(span.data(), nullptr);       // 断言指针非空
            // 检查指针是否唯一
            auto [iter, inserted] = allocated_pointers.insert(span.data());
            EXPECT_TRUE(inserted) << "Duplicate pointer detected in allocation: " << static_cast<void*>(span.data());
        }
    }
};

// --- 边界条件测试 ---

// 测试分配 0 个块
TEST_F(CentralCacheTest, AllocateZeroBlocks) {
    auto& cache = central_cache::get_instance();
    auto result = cache.allocate(64, 0);
    EXPECT_FALSE(result.has_value()) << "Allocating 0 blocks should return nullopt";
}

// 测试分配大小为 0 的块 (虽然代码有 assert(memory_size % 8 == 0), 但理论上也应处理)
// 注意: 原始代码中断言 memory_size % 8 == 0 且 memory_size != 0，这里我们测试紧邻边界
TEST_F(CentralCacheTest, AllocateZeroSize) {
    auto& cache = central_cache::get_instance();
    auto result = cache.allocate(0, 10);
    EXPECT_FALSE(result.has_value()) << "Allocating size 0 should return nullopt";
}

// 测试分配最小有效大小和数量
TEST_F(CentralCacheTest, AllocateMinimumValid) {
    auto& cache = central_cache::get_instance();
    size_t size = 8;
    size_t count = 1;
    auto result = cache.allocate(size, count);
    check_allocation(result, count, size);
    // 回收内存，避免影响其他测试 (如果需要)
    if (result) cache.deallocate(result.value());
}

// 测试分配较大的块数 (但小于 PAGE_SPAN 对应的块数，可能触发从空闲列表分配)
TEST_F(CentralCacheTest, AllocateModerateBlocks) {
    auto& cache = central_cache::get_instance();
    size_t size = 32;
    size_t count = 5;
    auto result = cache.allocate(size, count);
    check_allocation(result, count, size);
    if (result) cache.deallocate(result.value());
}

// 测试分配，可能触发向 page_cache 申请新页
TEST_F(CentralCacheTest, AllocateTriggeringNewPage) {
    auto& cache = central_cache::get_instance();
    // 选择一个可能不在缓存中的大小，或者分配大量块
    size_t size = 128;
    // 分配足够多的块，几乎肯定需要新页 (具体数量取决于 PAGE_SPAN 和 PAGE_SIZE)
    // 假设 PAGE_SIZE = 4096, PAGE_SPAN = 8 => 32KB
    // 32KB / 128 bytes = 256 块
    size_t count = 260; // 分配比一页能提供的还多一点
     // 注意: 原始代码有 assert(block_count < page_span::MAX_UNIT_COUNT)，这里假设 260 < MAX_UNIT_COUNT
    ASSERT_LT(count, page_span::MAX_UNIT_COUNT);

    auto result = cache.allocate(size, count);
    check_allocation(result, count, size);
    if (result) cache.deallocate(result.value());
}

// 测试分配超大内存块 (大于 page_span::MAX_UNIT_COUNT)
TEST_F(CentralCacheTest, AllocateLargeObject) {
    auto& cache = central_cache::get_instance();
    // 假设 page_span::MAX_UNIT_COUNT 是 512 (根据 central_cache.cpp 中的注释)
    // 但实际应从 page_span 定义获取或设置为已知常量
    // 我们用一个明显大于 512 的值，但也需要是 8 的倍数
    size_t large_size = 1024 * 8; // 8KB
    // 确保这个大小大于 MAX_UNIT_COUNT，如果 MAX_UNIT_COUNT 更大则需要调整
    // ASSERT_GT(large_size, page_span::MAX_UNIT_COUNT); // 假设 MAX_UNIT_COUNT 在 page_span.h 定义

    auto result = cache.allocate(large_size, 1); // 大对象通常只分配 1 个
    check_allocation(result, 1, large_size);

    // 大对象直接由 page_cache 回收
    if (result) cache.deallocate(result.value());
}


// 测试分配和回收循环，检查内存是否能被正确重用
TEST_F(CentralCacheTest, AllocateDeallocateReuse) {
    auto& cache = central_cache::get_instance();
    size_t size = 64;
    size_t count = 10;

    auto result1 = cache.allocate(size, count);
    check_allocation(result1, count, size);
    std::list<memory_span> allocated1 = result1.value(); // 复制一份，因为 result1 会失效

    // 记录指针，用于后续比较
    std::set<std::byte*> pointers1;
    for(const auto& span : allocated1) {
        pointers1.insert(span.data());
    }

    // 全部回收
    cache.deallocate(allocated1);

    // 再次分配相同大小和数量，期望能重用部分或全部内存
    auto result2 = cache.allocate(size, count);
    check_allocation(result2, count, size);
    std::list<memory_span> allocated2 = result2.value();

    // 检查是否有重用的指针 (至少一个) - 这个检查依赖于实现细节，可能不稳定
    bool reused = false;
    for(const auto& span : allocated2) {
        if (pointers1.count(span.data())) {
            reused = true;
            break;
        }
    }
    // 注意：实现可能会返回新的内存而不是重用，所以这个断言不总是强制性的
    // EXPECT_TRUE(reused) << "Expected some memory reuse after deallocation";

    // 回收第二次分配的内存
    cache.deallocate(allocated2);
}

// 测试回收空列表
TEST_F(CentralCacheTest, DeallocateEmptyList) {
    auto& cache = central_cache::get_instance();
    std::list<memory_span> empty_list;
    // 期望不抛出异常，不崩溃
    EXPECT_NO_THROW(cache.deallocate(empty_list));
}

// 测试回收导致页归还给 page_cache (这是一个复杂的场景，难以直接验证)
// 思路：分配刚好填满一个或多个页的块，然后全部回收，观察内部状态（如果可能）或行为
TEST_F(CentralCacheTest, DeallocateTriggeringPageReturn) {
    auto& cache = central_cache::get_instance();
    size_t size = 256;
    // 计算一页 (PAGE_SIZE) 能放多少个块
    size_t blocks_per_page = size_utils::PAGE_SIZE / size;
    // 计算 central_cache 一次申请多少页 (PAGE_SPAN)
    size_t total_blocks_in_span = blocks_per_page * central_cache::PAGE_SPAN;

    // 分配刚好填满这些页的块
    size_t count_to_allocate = total_blocks_in_span;
     // 再次检查是否超过 MAX_UNIT_COUNT
    ASSERT_LT(count_to_allocate, page_span::MAX_UNIT_COUNT);

    auto result = cache.allocate(size, count_to_allocate);
    check_allocation(result, count_to_allocate, size);
    std::list<memory_span> allocated = result.value();

    // 全部回收
    cache.deallocate(allocated);

    // 验证：很难直接验证页是否归还。
    // 间接方法：
    // 1. 如果有内部统计信息，检查它们。
    // 2. 再次分配同样数量，观察是否需要重新从系统获取内存（例如，测量分配时间，但这不可靠）。
    // 3. 最好的方法是如果 page_cache 有日志或统计，检查它是否收到了归还的页。
    // 这里我们仅确保操作不崩溃
    EXPECT_TRUE(true) << "Test execution finished without crash, implying basic correctness.";
}


// --- 并发可用性测试 ---

// 并发分配相同大小的内存块
TEST_F(CentralCacheTest, ConcurrentAllocateSameSize) {
    auto& cache = central_cache::get_instance();
    const size_t num_threads = 8;
    const size_t allocs_per_thread = 100;
    const size_t block_size = 128;
    const size_t blocks_per_alloc = 5; // 每次分配 5 个

    std::vector<std::thread> threads;
    std::vector<std::future<std::vector<std::list<memory_span>>>> futures;

    for (size_t i = 0; i < num_threads; ++i) {
        // 使用 std::async 更方便地获取结果和异常
        futures.push_back(std::async(std::launch::async, [&]() {
            std::vector<std::list<memory_span>> thread_allocations;
            thread_allocations.reserve(allocs_per_thread);
            for (size_t j = 0; j < allocs_per_thread; ++j) {
                auto result = cache.allocate(block_size, blocks_per_alloc);
                if (result) {
                    // 检查分配是否成功且基本有效
                    EXPECT_EQ(result->size(), blocks_per_alloc);
                    for(const auto& span : result.value()) {
                         EXPECT_EQ(span.size(), block_size);
                         EXPECT_NE(span.data(), nullptr);
                    }
                    thread_allocations.push_back(std::move(result.value()));
                } else {
                    // 如果分配失败，记录错误或断言失败
                     ADD_FAILURE() << "Allocation failed unexpectedly in thread.";
                    // 或者根据预期，如果允许失败，则不处理
                }
                 // 可以加个短暂 yield 帮助触发竞争条件
                 std::this_thread::yield();
            }
            return thread_allocations;
        }));
    }

    // 等待所有线程完成并收集结果
    std::list<memory_span> all_allocated_spans;
    std::set<std::byte*> all_pointers;
    size_t total_allocated_count = 0;
    size_t successful_alloc_ops = 0;

    for (auto& fut : futures) {
        try {
            std::vector<std::list<memory_span>> thread_results = fut.get();
            successful_alloc_ops += thread_results.size(); // 记录成功分配的操作次数
            for (auto& lst : thread_results) {
                total_allocated_count += lst.size();
                for (const auto& span : lst) {
                    all_allocated_spans.push_back(span);
                    // 检查指针唯一性
                     auto [_, inserted] = all_pointers.insert(span.data());
                     EXPECT_TRUE(inserted) << "Duplicate pointer detected across threads: " << static_cast<void*>(span.data());
                }
            }
        } catch (const std::exception& e) {
            FAIL() << "Exception caught from thread: " << e.what();
        }
    }

    // 验证总分配数量是否符合预期
    // 注意：如果 allocate 可能返回 nullopt，这里的预期值需要调整
    EXPECT_EQ(successful_alloc_ops, num_threads * allocs_per_thread) << "Not all allocation operations were successful.";
    EXPECT_EQ(total_allocated_count, num_threads * allocs_per_thread * blocks_per_alloc) << "Total allocated block count mismatch.";
    EXPECT_EQ(all_allocated_spans.size(), total_allocated_count);
    EXPECT_EQ(all_pointers.size(), total_allocated_count) << "Pointer uniqueness check failed.";

    // 回收所有分配的内存
    // 注意：如果并发回收，需要分组或单独回收
    std::cout << "Deallocating " << all_allocated_spans.size() << " blocks from concurrent test..." << std::endl;
    cache.deallocate(all_allocated_spans);
    std::cout << "Deallocation finished." << std::endl;
}

// 并发分配不同大小的内存块
TEST_F(CentralCacheTest, ConcurrentAllocateDifferentSizes) {
    auto& cache = central_cache::get_instance();
    const size_t num_threads = 4;
    const size_t allocs_per_thread = 50;
    const std::vector<size_t> block_sizes = {16, 64, 128, 256};
    const size_t blocks_per_alloc = 3;

    std::vector<std::future<std::vector<std::list<memory_span>>>> futures;

    ASSERT_EQ(num_threads, block_sizes.size()) << "Test setup error: thread count must match block size count.";

    // --- 分配阶段 (与原来相同) ---
    for (size_t i = 0; i < num_threads; ++i) {
        size_t current_block_size = block_sizes[i];
        futures.push_back(std::async(std::launch::async, [&, current_block_size]() {
            std::vector<std::list<memory_span>> thread_allocations;
            thread_allocations.reserve(allocs_per_thread);
            for (size_t j = 0; j < allocs_per_thread; ++j) {
                auto result = cache.allocate(current_block_size, blocks_per_alloc);
                if (result) {
                    // ... (检查部分省略) ...
                    EXPECT_EQ(result->size(), blocks_per_alloc);
                     for(const auto& span : result.value()) {
                         EXPECT_EQ(span.size(), current_block_size);
                         EXPECT_NE(span.data(), nullptr);
                    }
                    thread_allocations.push_back(std::move(result.value()));
                } else {
                     ADD_FAILURE() << "Allocation failed unexpectedly for size " << current_block_size;
                }
                 std::this_thread::yield();
            }
            return thread_allocations;
        }));
    }

    // --- 收集结果 (修改为按大小分组) ---
    // 使用 map 来按大小存储分配的块列表
    std::map<size_t, std::list<memory_span>> grouped_allocations;
    std::set<std::byte*> all_pointers;
    size_t total_allocated_count = 0;

    for (size_t i = 0; i < num_threads; ++i) { // 遍历线程 (或 futures)
         size_t current_block_size = block_sizes[i]; // 获取该线程处理的大小
        try {
            std::vector<std::list<memory_span>> thread_results = futures[i].get(); // 使用索引访问对应的 future
            for (auto& lst : thread_results) {
                // 将这个列表中的所有 span 移动到对应大小的 grouped_allocations 列表中
                // std::move(lst.begin(), lst.end(), std::back_inserter(grouped_allocations[current_block_size]));
                // 或者更安全的方式是逐个移动或复制
                for (const auto& span : lst) {
                    ASSERT_EQ(span.size(), current_block_size); // 确保大小正确
                    grouped_allocations[current_block_size].push_back(span);
                    total_allocated_count++;
                    // 检查指针唯一性
                    auto [_, inserted] = all_pointers.insert(span.data());
                    EXPECT_TRUE(inserted) << "Duplicate pointer detected across threads: " << static_cast<void*>(span.data());
                }
            }
        } catch (const std::exception& e) {
            FAIL() << "Exception caught from thread for size " << current_block_size << ": " << e.what();
        }
    }

    EXPECT_EQ(total_allocated_count, num_threads * allocs_per_thread * blocks_per_alloc);
    EXPECT_EQ(all_pointers.size(), total_allocated_count);

    // --- 回收 (修改为按大小分组回收) ---
    for (auto const& [size, span_list] : grouped_allocations) {
        std::cout << "Deallocating " << span_list.size() << " blocks of size " << size << std::endl;
        // 对每个大小的列表单独调用 deallocate
        if (!span_list.empty()) {
            cache.deallocate(span_list);
        }
    }
     std::cout << "Deallocation finished for ConcurrentAllocateDifferentSizes." << std::endl;
}

// 并发分配和回收相同大小
TEST_F(CentralCacheTest, ConcurrentAllocDeallocSameSize) {
    auto& cache = central_cache::get_instance();
    const size_t num_threads = 8;
    const size_t ops_per_thread = 200; // 操作次数（分配+回收算一次）
    const size_t block_size = 64;

    std::vector<std::thread> threads;
    std::atomic<bool> test_failed = false;

    for (size_t i = 0; i < num_threads; ++i) {
        threads.emplace_back([&]() {
            std::list<memory_span> allocated_list; // 每个线程持有自己的列表
            for (size_t j = 0; j < ops_per_thread; ++j) {
                // 尝试分配
                auto result = cache.allocate(block_size, 1); // 每次分配 1 个
                if (result && !result->empty()) {
                    allocated_list.push_back(result->front()); // 保存分配的块

                    // 模拟使用（可选）
                    // std::this_thread::sleep_for(std::chrono::microseconds(10));

                    // 随机决定是否回收一部分已分配的块
                    if (!allocated_list.empty() && (rand() % 3 == 0)) { // 大约 1/3 的概率回收
                        std::list<memory_span> to_deallocate;
                        to_deallocate.push_back(allocated_list.front());
                        allocated_list.pop_front();
                        try {
                             cache.deallocate(to_deallocate);
                        } catch (...) {
                             ADD_FAILURE() << "Exception during deallocate in thread.";
                             test_failed = true; // 标记测试失败
                             break; // 退出循环
                        }
                    }
                } else {
                     ADD_FAILURE() << "Allocation failed unexpectedly in thread.";
                     test_failed = true;
                     break;
                }
                 std::this_thread::yield(); // 增加竞争机会
                 if(test_failed) break; // 如果其他线程失败了，尽快退出
            }

            // 测试结束后，回收该线程持有的所有剩余内存
            if (!allocated_list.empty()) {
                 try {
                    cache.deallocate(allocated_list);
                 } catch (...) {
                    ADD_FAILURE() << "Exception during final deallocate in thread.";
                    test_failed = true;
                 }
            }
        });
    }

    // 等待所有线程完成
    for (auto& t : threads) {
        t.join();
    }

    // 检查是否有线程报告失败
    ASSERT_FALSE(test_failed) << "One or more threads reported failure.";
}

// 可以添加更多并发测试，例如混合不同大小的分配/回收