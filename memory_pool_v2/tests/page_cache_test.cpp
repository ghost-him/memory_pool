#include "gtest/gtest.h"
#include "central_cache.h" // Include the header for central_cache
#include "page_cache.h"   // Needed for context, though assumed correct
#include "utils.h"        // For size_utils etc.

#include <vector>
#include <thread>
#include <set>
#include <numeric>
#include <random>
#include <future> // For std::async

using namespace memory_pool;

// Helper function to get the length of the intrusive linked list
size_t getListLength(std::byte* head) {
    size_t count = 0;
    while (head != nullptr) {
        count++;
        // Move to the next node by reading the pointer stored at the beginning of the current node
        head = *(reinterpret_cast<std::byte**>(head));
    }
    return count;
}

// Helper function to convert list to vector (for easier checking)
std::vector<std::byte*> listToVector(std::byte* head) {
    std::vector<std::byte*> vec;
    while (head != nullptr) {
        vec.push_back(head);
        head = *(reinterpret_cast<std::byte**>(head));
    }
    return vec;
}

// Helper function to build an intrusive list from a vector
std::byte* vectorToList(const std::vector<std::byte*>& vec) {
    std::byte* head = nullptr;
    for (auto it = vec.rbegin(); it != vec.rend(); ++it) {
        std::byte* current = *it;
        *(reinterpret_cast<std::byte**>(current)) = head;
        head = current;
    }
    return head;
}


// Test Fixture (Optional, but good practice if setup/teardown is needed)
class CentralCacheTest : public ::testing::Test {
protected:
    central_cache& cache = central_cache::get_instance();

    // You might add helper methods here if needed
    void TearDown() override {
         // Be cautious cleaning up a singleton's state between tests,
         // as it might affect subsequent tests.
         // For simplicity here, we assume tests are independent enough
         // or that the state doesn't need explicit reset.
         // A proper reset might involve clearing free lists and returning
         // all pages to page_cache, which would require more access/methods.
    }
};

// Test basic allocation and deallocation for a single block
TEST_F(CentralCacheTest, BasicAllocDealloc) {
    const size_t alloc_size = 16; // Must be multiple of 8
    auto ptr_opt = cache.allocate(alloc_size, 1);
    ASSERT_TRUE(ptr_opt.has_value());
    ASSERT_NE(nullptr, *ptr_opt);
    EXPECT_EQ(nullptr, *(reinterpret_cast<std::byte**>(*ptr_opt))) << "Single allocation should have null next ptr";

    // Write to the memory to check writability
    memset(*ptr_opt, 0xAA, alloc_size);

    cache.deallocate(*ptr_opt, alloc_size);
    // Potential further check: try allocating again, maybe get the same pointer?
    // Or check internal free list size (requires access)
}

// Test allocation and deallocation for multiple blocks requested at once
TEST_F(CentralCacheTest, MultiBlockAllocDealloc) {
    const size_t alloc_size = 32;
    const size_t block_count = 5;

    auto head_opt = cache.allocate(alloc_size, block_count);
    ASSERT_TRUE(head_opt.has_value());
    ASSERT_NE(nullptr, *head_opt);

    std::byte* head = *head_opt;
    EXPECT_EQ(block_count, getListLength(head));

    // Check uniqueness and writability
    auto blocks = listToVector(head);
    std::set<std::byte*> block_set(blocks.begin(), blocks.end());
    EXPECT_EQ(block_count, block_set.size()) << "Allocated blocks should be unique";

    for(std::byte* block : blocks) {
        ASSERT_NE(nullptr, block);
        memset(block, 0xBB, alloc_size);
    }

    cache.deallocate(head, alloc_size);
}

