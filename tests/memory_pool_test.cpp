// 该内容由 gemini 2.5 pro preview 03-25 生成，https://aistudio.google.com/prompts/new_chat
#include "memory_pool.h" // Include the top-level header
#include "utils.h"      // Include utils for constants and alignment functions

#include <gtest/gtest.h>
#include <vector>
#include <thread>
#include <numeric>
#include <set>
#include <map>
#include <future>
#include <atomic>
#include <cstring> // For memset
#include <limits>  // For numeric_limits

// Helper function to get aligned size, mimicking internal behavior for deallocation
size_t get_aligned_size(size_t size) {
    return memory_pool::size_utils::align(size);
}

// === Basic Allocation and Deallocation Tests ===

TEST(MemoryPoolTest, AllocateZeroSize) {
    // 根据需求，分配0字节应返回std::nullopt
    size_t requested_size = 0;

    auto ptr_opt = memory_pool::memory_pool::allocate(requested_size);

    // 断言分配失败（返回nullopt）
    ASSERT_FALSE(ptr_opt.has_value()) << "分配0字节大小意外成功。";

    // 无需释放，因为分配应该已失败。
}

TEST(MemoryPoolTest, AllocateMinimumAlignedSize) {
    // 分配最小的正对齐内存块
    size_t size = memory_pool::size_utils::ALIGNMENT; // 例如8字节
    ASSERT_GT(size, 0); // 确保对齐大小不为0

    auto ptr_opt = memory_pool::memory_pool::allocate(size);
    ASSERT_TRUE(ptr_opt.has_value()) << "最小对齐大小（" << size << "）分配失败。";
    void* ptr = ptr_opt.value();
    ASSERT_NE(ptr, nullptr);

    // 基本的写入/读取检查
    memset(ptr, 0xAA, size);
    // 检查首字节和末字节
    ASSERT_EQ(static_cast<unsigned char*>(ptr)[0], 0xAA);
    ASSERT_EQ(static_cast<unsigned char*>(ptr)[size - 1], 0xAA);

    memory_pool::memory_pool::deallocate(ptr, size);
}

TEST(MemoryPoolTest, AllocateSmallSizeWithinCache) {
    // Allocate a size that should be handled by thread_cache free lists
    size_t size = 32;
    ASSERT_LE(size, memory_pool::size_utils::MAX_CACHED_UNIT_SIZE);
    ASSERT_EQ(size % memory_pool::size_utils::ALIGNMENT, 0); // Ensure it's aligned

    auto ptr_opt = memory_pool::memory_pool::allocate(size);
    ASSERT_TRUE(ptr_opt.has_value());
    void* ptr = ptr_opt.value();
    ASSERT_NE(ptr, nullptr);

    memset(ptr, 0xBB, size);
    ASSERT_EQ(static_cast<unsigned char*>(ptr)[size / 2], 0xBB);

    memory_pool::memory_pool::deallocate(ptr, size);
}

TEST(MemoryPoolTest, AllocateMaxSizeWithinCache) {
    // Allocate the largest size handled by the thread_cache free lists
    size_t size = memory_pool::size_utils::MAX_CACHED_UNIT_SIZE; // e.g., 512 bytes
    ASSERT_EQ(size % memory_pool::size_utils::ALIGNMENT, 0);

    auto ptr_opt = memory_pool::memory_pool::allocate(size);
    ASSERT_TRUE(ptr_opt.has_value());
    void* ptr = ptr_opt.value();
    ASSERT_NE(ptr, nullptr);

    memset(ptr, 0xCC, size);
    ASSERT_EQ(static_cast<unsigned char*>(ptr)[size - 1], 0xCC);

    memory_pool::memory_pool::deallocate(ptr, size);
}

TEST(MemoryPoolTest, AllocateSlightlyLargerThanCache) {
    // Allocate a size just over the thread_cache limit, likely hitting central_cache directly
    size_t size = memory_pool::size_utils::MAX_CACHED_UNIT_SIZE + memory_pool::size_utils::ALIGNMENT;
    size_t aligned_size = get_aligned_size(size); // Should be == size if ALIGNMENT divides MAX_CACHED_UNIT_SIZE

    auto ptr_opt = memory_pool::memory_pool::allocate(size);
    ASSERT_TRUE(ptr_opt.has_value());
    void* ptr = ptr_opt.value();
    ASSERT_NE(ptr, nullptr);

    memset(ptr, 0xDD, aligned_size); // Use aligned size for memset range check
    ASSERT_EQ(static_cast<unsigned char*>(ptr)[0], 0xDD);
    ASSERT_EQ(static_cast<unsigned char*>(ptr)[aligned_size - 1], 0xDD);


    memory_pool::memory_pool::deallocate(ptr, aligned_size); // Deallocate with aligned size
}

