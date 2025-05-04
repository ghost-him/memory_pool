// 该内容由 gemini 2.5 pro preview 03-25 生成，https://aistudio.google.com/prompts/new_chat
#include <gtest/gtest.h>
#include <vector>
#include <thread>
#include <numeric>
#include <set>
#include <optional>
#include "utils.h"
#include "page_cache.h"

#include <random>

using namespace memory_pool_v2;

// Test Fixture for PageCache tests
class PageCacheTest : public ::testing::Test {
protected:
    // Get the singleton instance for tests
    page_cache& cache = page_cache::get_instance();
    static constexpr size_t PAGE_SIZE = size_utils::PAGE_SIZE;

    // Helper to check span validity
    void check_span(const std::optional<memory_span>& opt_span, size_t expected_page_count) {
        ASSERT_TRUE(opt_span.has_value());
        const auto& span = opt_span.value();
        ASSERT_NE(nullptr, span.data());
        ASSERT_EQ(expected_page_count * PAGE_SIZE, span.size());
        // Optional: Check if memory is zeroed (as promised by system_allocate_memory)
        // This can be slow for large allocations
        // std::vector<std::byte> zeros(span.size());
        // ASSERT_EQ(0, memcmp(span.data(), zeros.data(), span.size()));
    }

     // NOTE: Due to the identified bugs (especially the memory leak and
     // inconsistent map/set updates), some tests might pass trivially without
     // actually verifying the intended logic (like merging or cache reuse).
     // Running these tests with tools like Valgrind is crucial.
     // The primary goal here is to provide a structure and test basic operations.
};

// Test singleton behavior
TEST_F(PageCacheTest, SingletonInstance) {
    page_cache& instance1 = page_cache::get_instance();
    page_cache& instance2 = page_cache::get_instance();
    ASSERT_EQ(&instance1, &instance2);
}

// Test basic allocation of 1 page
TEST_F(PageCacheTest, AllocateSinglePage) {
    ASSERT_NO_THROW({
        auto span_opt = cache.allocate_page(1);
        check_span(span_opt, 1);
        // Deallocate immediately to clean up for other tests
        cache.deallocate_page(span_opt.value());
    });
}

// Test basic allocation of multiple pages
TEST_F(PageCacheTest, AllocateMultiplePages) {
     ASSERT_NO_THROW({
        const size_t num_pages = 5;
        auto span_opt = cache.allocate_page(num_pages);
        check_span(span_opt, num_pages);
        cache.deallocate_page(span_opt.value());
     });
}

// Test allocation when the cache is likely empty (forces system allocation)
// and potentially splitting if num_pages < PAGE_ALLOCATE_COUNT
TEST_F(PageCacheTest, AllocateForcesSystemAllocationAndSplit) {
    ASSERT_NO_THROW({
        // Allocate a number of pages potentially less than PAGE_ALLOCATE_COUNT
        const size_t num_pages = page_cache::PAGE_ALLOCATE_COUNT / 2;
        if (num_pages == 0) return; // Skip if PAGE_ALLOCATE_COUNT is 1

        auto span_opt = cache.allocate_page(num_pages);
         // BUG 3 means this might get the wrong size if not fixed
        check_span(span_opt, num_pages);
        cache.deallocate_page(span_opt.value());
     });

     ASSERT_NO_THROW({
        // Allocate exactly PAGE_ALLOCATE_COUNT
        const size_t num_pages = page_cache::PAGE_ALLOCATE_COUNT;
        auto span_opt = cache.allocate_page(num_pages);
        check_span(span_opt, num_pages);
        cache.deallocate_page(span_opt.value());
     });

     ASSERT_NO_THROW({
        // Allocate more than PAGE_ALLOCATE_COUNT
        const size_t num_pages = page_cache::PAGE_ALLOCATE_COUNT + 3;
        auto span_opt = cache.allocate_page(num_pages);
        check_span(span_opt, num_pages);
        cache.deallocate_page(span_opt.value());
     });
}