// Test allocation triggering page allocation from page_cache
TEST_F(CentralCacheTest, CacheMissTrigger) {
    const size_t alloc_size = 64;
    // Allocate more blocks than likely fit in the initial state, triggering page cache request
    // MAX_UNIT_COUNT is 512 in the code's comments/asserts
    const size_t block_count = page_span::MAX_UNIT_COUNT / 2; // Request a significant number

    auto head1_opt = cache.allocate(alloc_size, block_count);
    ASSERT_TRUE(head1_opt.has_value());
    ASSERT_NE(nullptr, *head1_opt);
    std::byte* head1 = *head1_opt;
    EXPECT_EQ(block_count, getListLength(head1));

    // Allocate more, likely hitting the cache this time
    auto head2_opt = cache.allocate(alloc_size, block_count);
    ASSERT_TRUE(head2_opt.has_value());
    ASSERT_NE(nullptr, *head2_opt);
     std::byte* head2 = *head2_opt;
    EXPECT_EQ(block_count, getListLength(head2));

    // Deallocate all
    cache.deallocate(head1, alloc_size);
    cache.deallocate(head2, alloc_size);

    // Maybe check internal state if possible (e.g., free list size)
     size_t index = size_utils::get_index(alloc_size);
     // Accessing internal state - only possible because members are public
     // EXPECT_GE(cache.m_free_array_size[index], block_count * 2); // Or check if page was returned
}

// Test the logic for returning pages to page_cache
// This test is harder to verify without access to page_cache state,
// but we can check if the central_cache internal state looks correct.
TEST_F(CentralCacheTest, PageReturn) {
    const size_t alloc_size = 128;
    const size_t index = size_utils::get_index(alloc_size);
    // Force allocation of at least one full page span
    const size_t count_to_fill_page = page_span::MAX_UNIT_COUNT;

    // Allocate enough to potentially fill and use one managed page span
    auto head_opt = cache.allocate(alloc_size, count_to_fill_page);
    ASSERT_TRUE(head_opt.has_value());
    std::byte* head = *head_opt;
    ASSERT_NE(nullptr, head);

    auto blocks = listToVector(head);
    EXPECT_EQ(count_to_fill_page, blocks.size());

    // Find the page span this allocation likely created (requires internal access)
    ASSERT_FALSE(cache.m_page_set[index].empty());
    auto it = cache.m_page_set[index].lower_bound(blocks[0]); // Find span containing first block
     if (it == cache.m_page_set[index].end() || it->first > blocks[0]) {
         // If lower_bound points past the block, we need the previous element
         ASSERT_NE(it, cache.m_page_set[index].begin());
         --it;
     }
    ASSERT_TRUE(it != cache.m_page_set[index].end() && blocks[0] >= it->first && blocks[0] < (it->first + it->second.size())) << "Could not find managing page span";
    std::byte* page_start_addr = it->first;
    size_t initial_page_set_count = cache.m_page_set[index].size();


    // Deallocate all blocks from this specific allocation
    cache.deallocate(head, alloc_size);

    // Check if the page span was removed and returned (indirectly check map size)
    // This assumes no other tests interfered with this size class's page spans.
    // And assumes the deallocation logic successfully identified the page as empty.
    size_t final_page_set_count = cache.m_page_set[index].count(page_start_addr);
    EXPECT_EQ(0, final_page_set_count) << "Page span should have been removed after deallocating all its blocks";

     // Also check if the free list associated with this page was cleaned up
     // (Hard to verify precisely without tracking which blocks were added to free list initially)
     // We can check the total free list size as a basic sanity check.
}


// Test allocations larger than MAX_CACHED_UNIT_SIZE
TEST_F(CentralCacheTest, LargeAllocation) {
    const size_t large_size = size_utils::MAX_CACHED_UNIT_SIZE + 8;
    auto ptr_opt = cache.allocate(large_size, 1); // block_count > 1 likely not supported for large allocs
    ASSERT_TRUE(ptr_opt.has_value());
    ASSERT_NE(nullptr, *ptr_opt);
     // For large allocations, the returned pointer points directly to the block, not a list
     // (Based on the code: transform([](memory_span memory) { return memory.data(); }))

    std::byte* ptr = *ptr_opt;
    memset(ptr, 0xCC, large_size); // Check writability

    cache.deallocate(ptr, large_size); // Deallocate requires the original pointer and size
}

// Test edge cases: size 0, count 0
TEST_F(CentralCacheTest, EdgeCaseZero) {
    auto ptr_opt1 = cache.allocate(0, 10);
    EXPECT_FALSE(ptr_opt1.has_value());

    auto ptr_opt2 = cache.allocate(16, 0);
    EXPECT_FALSE(ptr_opt2.has_value());
}