TEST(MemoryPoolTest, AllocateLargeSize) {
    // Allocate a large block, likely hitting page_cache (multiple pages)
    size_t size = memory_pool::size_utils::PAGE_SIZE * 4; // 16 KiB
    size_t aligned_size = get_aligned_size(size);
    ASSERT_EQ(aligned_size, size); // Should already be page aligned -> 8-byte aligned

    auto ptr_opt = memory_pool::memory_pool::allocate(size);
    ASSERT_TRUE(ptr_opt.has_value());
    void* ptr = ptr_opt.value();
    ASSERT_NE(ptr, nullptr);

    memset(ptr, 0xEE, size);
    ASSERT_EQ(static_cast<unsigned char*>(ptr)[size - 1], 0xEE);

    memory_pool::memory_pool::deallocate(ptr, size);
}


TEST(MemoryPoolTest, AllocateUnalignedSize) {
    // Allocate an unaligned size and verify deallocation works correctly
    // when using the *aligned* size for deallocation.
    size_t requested_size = 21;
    size_t aligned_size = get_aligned_size(requested_size); // Should be 24
    ASSERT_EQ(aligned_size, 24);
    ASSERT_NE(requested_size, aligned_size);

    auto ptr_opt = memory_pool::memory_pool::allocate(requested_size);
    ASSERT_TRUE(ptr_opt.has_value());
    void* ptr = ptr_opt.value();
    ASSERT_NE(ptr, nullptr);

    // User can only safely write up to the requested size
    memset(ptr, 0xFF, requested_size);
    ASSERT_EQ(static_cast<unsigned char*>(ptr)[0], 0xFF);
    ASSERT_EQ(static_cast<unsigned char*>(ptr)[requested_size - 1], 0xFF);

    // *** CRITICAL: Deallocate with the ALIGNED size ***
    // Based on internal code analysis, this seems necessary for correctness.
    memory_pool::memory_pool::deallocate(ptr, aligned_size);

    // Try allocating the aligned size again to potentially reuse the block
    auto ptr2_opt = memory_pool::memory_pool::allocate(aligned_size);
    ASSERT_TRUE(ptr2_opt.has_value());
    ASSERT_NE(ptr2_opt.value(), nullptr);
    memory_pool::memory_pool::deallocate(ptr2_opt.value(), aligned_size);
}

// === Multiple Allocation/Deallocation Tests ===

TEST(MemoryPoolTest, SequentialAllocDealloc) {
    const size_t num_allocs = 100;
    const size_t size = 64;
    std::vector<void*> pointers;
    pointers.reserve(num_allocs);

    for (size_t i = 0; i < num_allocs; ++i) {
        auto ptr_opt = memory_pool::memory_pool::allocate(size);
        ASSERT_TRUE(ptr_opt.has_value());
        ASSERT_NE(ptr_opt.value(), nullptr);
        pointers.push_back(ptr_opt.value());
        memset(pointers.back(), static_cast<int>(i % 256), size);
    }

    for (void* ptr : pointers) {
        // Simple check before deallocating
        ASSERT_NE(ptr, nullptr);
        memory_pool::memory_pool::deallocate(ptr, size);
    }
}

TEST(MemoryPoolTest, InterleavedAllocDealloc) {
    const size_t num_ops = 200;
    const size_t size1 = 16;
    const size_t size2 = 48; // Different aligned size
    std::vector<std::pair<void*, size_t>> pointers;

    for (size_t i = 0; i < num_ops; ++i) {
        if (i % 3 == 0 && !pointers.empty()) {
            // Deallocate
            auto back_pair = pointers.back();
            pointers.pop_back();
            memory_pool::memory_pool::deallocate(back_pair.first, back_pair.second);
        } else {
            // Allocate
            size_t current_size = (i % 2 == 0) ? size1 : size2;
            auto ptr_opt = memory_pool::memory_pool::allocate(current_size);
            ASSERT_TRUE(ptr_opt.has_value());
            ASSERT_NE(ptr_opt.value(), nullptr);
            memset(ptr_opt.value(), static_cast<int>(i % 256), current_size);
            pointers.push_back({ptr_opt.value(), current_size});
        }
    }

    // Deallocate remaining pointers
    for (const auto& pair : pointers) {
        memory_pool::memory_pool::deallocate(pair.first, pair.second);
    }
}