// Test allocating, deallocating, and re-allocating the same size
// This *should* hit the cache if bugs 1, 4, 5 were fixed.
TEST_F(PageCacheTest, AllocateDeallocateReallocate) {
    const size_t num_pages = 3;
    std::optional<memory_span> span_opt1, span_opt2;

    ASSERT_NO_THROW({
        span_opt1 = cache.allocate_page(num_pages);
        check_span(span_opt1, num_pages);
    });

    ASSERT_NO_THROW({
        cache.deallocate_page(span_opt1.value());
    });

    // Allocate again - might get the same memory or different, but should succeed
    ASSERT_NO_THROW({
        span_opt2 = cache.allocate_page(num_pages);
        check_span(span_opt2, num_pages);
    });

    // Clean up the second allocation
    ASSERT_NO_THROW({
        cache.deallocate_page(span_opt2.value());
    });
    // Note: Without fixing Bug 1, the second allocation might trigger
    // system allocation instead of using the cache.
}

// Test deallocation sequence designed to trigger forward merge.
// WARNING: Cannot easily verify merge occurred without internal access or fixing bugs.
// We mainly test that the sequence doesn't crash.
TEST_F(PageCacheTest, DeallocationSequenceForwardMerge) {
    const size_t num_pages1 = 2;
    const size_t num_pages2 = 3;
    std::optional<memory_span> span_opt1, span_opt2;

    // Need contiguous allocations for merge test. This is hard to guarantee
    // externally. We allocate a large chunk and split it manually for testing.
    const size_t total_pages = 10; // Ensure enough space potentially
    auto large_span_opt = cache.allocate_page(total_pages);
    ASSERT_TRUE(large_span_opt.has_value()) << "Failed to get large span for merge test setup";
    if (!large_span_opt) return; // Stop test if setup fails

    memory_span large_span = large_span_opt.value();
    std::byte* start_ptr = large_span.data();

    memory_span span1 = large_span.subspan(0, num_pages1 * PAGE_SIZE);
    memory_span span2 = large_span.subspan(num_pages1 * PAGE_SIZE, num_pages2 * PAGE_SIZE);
    memory_span remaining = large_span.subspan((num_pages1 + num_pages2) * PAGE_SIZE);

    // Deallocate the pieces individually in an order that *should* allow forward merge
    ASSERT_NO_THROW(cache.deallocate_page(span1));
    ASSERT_NO_THROW(cache.deallocate_page(span2)); // Deallocating span2 adjacent to free span1

    // Deallocate the rest
    if (remaining.size() > 0) {
        ASSERT_NO_THROW(cache.deallocate_page(remaining));
    }
    // Ideally, after fixing bugs 4 & 6, allocating num_pages1 + num_pages2
    // would reuse the merged block.
}


// Test deallocation sequence designed to trigger backward merge.
// WARNING: Similar verification limitations as the forward merge test.
TEST_F(PageCacheTest, DeallocationSequenceBackwardMerge) {
     const size_t num_pages1 = 2;
     const size_t num_pages2 = 3;
     std::optional<memory_span> span_opt1, span_opt2;

     const size_t total_pages = 10;
     auto large_span_opt = cache.allocate_page(total_pages);
     ASSERT_TRUE(large_span_opt.has_value()) << "Failed to get large span for merge test setup";
     if (!large_span_opt) return;

     memory_span large_span = large_span_opt.value();
     std::byte* start_ptr = large_span.data();

     memory_span span1 = large_span.subspan(0, num_pages1 * PAGE_SIZE);
     memory_span span2 = large_span.subspan(num_pages1 * PAGE_SIZE, num_pages2 * PAGE_SIZE);
     memory_span remaining = large_span.subspan((num_pages1 + num_pages2) * PAGE_SIZE);

     // Deallocate in an order that *should* allow backward merge
     ASSERT_NO_THROW(cache.deallocate_page(span2));
     ASSERT_NO_THROW(cache.deallocate_page(span1)); // Deallocating span1 adjacent to free span2

     if (remaining.size() > 0) {
        ASSERT_NO_THROW(cache.deallocate_page(remaining));
     }
     // Ideally, after fixing bugs 5 & 6, allocating num_pages1 + num_pages2
     // would reuse the merged block.
}