// Concurrency Test: Multiple threads allocating/deallocating the SAME size class
TEST_F(CentralCacheTest, ConcurrentSameSize) {
    const size_t alloc_size = 40; // Use a less common size maybe
    const size_t num_threads = 4;
    const int iterations_per_thread = 500;
    const size_t blocks_per_alloc = 3;

    std::vector<std::thread> threads;
    std::atomic<int> success_count = 0;
    std::atomic<int> fail_count = 0;

    for (size_t i = 0; i < num_threads; ++i) {
        threads.emplace_back([&]() {
            std::vector<std::byte*> allocations; // Store pointers to deallocate later
            int local_success = 0;
            int local_fail = 0;
            std::mt19937 rng(std::hash<std::thread::id>{}(std::this_thread::get_id())); // Seed PRNG per thread
            std::uniform_int_distribution<> dist(1, 10); // Random block count

            for (int j = 0; j < iterations_per_thread; ++j) {
                 size_t count = dist(rng); // Use varying block counts
                 auto head_opt = cache.allocate(alloc_size, count);
                 if (head_opt.has_value() && *head_opt != nullptr) {
                    // Basic check on returned list
                    ASSERT_EQ(count, getListLength(*head_opt));
                    allocations.push_back(*head_opt); // Store head for later deallocation
                    local_success++;
                 } else {
                     // Allocation failure might happen under memory pressure, not necessarily an error
                     local_fail++;
                 }
            }
             // Deallocate everything allocated by this thread
            for (std::byte* head : allocations) {
                // Need the size used during allocation
                cache.deallocate(head, alloc_size);
            }
            success_count += local_success;
            fail_count += local_fail;
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    std::cout << "ConcurrentSameSize Test: Success=" << success_count << ", Fail=" << fail_count << std::endl;
    // We expect mostly successes. Failures might indicate resource exhaustion.
    // Run this test under ASan/TSan to detect races or memory errors.
}

// Concurrency Test: Multiple threads allocating/deallocating DIFFERENT size classes
TEST_F(CentralCacheTest, ConcurrentDifferentSizes) {
    const std::vector<size_t> sizes = {16, 24, 40, 64, 128, 256};
    const size_t num_threads = sizes.size(); // One thread per size
    const int iterations_per_thread = 500;

    std::vector<std::thread> threads;
    std::atomic<int> total_success = 0;
     std::atomic<int> total_fail = 0;

    for (size_t i = 0; i < num_threads; ++i) {
        const size_t alloc_size = sizes[i];
        threads.emplace_back([&, alloc_size]() { // Capture alloc_size by value
             std::vector<std::byte*> allocations;
             int local_success = 0;
             int local_fail = 0;
             std::mt19937 rng(std::hash<std::thread::id>{}(std::this_thread::get_id()));
             std::uniform_int_distribution<> dist(1, 5); // Smaller random block count


            for (int j = 0; j < iterations_per_thread; ++j) {
                size_t count = dist(rng);
                auto head_opt = cache.allocate(alloc_size, count);
                 if (head_opt.has_value() && *head_opt != nullptr) {
                     ASSERT_EQ(count, getListLength(*head_opt));
                     allocations.push_back(*head_opt);
                     local_success++;
                 } else {
                     local_fail++;
                 }
            }
            for (std::byte* head : allocations) {
                cache.deallocate(head, alloc_size);
            }
            total_success += local_success;
            total_fail += local_fail;
        });
    }

    for (auto& t : threads) {
        t.join();
    }
    std::cout << "ConcurrentDifferentSizes Test: Success=" << total_success << ", Fail=" << total_fail << std::endl;
    // Should generally succeed. Run under ASan/TSan.
}


// Stress test with random allocations/deallocations across threads
TEST_F(CentralCacheTest, StressTest) {
    const std::vector<size_t> sizes = {8, 16, 24, 32, 48, 64, 96, 128, 192, 256, 384, 512};
    const size_t num_threads = 8;
    const int operations_per_thread = 2000; // Mix of alloc/dealloc

    std::vector<std::thread> threads;
    std::vector<std::vector<std::pair<std::byte*, size_t>>> live_allocations(num_threads); // Track live allocs per thread

    for (size_t i = 0; i < num_threads; ++i) {
        threads.emplace_back([&, thread_id = i]() {
            std::mt19937 rng(std::hash<std::thread::id>{}(std::this_thread::get_id()) + thread_id);
            std::uniform_int_distribution<> size_dist(0, sizes.size() - 1);
            std::uniform_int_distribution<> count_dist(1, 10);
            std::uniform_int_distribution<> op_dist(0, 1); // 0 = allocate, 1 = deallocate

            auto& my_allocs = live_allocations[thread_id];

            for (int j = 0; j < operations_per_thread; ++j) {
                int operation = op_dist(rng);

                if (my_allocs.empty() || operation == 0) {
                    // Allocate
                    size_t size_index = size_dist(rng);
                    size_t alloc_size = sizes[size_index];
                    size_t block_count = count_dist(rng);

                    auto head_opt = cache.allocate(alloc_size, block_count);
                    if (head_opt.has_value() && *head_opt != nullptr) {
                        // Store head and size for later deallocation
                        // We store the head of the list, assuming deallocate takes the head
                        my_allocs.push_back({*head_opt, alloc_size});
                    }
                } else {
                    // Deallocate
                    std::uniform_int_distribution<> dealloc_idx_dist(0, my_allocs.size() - 1);
                    size_t dealloc_idx = dealloc_idx_dist(rng);

                    std::pair<std::byte*, size_t> alloc_to_free = my_allocs[dealloc_idx];
                    // Remove from live list *before* deallocating
                    std::swap(my_allocs[dealloc_idx], my_allocs.back());
                    my_allocs.pop_back();

                    cache.deallocate(alloc_to_free.first, alloc_to_free.second);
                }
            }

            // Clean up any remaining allocations for this thread
            for (const auto& alloc : my_allocs) {
                cache.deallocate(alloc.first, alloc.second);
            }
            my_allocs.clear(); // Ensure list is clear after cleanup
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    // After stress, maybe check if any free lists are unexpectedly huge or small
    // This requires accessing internal state.
    // Primarily rely on ASan/TSan/Valgrind for detecting issues during the stress run.
    std::cout << "Stress Test Completed." << std::endl;
}


// Potential test for double free (EXPECT DEATH if using ASan or asserts are enabled)
// Or just check internal state consistency if possible
TEST_F(CentralCacheTest, DoubleFree) {
     const size_t alloc_size = 16;
     auto ptr_opt = cache.allocate(alloc_size, 1);
     ASSERT_TRUE(ptr_opt.has_value());
     std::byte* ptr = *ptr_opt;
     ASSERT_NE(nullptr, ptr);

     cache.deallocate(ptr, alloc_size);

     // Deallocating again
     // This might crash, trigger an assert, or corrupt state depending on implementation details
     // If asserts are off and it doesn't crash, checking internal state might reveal inconsistency.
     // Using ASSERT_DEATH or EXPECT_DEATH might be appropriate if a crash is expected.
     // EXPECT_NO_FATAL_FAILURE might be used if it's expected to handle it gracefully (unlikely).
     // For now, just call it and rely on sanitizers. If it crashes, the test fails.
     // If specific assert messages are expected, ASSERT_DEATH can check for them.
 #ifndef NDEBUG // Asserts enabled
      ASSERT_DEATH(cache.deallocate(ptr, alloc_size), ".*"); // Expect assertion failure or crash
 #else
     // In release mode, behavior is less predictable. Might corrupt state.
     // We might just call it and hope sanitizers catch issues, or skip this specific check.
     // Or try to allocate again and see if things seem consistent.
     // Let's just call it and rely on sanitizers / test failure on crash.
     // cache.deallocate(ptr, alloc_size); // Uncomment cautiously
     std::cout << "Skipping explicit double free check in release mode (rely on sanitizers/crash)" << std::endl;
 #endif

}

// Potential test for freeing invalid pointer (EXPECT DEATH or check consistency)
TEST_F(CentralCacheTest, InvalidFree) {
    const size_t alloc_size = 16;
    std::byte invalid_ptr_stack[100]; // A pointer not from the allocator

     // Similar to DoubleFree, this might crash or assert.
 #ifndef NDEBUG // Asserts enabled
     // The assert `it != m_page_set[index].begin()` in deallocate is likely to fire.
     ASSERT_DEATH(cache.deallocate(invalid_ptr_stack, alloc_size), ".*");
 #else
      std::cout << "Skipping explicit invalid free check in release mode (rely on sanitizers/crash)" << std::endl;
     // cache.deallocate(invalid_ptr_stack, alloc_size); // Uncomment cautiously
 #endif
}