TEST(MemoryPoolTest, NoOverlap) {
    const size_t num_allocs = 500;
    const size_t size = 128;
    std::set<void*> allocated_pointers;
    std::map<void*, size_t> allocated_ranges; // Store ptr -> end_addr

    for (size_t i = 0; i < num_allocs; ++i) {
        auto ptr_opt = memory_pool::memory_pool::allocate(size);
        ASSERT_TRUE(ptr_opt.has_value());
        void* ptr = ptr_opt.value();
        ASSERT_NE(ptr, nullptr);

        // Check 1: Direct pointer equality
        ASSERT_TRUE(allocated_pointers.find(ptr) == allocated_pointers.end()) << "Duplicate pointer returned: " << ptr;
        allocated_pointers.insert(ptr);

        // Check 2: Range overlap
        std::byte* start_byte = static_cast<std::byte*>(ptr);
        std::byte* end_byte = start_byte + size; // End is one past the last byte

        for(const auto& pair : allocated_ranges) {
             std::byte* existing_start = static_cast<std::byte*>(pair.first);
             std::byte* existing_end = existing_start + pair.second;
             // Check if [start_byte, end_byte) overlaps with [existing_start, existing_end)
             bool overlap = (start_byte < existing_end) && (end_byte > existing_start);
             ASSERT_FALSE(overlap) << "Overlap detected between new allocation [" << (void*)start_byte << ", " << (void*)end_byte
                                  << ") and existing [" << (void*)existing_start << ", " << (void*)existing_end << ")";
        }
        allocated_ranges[ptr] = size;

        memset(ptr, static_cast<int>(i % 256), size);
    }

    // Deallocate all
    for (void* ptr : allocated_pointers) {
        memory_pool::memory_pool::deallocate(ptr, size);
    }
}


// === Thread Cache Threshold Tests ===

TEST(MemoryPoolTest, ThreadCacheThresholdGetSet) {
    // Get the default threshold for this thread
    size_t default_threshold = memory_pool::memory_pool::get_this_thread_max_free_memory_blocks();
    ASSERT_GT(default_threshold, 0) << "Default threshold should be positive";

    // Set a new threshold
    size_t new_threshold = 10;
    memory_pool::memory_pool::set_this_thread_max_free_memory_blocks(new_threshold);

    // Verify the new threshold is set
    ASSERT_EQ(memory_pool::memory_pool::get_this_thread_max_free_memory_blocks(), new_threshold);

    // Set another value
    size_t another_threshold = default_threshold * 2;
     memory_pool::memory_pool::set_this_thread_max_free_memory_blocks(another_threshold);
     ASSERT_EQ(memory_pool::memory_pool::get_this_thread_max_free_memory_blocks(), another_threshold);


    // Restore the default (or a known state) to potentially avoid interfering with other tests
    // Note: Thread locality means this only affects the *current* test thread.
    memory_pool::memory_pool::set_this_thread_max_free_memory_blocks(default_threshold);
    ASSERT_EQ(memory_pool::memory_pool::get_this_thread_max_free_memory_blocks(), default_threshold);
}

// === Multi-threading Tests ===