// Test deallocation sequence for merging on both sides.
// WARNING: Similar verification limitations.
TEST_F(PageCacheTest, DeallocationSequenceBothMerge) {
    const size_t num_pages1 = 2;
    const size_t num_pages2 = 3;
    const size_t num_pages3 = 4;

    const size_t total_pages = 15; // Need enough space
    auto large_span_opt = cache.allocate_page(total_pages);
    ASSERT_TRUE(large_span_opt.has_value()) << "Failed to get large span for merge test setup";
     if (!large_span_opt) return;

    memory_span large_span = large_span_opt.value();
    std::byte* start_ptr = large_span.data();

    memory_span span1 = large_span.subspan(0, num_pages1 * PAGE_SIZE);
    memory_span span2 = large_span.subspan(num_pages1 * PAGE_SIZE, num_pages2 * PAGE_SIZE);
    memory_span span3 = large_span.subspan((num_pages1 + num_pages2) * PAGE_SIZE, num_pages3 * PAGE_SIZE);
    memory_span remaining = large_span.subspan((num_pages1 + num_pages2 + num_pages3) * PAGE_SIZE);

     // Deallocate in an order that *should* allow merge on both sides of span2
    ASSERT_NO_THROW(cache.deallocate_page(span1));
    ASSERT_NO_THROW(cache.deallocate_page(span3));
    ASSERT_NO_THROW(cache.deallocate_page(span2)); // Deallocating span2 between free span1 and span3

    if (remaining.size() > 0) {
        ASSERT_NO_THROW(cache.deallocate_page(remaining));
    }
    // Ideally, after fixing bugs 4, 5 & 6, allocating num_pages1 + num_pages2 + num_pages3
    // would reuse the merged block.
}


