// tests/thread_cache_test.cpp
// 该内容由 gemini 2.5 pro preview 03-25 生成，https://aistudio.google.com/prompts/new_chat
#include <gtest/gtest.h>
#include <vector>
#include <thread> // Include for potential multi-threading tests later, though not strictly needed for these basic ones
#include <set>
#include <optional>

// Include the header for the class we are testing
#include "thread_cache.h"
// Include necessary utility headers from your project
#include "utils.h" // Assuming size_utils and MAX_CACHED_UNIT_SIZE are here
#include "central_cache.h" // Needed indirectly, ensure it links

// Using namespace for convenience in the test file
using namespace memory_pool;

// Test Fixture (Optional, but good practice if setup/teardown is needed)
// For thread_cache, since it's a thread_local singleton, getting the instance
// within each test is often sufficient.
class ThreadCacheTest : public ::testing::Test {
protected:
    // Per-test setup can go here if needed
    void SetUp() override {
        // Reset central cache state if necessary, though difficult without internal access
        // Ensure thread_cache starts in a known state (e.g., clear free lists - hard without friend access)
        // For simplicity, we rely on each test running on a 'clean' thread cache state
        // or accept state leakage between tests within the same thread.
    }

    // Per-test tear-down can go here if needed
    void TearDown() override {
        // Clean up any resources allocated during the test
    }

    // Helper to get the thread cache instance
    thread_cache& get_cache() {
        return thread_cache::get_instance();
    }
};

// Test case for basic allocation and deallocation of small sizes
TEST_F(ThreadCacheTest, BasicAllocationDeallocation) {
    thread_cache& cache = get_cache();
    const size_t alloc_size = 16; // A small size, likely cached

    // Allocate memory
    std::optional<void*> ptr_opt = cache.allocate(alloc_size);
    ASSERT_TRUE(ptr_opt.has_value()); // Check if allocation succeeded
    ASSERT_NE(ptr_opt.value(), nullptr); // Check if the pointer is valid

    // Simple check: try writing to the memory (optional)
    // char* data = static_cast<char*>(ptr_opt.value());
    // data[0] = 'a';
    // data[alloc_size - 1] = 'z';

    // Deallocate the memory
    cache.deallocate(ptr_opt.value(), alloc_size);
    // No direct way to assert deallocation success other than avoiding crashes
    // and potentially checking for reuse below.
}

// Test case for reusing a block after deallocation
TEST_F(ThreadCacheTest, ReuseDeallocatedBlock) {
    thread_cache& cache = get_cache();
    const size_t alloc_size = 32;

    // Allocate and deallocate once
    std::optional<void*> ptr1_opt = cache.allocate(alloc_size);
    ASSERT_TRUE(ptr1_opt.has_value());
    void* ptr1 = ptr1_opt.value();
    cache.deallocate(ptr1, alloc_size);

    // Allocate again with the same size
    std::optional<void*> ptr2_opt = cache.allocate(alloc_size);
    ASSERT_TRUE(ptr2_opt.has_value());

    // Because the block was just deallocated and pushed to the front
    // of the free list, the next allocation of the same size should reuse it.
    EXPECT_EQ(ptr2_opt.value(), ptr1);

    // Deallocate the second pointer
    cache.deallocate(ptr2_opt.value(), alloc_size);
}

// Test case for allocating sizes larger than the cache limit
TEST_F(ThreadCacheTest, LargeAllocation) {
    thread_cache& cache = get_cache();
    // Calculate a size guaranteed to be larger than the max cacheable unit
    const size_t large_alloc_size = size_utils::MAX_CACHED_UNIT_SIZE + 8;

    // Allocate large memory block
    std::optional<void*> ptr_opt = cache.allocate(large_alloc_size);
    ASSERT_TRUE(ptr_opt.has_value());
    ASSERT_NE(ptr_opt.value(), nullptr);

    // Deallocate the large block
    cache.deallocate(ptr_opt.value(), large_alloc_size);
    // This tests the path that bypasses the free lists and goes directly
    // to central_cache for both allocation and deallocation.
}

// Test case for multiple allocations of different small sizes
TEST_F(ThreadCacheTest, MultipleSmallAllocations) {
    thread_cache& cache = get_cache();
    std::vector<void*> allocated_pointers;
    std::vector<size_t> allocated_sizes = {8, 16, 24, 64, 128, 256, 8, 16}; // Mix of sizes within cache range

    // Allocate several blocks
    for (size_t size : allocated_sizes) {
        std::optional<void*> ptr_opt = cache.allocate(size);
        ASSERT_TRUE(ptr_opt.has_value()) << "Failed to allocate size: " << size;
        ASSERT_NE(ptr_opt.value(), nullptr);
        allocated_pointers.push_back(ptr_opt.value());
    }

    // Ensure pointers are distinct (basic sanity check)
    std::set<void*> pointer_set(allocated_pointers.begin(), allocated_pointers.end());
    ASSERT_EQ(pointer_set.size(), allocated_pointers.size());

    // Deallocate all blocks (in reverse order, just for variety)
    for (size_t i = 0; i < allocated_pointers.size(); ++i) {
        size_t index = allocated_pointers.size() - 1 - i;
        cache.deallocate(allocated_pointers[index], allocated_sizes[index]);
    }
}

// Test allocation with size 0
TEST_F(ThreadCacheTest, AllocateZeroSizeReturnsNullopt) {
    thread_cache& cache = get_cache();
    const size_t zero_size = 0;

    // Call allocate with size 0
    std::optional<void*> ptr_opt = cache.allocate(zero_size);

    // Verify that the allocation returns std::nullopt as per the updated logic
    // ASSERT_FALSE is appropriate here, as returning a value is a test failure.
    ASSERT_FALSE(ptr_opt.has_value());
}

// Test alignment (implicitly tested, but good to be aware)
// Assumes central_cache provides aligned memory.
TEST_F(ThreadCacheTest, AlignmentTest) {
    thread_cache& cache = get_cache();
    // Request an unaligned size
    const size_t unaligned_size = 13;
    const size_t expected_aligned_size = size_utils::align(unaligned_size); // e.g., 16 if alignment is 8

    std::optional<void*> ptr_opt = cache.allocate(unaligned_size);
    ASSERT_TRUE(ptr_opt.has_value());
    void* ptr = ptr_opt.value();
    ASSERT_NE(ptr, nullptr);

    // Check if the returned pointer is aligned (usually to 8 or 16 bytes)
    EXPECT_EQ(reinterpret_cast<uintptr_t>(ptr) % size_utils::ALIGNMENT, 0);

    // Deallocate using the original requested size
    cache.deallocate(ptr, unaligned_size);
}