// Task for concurrent allocation/deallocation within a single thread
void AllocDeallocTask(size_t num_allocs, size_t base_alloc_size, std::atomic<bool>& success_flag) {
    std::vector<std::pair<void*, size_t>> pointers;
    pointers.reserve(num_allocs);
    bool local_success = true;

    for (size_t i = 0; i < num_allocs; ++i) {
        // Vary size slightly per allocation to hit different small bins potentially
        size_t requested_size = base_alloc_size + (i % 5) * memory_pool::size_utils::ALIGNMENT;
        size_t aligned_size = get_aligned_size(requested_size);

        auto ptr_opt = memory_pool::memory_pool::allocate(requested_size);
        if (!ptr_opt.has_value() || ptr_opt.value() == nullptr) {
             // Allocation failure is possible under stress, but shouldn't happen easily
             // in this test unless system is under heavy load. Mark as failure for strictness.
             fprintf(stderr, "Thread %ld: Allocation failed for size %zu\n", std::this_thread::get_id(), requested_size);
             local_success = false;
             break; // Stop this thread's test on failure
        }
        void* ptr = ptr_opt.value();
        memset(ptr, static_cast<int>(i % 256), aligned_size); // Use aligned size for memset boundary
        pointers.push_back({ptr, aligned_size}); // Store pointer and *aligned* size
    }

    // Read check (optional, simple check)
    for(size_t i = 0; i < pointers.size(); ++i) {
        unsigned char expected_val = static_cast<unsigned char>(i % 256);
        if (static_cast<unsigned char*>(pointers[i].first)[0] != expected_val) {
             fprintf(stderr, "Thread %ld: Memory corruption detected (read check failed)\n", std::this_thread::get_id());
            local_success = false;
            break;
        }
    }


    // Deallocate all pointers allocated by this thread
    for (const auto& pair : pointers) {
        memory_pool::memory_pool::deallocate(pair.first, pair.second); // Use the stored aligned size
    }

    if (!local_success) {
        success_flag.store(false, std::memory_order_relaxed);
    }
}

TEST(MemoryPoolTest, ConcurrentAllocDeallocSameThread) {
    const int num_threads = std::thread::hardware_concurrency() > 0 ? std::thread::hardware_concurrency() : 4;
    const size_t num_allocs_per_thread = 10000; // Increase for more stress
    const size_t base_alloc_size = 16; // Small allocations likely cached

    std::vector<std::thread> threads;
    std::atomic<bool> success_flag(true);

    std::cout << "Starting ConcurrentAllocDeallocSameThread with " << num_threads << " threads..." << std::endl;

    for (int i = 0; i < num_threads; ++i) {
        // Each thread works with its own allocations/deallocations
        threads.emplace_back(AllocDeallocTask, num_allocs_per_thread, base_alloc_size + i, std::ref(success_flag));
    }

    for (auto& t : threads) {
        t.join();
    }

    std::cout << "ConcurrentAllocDeallocSameThread finished." << std::endl;
    ASSERT_TRUE(success_flag.load(std::memory_order_relaxed)) << "One or more threads failed during concurrent allocation/deallocation.";
}


// Task for cross-thread deallocation test
struct CrossThreadData {
    std::promise<void*> ptr_promise;
    std::future<void*> ptr_future;
    size_t requested_size = 0;
    size_t aligned_size = 0;
    std::atomic<bool> alloc_done{false};
    std::atomic<bool> dealloc_done{false};
    std::atomic<bool> success{true};

    CrossThreadData(size_t size) : requested_size(size) {
        ptr_future = ptr_promise.get_future();
        aligned_size = get_aligned_size(requested_size);
    }
};

void AllocatorThread(CrossThreadData& data) {
    try {
        auto ptr_opt = memory_pool::memory_pool::allocate(data.requested_size);
        if (ptr_opt.has_value() && ptr_opt.value() != nullptr) {
            memset(ptr_opt.value(), 0xAB, data.aligned_size);
            data.ptr_promise.set_value(ptr_opt.value());
        } else {
            fprintf(stderr, "AllocatorThread: Failed to allocate %zu bytes\n", data.requested_size);
            data.ptr_promise.set_exception(std::make_exception_ptr(std::runtime_error("Allocation failed")));
            data.success = false;
        }
    } catch (...) {
        data.ptr_promise.set_exception(std::current_exception());
        data.success = false;
    }
    data.alloc_done = true;
}

void DeallocatorThread(CrossThreadData& data) {
    try {
        // Wait for the pointer from the allocator thread
        void* ptr = data.ptr_future.get(); // This will block until promise is set or throw if exception set

        // Optional: Add a small delay or yield to increase chance of thread switch
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

        // Perform deallocation using the aligned size
        memory_pool::memory_pool::deallocate(ptr, data.aligned_size);

    } catch (const std::exception& e) {
         fprintf(stderr, "DeallocatorThread: Exception caught: %s\n", e.what());
         data.success = false; // Mark failure if exception during get() or deallocate
    } catch (...) {
        fprintf(stderr, "DeallocatorThread: Unknown exception caught\n");
        data.success = false;
    }
     data.dealloc_done = true;
}