// Test concurrent allocations and deallocations from multiple threads
TEST_F(PageCacheTest, ConcurrentAccess) {
    const int num_threads = 4;
    const int ops_per_thread = 50;
    const size_t max_pages_per_alloc = 5; // Keep individual allocations small
    std::vector<std::thread> threads;
    std::atomic<int> success_count = 0;
    std::atomic<bool> test_failed = false; // Use atomic flag for failures

    auto thread_func = [&]() {
        // --- Start of Changes for Thread-Safe Randomness ---
        // 每个线程使用自己的随机数生成器和分布对象
        // 使用 random_device 获取一个种子 (可能非确定性)
        std::random_device rd;
        // 使用 Mersenne Twister 引擎，并用 random_device 进行初始化
        std::mt19937 gen(rd());
        // 分布：生成要分配的页数 [1, max_pages_per_alloc]
        std::uniform_int_distribution<size_t> page_dist(1, max_pages_per_alloc);
        // 分布：决定是否释放 [0, 2]，用于模拟 rand() % 3 == 0 (约 1/3 概率)
        std::uniform_int_distribution<> dealloc_chance_dist(0, 2);
        // --- End of Changes for Thread-Safe Randomness ---

        std::vector<memory_span> allocated_spans;
        allocated_spans.reserve(ops_per_thread);
        size_t allocated_count = 0;
        size_t deallocated_count = 0;

        try {
            for (int i = 0; i < ops_per_thread; ++i) {
                // 使用线程安全的分布来生成随机数
                size_t pages_to_alloc = page_dist(gen);
                auto span_opt = cache.allocate_page(pages_to_alloc);

                if (span_opt.has_value()) {
                    // Basic check within thread
                    if (span_opt.value().size() != pages_to_alloc * PAGE_SIZE || span_opt.value().data() == nullptr) {
                         test_failed = true; // Signal failure
                         // Consider logging error here
                         continue; // Try next operation
                    }
                    allocated_spans.push_back(span_opt.value());
                    allocated_count++;
                    success_count++;

                    // Occasionally deallocate a random previous allocation from this thread
                    // 使用线程安全的分布来决定是否释放
                    if (!allocated_spans.empty() && (dealloc_chance_dist(gen) == 0)) {
                        // --- Change for Index Generation ---
                        // 分布：生成要释放的索引 [0, allocated_spans.size() - 1]
                        // 注意：每次需要时创建或更新分布，因为上界会变化
                        std::uniform_int_distribution<int> idx_dist(0, allocated_spans.size() - 1);
                        int idx_to_free = idx_dist(gen);
                        // --- End of Change for Index Generation ---

                        cache.deallocate_page(allocated_spans[idx_to_free]);
                        allocated_spans.erase(allocated_spans.begin() + idx_to_free);
                        deallocated_count++;
                    }
                } else {
                     // Allocation failure might be okay if system is out of memory,
                     // but less likely in a short test. Could indicate an issue.
                     // For now, just log or ignore.
                     // std::cerr << "Thread allocation failed (ok if rare)\n";
                }
            }

            // Deallocate remaining spans allocated by this thread
            for (const auto& span : allocated_spans) {
                cache.deallocate_page(span);
                deallocated_count++;
            }
            // Ensure all allocated spans were intended for deallocation
             if (allocated_count != deallocated_count) {
                 std::cerr << "Thread Mismatch: allocated " << allocated_count << " vs deallocated " << deallocated_count << std::endl;
                 test_failed = true; // Mismatch indicates a problem
             }
        } catch (const std::exception& e) {
            std::cerr << "!!! Exception in thread: " << e.what() << std::endl;
            test_failed = true; // Signal failure
        } catch (...) {
             std::cerr << "!!! Unknown exception in thread." << std::endl;
             test_failed = true; // Signal failure
        }
    };

    // Launch threads
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back(thread_func);
    }

    // Wait for threads to complete
    for (auto& t : threads) {
        t.join();
    }

    // Check the atomic flag
    ASSERT_FALSE(test_failed.load()) << "One or more threads encountered errors or exceptions.";
    // Optional: Check success_count if needed, but ASSERT_FALSE is the main check here.
    // std::cout << "Concurrent test completed " << success_count << " successful allocations." << std::endl;
}

// Note: Testing the stop() method implicitly happens when the singleton
// is destroyed after all tests run. Explicitly calling stop() could interfere
// with other tests if they run after it in the same process.
// The *real* test for stop() releasing memory is using Valgrind.

// Test allocating zero pages (should likely return nullopt or handle gracefully)
TEST_F(PageCacheTest, AllocateZeroPages) {
    // The current implementation might behave unpredictably or allocate
    // based on PAGE_ALLOCATE_COUNT due to std::max.
    // A robust implementation should probably return nullopt or assert/throw.
    // Let's just check it doesn't crash and potentially returns nullopt.
     ASSERT_NO_THROW({
         auto span_opt = cache.allocate_page(0);
         // Depending on implementation details (especially std::max),
         // this might actually allocate PAGE_ALLOCATE_COUNT pages, or fail.
         // If it allocates, we need to deallocate.
         if (span_opt.has_value()) {
             std::cout << "Warning: allocate_page(0) returned a valid span of size "
                       << span_opt.value().size() << ". Deallocating." << std::endl;
             cache.deallocate_page(span_opt.value());
         } else {
              SUCCEED(); // Returning nullopt is reasonable for 0 pages.
         }
         // If the fixed code *always* returns nullopt for 0:
         // ASSERT_FALSE(span_opt.has_value());
     });
}