TEST(MemoryPoolTest, CrossThreadDeallocation) {
    const size_t alloc_size = 256; // A size likely handled by central cache or page cache

    CrossThreadData test_data(alloc_size);

    std::thread t1(AllocatorThread, std::ref(test_data));
    std::thread t2(DeallocatorThread, std::ref(test_data));

    t1.join();
    t2.join();

    ASSERT_TRUE(test_data.alloc_done) << "Allocator thread did not finish.";
    ASSERT_TRUE(test_data.dealloc_done) << "Deallocator thread did not finish.";
    ASSERT_TRUE(test_data.success) << "Cross-thread deallocation test failed.";
}

// === Stress and Edge Case Tests ===

TEST(MemoryPoolTest, HighFrequencyAllocDealloc) {
    // Allocate and immediately deallocate many times to stress the cache pathways
    const size_t num_ops = 50000;
    const size_t size = 16; // Small size, likely stays in thread cache

    for (size_t i = 0; i < num_ops; ++i) {
        auto ptr_opt = memory_pool::memory_pool::allocate(size);
        ASSERT_TRUE(ptr_opt.has_value());
        ASSERT_NE(ptr_opt.value(), nullptr);
        // No memset needed, just testing alloc/dealloc path
        memory_pool::memory_pool::deallocate(ptr_opt.value(), size);
    }
}

TEST(MemoryPoolTest, VaryingSizesStress) {
    const size_t num_ops = 20000;
    std::vector<std::pair<void*, size_t>> pointers;
    pointers.reserve(num_ops / 2); // Rough estimate

    size_t sizes[] = {8, 16, 32, 64, 128, 256, 512, 1024, 4096, 8192};
    size_t num_sizes = sizeof(sizes) / sizeof(sizes[0]);

    for(size_t i = 0; i < num_ops; ++i) {
         if (i % 5 != 0 && !pointers.empty()) { // Deallocate sometimes
            size_t idx_to_remove = std::rand() % pointers.size();
            std::pair<void*, size_t> item = pointers[idx_to_remove];
            // Swap-and-pop for efficiency
            std::swap(pointers[idx_to_remove], pointers.back());
            pointers.pop_back();
            memory_pool::memory_pool::deallocate(item.first, item.second); // Use stored aligned size
         } else { // Allocate mostly
            size_t requested_size = sizes[std::rand() % num_sizes];
            size_t aligned_size = get_aligned_size(requested_size);
            auto ptr_opt = memory_pool::memory_pool::allocate(requested_size);
             if (ptr_opt.has_value() && ptr_opt.value() != nullptr) {
                 memset(ptr_opt.value(), 0xFE, aligned_size); // Use aligned size for memset
                 pointers.push_back({ptr_opt.value(), aligned_size}); // Store aligned size
             } else {
                 // Log failure, but continue test? Or Assert? Depends on expectation.
                 // If system is truly out of memory, nullopt is valid.
                 // Let's log and continue, but fail if *too many* failures occur?
                 // For now, just log.
                 // fprintf(stderr, "VaryingSizesStress: Failed allocation for size %zu\n", requested_size);
             }
         }
    }

    // Cleanup remaining
    for(const auto& item : pointers) {
        memory_pool::memory_pool::deallocate(item.first, item.second);
    }
     // Test primarily checks for crashes/deadlocks/asserts during the stress period.
}

TEST(MemoryPoolTest, AllocationFailure) {
    // Attempt to allocate an extremely large amount of memory, expecting failure (nullopt)
    // This size should exceed available virtual memory address space or system limits.
    // Using size_t max / 2 is a common way to request an unreasonably large block.
    size_t huge_size = std::numeric_limits<size_t>::max() / 2;

    auto ptr_opt = memory_pool::memory_pool::allocate(huge_size);

    // We expect this allocation to fail and return nullopt
    ASSERT_FALSE(ptr_opt.has_value()) << "Allocation of extremely large size (" << huge_size << ") unexpectedly succeeded.";
}

// === Main function (provided by GTest::gtest_main) ===
// No need to write main() if linking against GTest::gtest_main