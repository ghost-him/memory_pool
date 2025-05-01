#include <iostream>
#include <vector>
#include <thread>
#include <chrono>
#include <random>
#include <numeric>
#include <atomic>
#include <optional>
#include <memory> // for std::unique_ptr if needed, though raw pointers are used here
#include <mutex>
#include <condition_variable>
#include <algorithm>
#include <iomanip>
#include <list> // Using list for easier random removal

// 包含你的内存池头文件
#include "memory_pool.h" // 假设 memory_pool.h 在同一目录或 include 路径中

// --- 配置参数 ---
const unsigned int NUM_THREADS = std::thread::hardware_concurrency(); // 使用硬件支持的并发线程数
const std::chrono::seconds RUN_DURATION = std::chrono::seconds(5); // 运行持续时间 (至少3秒)
const size_t MIN_ALLOC_SIZE = 8;        // 最小申请空间
const size_t MAX_ALLOC_SIZE = 4096;     // 最大申请空间 (模拟常见的小对象)
const int ALLOC_PERCENTAGE = 60;        // 申请操作的百分比 (剩下的是释放)
const unsigned int RANDOM_SEED = 12345; // 固定随机种子以保证可复现性
const size_t OPS_PER_CHUNK = 100;      // 每个线程在检查时间前执行的操作次数块

// --- 统计结构 ---
struct Stats {
    std::atomic<size_t> total_allocs{0};
    std::atomic<size_t> successful_allocs{0};
    std::atomic<size_t> failed_allocs{0};
    std::atomic<size_t> total_deallocs{0};
    std::atomic<long long> total_alloc_latency_ns{0};
    std::atomic<long long> total_dealloc_latency_ns{0};
    std::atomic<size_t> peak_memory_usage{0}; // 追踪测试程序自身分配的总字节数峰值
    std::atomic<size_t> current_memory_usage{0}; // 当前测试程序分配的总字节数
    std::vector<long long> alloc_latencies; // 存储所有分配延迟 (用于P99计算)
    std::vector<long long> dealloc_latencies; // 存储所有释放延迟 (用于P99计算)
    std::mutex latency_mutex; // 保护延迟向量

};

// --- 操作类型 ---
enum class OpType { ALLOCATE, DEALLOCATE };

// --- 预生成的操作序列 ---
struct Operation {
    OpType type;
    size_t size;
};

// --- 工作线程函数 ---
template <typename AllocFunc, typename DeallocFunc>
void worker_thread(
    int thread_id,
    const std::vector<Operation>& operations,
    AllocFunc allocate_func,
    DeallocFunc deallocate_func,
    Stats& global_stats,
    std::atomic<bool>& should_stop,
    std::condition_variable& cv_start,
    std::mutex& mtx_start,
    bool& ready_to_start)
{
    // 本地统计和状态
    size_t local_allocs = 0;
    size_t local_successful_allocs = 0;
    size_t local_failed_allocs = 0;
    size_t local_deallocs = 0;
    long long local_alloc_latency_ns = 0;
    long long local_dealloc_latency_ns = 0;
    size_t local_current_memory = 0;
    size_t local_peak_memory = 0;
    std::vector<long long> local_alloc_latencies_vec;
    std::vector<long long> local_dealloc_latencies_vec;
    local_alloc_latencies_vec.reserve(operations.size() * ALLOC_PERCENTAGE / 100); // 预估大小
    local_dealloc_latencies_vec.reserve(operations.size() * (100 - ALLOC_PERCENTAGE) / 100);

    // 存储当前线程持有的分配信息 (指针和大小)
    // 使用 std::list 以便高效随机删除
    std::list<std::pair<void*, size_t>> allocations;

    // 设置随机数引擎以选择要释放的块 (即使操作序列是固定的，释放哪个块可以是随机的)
    std::mt19937 rng(RANDOM_SEED + thread_id); // 每个线程不同的种子，但基于全局种子

    // 等待开始信号
    {
        std::unique_lock<std::mutex> lock(mtx_start);
        cv_start.wait(lock, [&ready_to_start]{ return ready_to_start; });
    }

    size_t op_index = 0;
    auto start_time = std::chrono::high_resolution_clock::now();

    while (!should_stop.load(std::memory_order_relaxed)) {
        for (size_t i = 0; i < OPS_PER_CHUNK && !should_stop.load(std::memory_order_relaxed); ++i) {
            const Operation& op = operations[op_index % operations.size()];
            op_index++;

            if (op.type == OpType::ALLOCATE) {
                local_allocs++;
                auto alloc_start = std::chrono::high_resolution_clock::now();
                auto result = allocate_func(op.size); // 调用分配函数
                auto alloc_end = std::chrono::high_resolution_clock::now();
                auto latency = std::chrono::duration_cast<std::chrono::nanoseconds>(alloc_end - alloc_start).count();
                local_alloc_latency_ns += latency;
                local_alloc_latencies_vec.push_back(latency);

                void* ptr = nullptr;
                bool success = false;

                // 处理 std::optional<void*> 或 void* 返回值
                if constexpr (std::is_same_v<decltype(result), std::optional<void*>>) {
                    if (result) {
                       ptr = *result;
                       success = true;
                    }
                } else { // 假设返回 void* (例如 malloc)
                    ptr = result;
                    if (ptr != nullptr) {
                        success = true;
                    }
                }

                if (success) {
                    local_successful_allocs++;
                    allocations.push_back({ptr, op.size});
                    local_current_memory += op.size;
                    local_peak_memory = std::max(local_peak_memory, local_current_memory);
                } else {
                    local_failed_allocs++;
                    // 可以选择在这里添加日志或处理失败
                }
            } else { // DEALLOCATE
                if (!allocations.empty()) {
                    // 随机选择一个已分配的块来释放
                    std::uniform_int_distribution<size_t> dist(0, allocations.size() - 1);
                    size_t index_to_remove = dist(rng);
                    auto it = allocations.begin();
                    std::advance(it, index_to_remove);
                    void* ptr_to_free = it->first;
                    size_t size_to_free = it->second;

                    local_deallocs++;
                    auto dealloc_start = std::chrono::high_resolution_clock::now();
                    deallocate_func(ptr_to_free, size_to_free); // 调用释放函数
                    auto dealloc_end = std::chrono::high_resolution_clock::now();
                    auto latency = std::chrono::duration_cast<std::chrono::nanoseconds>(dealloc_end - dealloc_start).count();
                    local_dealloc_latency_ns += latency;
                    local_dealloc_latencies_vec.push_back(latency);

                    local_current_memory -= size_to_free;
                    allocations.erase(it);
                }
                 // else: 尝试释放但没有可释放的块，可以忽略或计数
            }
        }
        // 检查是否超时 (粗略检查，避免每次循环都检查)
        // if (std::chrono::high_resolution_clock::now() - start_time > RUN_DURATION) {
        //     should_stop.store(true, std::memory_order_relaxed);
        // }
        // should_stop 由主线程控制
    }

    // 测试结束后，释放所有剩余的分配
    for (const auto& alloc_info : allocations) {
        // 注意：这里不计时，因为这是清理阶段
        deallocate_func(alloc_info.first, alloc_info.second);
        local_current_memory -= alloc_info.second; // 应该最终为 0
    }
    allocations.clear();


    // 更新全局统计信息 (原子操作)
    global_stats.total_allocs.fetch_add(local_allocs, std::memory_order_relaxed);
    global_stats.successful_allocs.fetch_add(local_successful_allocs, std::memory_order_relaxed);
    global_stats.failed_allocs.fetch_add(local_failed_allocs, std::memory_order_relaxed);
    global_stats.total_deallocs.fetch_add(local_deallocs, std::memory_order_relaxed);
    global_stats.total_alloc_latency_ns.fetch_add(local_alloc_latency_ns, std::memory_order_relaxed);
    global_stats.total_dealloc_latency_ns.fetch_add(local_dealloc_latency_ns, std::memory_order_relaxed);

    // 更新峰值内存使用需要锁或更复杂的原子操作，这里采用线程结束后累加线程峰值的方式（这是一个近似值）
    // 或者在每次 local_peak_memory 更新时使用 fetch_max (如果可用且高效)
    // 这里采用简单累加线程峰值的方式，表示各线程在其生命周期内达到的最大值之和，不是真实的全局同时峰值
    // 更精确的全局峰值需要在每次分配/释放后原子地更新全局当前值并比较峰值
    size_t current_total_mem = global_stats.current_memory_usage.fetch_add(local_peak_memory, std::memory_order_relaxed); // 使用 peak 作为贡献值
    // global_stats.peak_memory_usage.fetch_max(current_total_mem + local_peak_memory); // 尝试更新全局峰值 (需要 C++20 atomic fetch_max 或锁)
    // 简单地累加每个线程的峰值作为估算
    global_stats.peak_memory_usage.fetch_add(local_peak_memory, std::memory_order_relaxed);


    // 合并延迟数据 (需要锁)
    {
        std::lock_guard<std::mutex> lock(global_stats.latency_mutex);
        global_stats.alloc_latencies.insert(global_stats.alloc_latencies.end(),
                                            local_alloc_latencies_vec.begin(), local_alloc_latencies_vec.end());
        global_stats.dealloc_latencies.insert(global_stats.dealloc_latencies.end(),
                                              local_dealloc_latencies_vec.begin(), local_dealloc_latencies_vec.end());
    }
}

// --- 辅助函数：计算 P99 延迟 ---
long long calculate_p99_latency(std::vector<long long>& latencies) {
    if (latencies.empty()) {
        return 0;
    }
    std::sort(latencies.begin(), latencies.end());
    size_t index = static_cast<size_t>(latencies.size() * 0.99);
    if (index >= latencies.size()) {
        index = latencies.size() - 1; // 防止越界
    }
    return latencies[index];
}

// --- 运行基准测试 ---
template <typename AllocFunc, typename DeallocFunc>
void run_benchmark(const std::string& name,
                   const std::vector<std::vector<Operation>>& ops_per_thread,
                   AllocFunc allocate_func,
                   DeallocFunc deallocate_func,
                   Stats& stats)
{
    std::cout << "\n--- Running Benchmark: " << name << " ---" << std::endl;
    std::cout << "Threads: " << NUM_THREADS << ", Duration: " << RUN_DURATION.count() << "s" << std::endl;

    std::vector<std::thread> threads;
    std::atomic<bool> should_stop{false};
    std::mutex mtx_start;
    std::condition_variable cv_start;
    bool ready_to_start = false;

    // --- 调整内存池阈值 (如果需要) ---
    // 可以在这里为 memory_pool 设置特定的阈值，如果需要对比不同设置的话
    // 例如: if constexpr (std::is_same_v<AllocFunc, decltype(&memory_pool::memory_pool::allocate)>) {
    //          memory_pool::memory_pool::set_this_thread_max_free_memory_blocks(SOME_VALUE); // 注意：这只设置主线程的，worker 需要自己设置
    //       }
    // 更通用的做法是在 worker_thread 开始时设置

    // 创建工作线程
    threads.reserve(NUM_THREADS);
    for (unsigned int i = 0; i < NUM_THREADS; ++i) {
        threads.emplace_back(worker_thread<AllocFunc, DeallocFunc>,
                             i,
                             std::ref(ops_per_thread[i]),
                             allocate_func,
                             deallocate_func,
                             std::ref(stats),
                             std::ref(should_stop),
                             std::ref(cv_start),
                             std::ref(mtx_start),
                             std::ref(ready_to_start));
    }

    // 确保所有线程准备就绪 (可选，但有助于同步启动)
    // std::this_thread::sleep_for(std::chrono::milliseconds(100)); // 简单等待

    auto benchmark_start_time = std::chrono::high_resolution_clock::now();

    // 发送开始信号
    {
        std::lock_guard<std::mutex> lock(mtx_start);
        ready_to_start = true;
    }
    cv_start.notify_all();

    // 等待指定的运行时间
    std::this_thread::sleep_for(RUN_DURATION);

    // 通知线程停止
    should_stop.store(true, std::memory_order_relaxed);

    // 等待所有线程完成
    for (auto& t : threads) {
        if (t.joinable()) {
            t.join();
        }
    }

    auto benchmark_end_time = std::chrono::high_resolution_clock::now();
    auto total_duration = std::chrono::duration_cast<std::chrono::milliseconds>(benchmark_end_time - benchmark_start_time);

    // --- 计算并打印统计结果 ---
    size_t total_ops = stats.total_allocs.load() + stats.total_deallocs.load();
    double avg_alloc_latency_us = (stats.successful_allocs.load() == 0) ? 0 : (double)stats.total_alloc_latency_ns.load() / stats.successful_allocs.load() / 1000.0;
    double avg_dealloc_latency_us = (stats.total_deallocs.load() == 0) ? 0 : (double)stats.total_dealloc_latency_ns.load() / stats.total_deallocs.load() / 1000.0;

    // 计算 P99 延迟 (需要对收集到的所有延迟数据排序)
    long long p99_alloc_latency_ns = calculate_p99_latency(stats.alloc_latencies);
    long long p99_dealloc_latency_ns = calculate_p99_latency(stats.dealloc_latencies);
    double p99_alloc_latency_us = (double)p99_alloc_latency_ns / 1000.0;
    double p99_dealloc_latency_us = (double)p99_dealloc_latency_ns / 1000.0;


    std::cout << std::fixed << std::setprecision(2);
    std::cout << "Total Time:         " << total_duration.count() << " ms" << std::endl;
    std::cout << "Total Operations:   " << total_ops << std::endl;
    std::cout << "Operations/Sec:     " << (total_ops * 1000.0 / total_duration.count()) << std::endl;
    std::cout << "Successful Allocs:  " << stats.successful_allocs.load() << std::endl;
    std::cout << "Failed Allocs:      " << stats.failed_allocs.load() << std::endl;
    std::cout << "Total Deallocs:     " << stats.total_deallocs.load() << std::endl;
    std::cout << "Avg Alloc Latency:  " << avg_alloc_latency_us << " us" << std::endl;
    std::cout << "P99 Alloc Latency:  " << p99_alloc_latency_us << " us" << std::endl;
    std::cout << "Avg Dealloc Latency:" << avg_dealloc_latency_us << " us" << std::endl;
    std::cout << "P99 Dealloc Latency:" << p99_dealloc_latency_us << " us" << std::endl;
    // 注意：Peak Memory Usage 是所有线程在其生命周期内峰值内存占用的总和，不是精确的全局同时峰值
    std::cout << "Peak Memory Usage (Sum of Thread Peaks): " << (stats.peak_memory_usage.load() / 1024.0 / 1024.0) << " MB" << std::endl;
    std::cout << "--- Benchmark Finished: " << name << " ---" << std::endl;
}

// --- main 函数 ---
int main() {
    std::cout << "Starting High-Concurrency Memory Allocator Benchmark..." << std::endl;
    std::cout << "======================================================" << std::endl;

    // --- 1. 生成确定性的操作序列 ---
    std::cout << "Generating deterministic operation sequence..." << std::endl;
    std::vector<std::vector<Operation>> ops_per_thread(NUM_THREADS);
    std::mt19937 master_rng(RANDOM_SEED); // 主随机数生成器
    std::uniform_int_distribution<size_t> size_dist(MIN_ALLOC_SIZE, MAX_ALLOC_SIZE);
    std::uniform_int_distribution<int> op_dist(1, 100);

    // 估算每个线程在运行期间可能执行的总操作数，可以设置一个较大的上限
    size_t estimated_ops_per_thread = (1000000 / NUM_THREADS) * RUN_DURATION.count(); // 粗略估计
    estimated_ops_per_thread = std::max(estimated_ops_per_thread, (size_t)200000); // 保证至少有这么多操作

    for (unsigned int i = 0; i < NUM_THREADS; ++i) {
        ops_per_thread[i].reserve(estimated_ops_per_thread);
        for (size_t j = 0; j < estimated_ops_per_thread; ++j) {
            Operation op;
            op.size = size_dist(master_rng);
            if (op_dist(master_rng) <= ALLOC_PERCENTAGE) {
                op.type = OpType::ALLOCATE;
            } else {
                op.type = OpType::DEALLOCATE;
                // Deallocate 不需要 size，但为了结构统一可以保留，或者设为0
                // worker 函数会忽略 deallocate 操作的 size，而是用追踪列表里的
            }
            ops_per_thread[i].push_back(op);
        }
    }
    std::cout << "Generated " << estimated_ops_per_thread << " operations per thread." << std::endl;


    // --- 2. 定义分配器接口包装 ---

    // memory_pool 包装器
    auto memory_pool_alloc = [](size_t size) {
        // 可以在这里为当前线程设置阈值 (但最好在线程开始时做)
        // memory_pool::memory_pool::set_this_thread_max_free_memory_blocks(YOUR_THRESHOLD);
        return memory_pool::memory_pool::allocate(size);
    };
    auto memory_pool_dealloc = [](void* p, size_t s) {
        memory_pool::memory_pool::deallocate(p, s);
    };

    // malloc/free 包装器
    auto malloc_alloc = [](size_t size) -> void* { // 返回 void* 以匹配 worker 模板期望
        return malloc(size);
    };
    auto malloc_dealloc = [](void* p, size_t /* ignored_size */) {
        free(p);
    };


    // --- 3. 运行基准测试 ---

    Stats pool_stats;
    run_benchmark("Custom Memory Pool", ops_per_thread, memory_pool_alloc, memory_pool_dealloc, pool_stats); // <--- 传入引用

    // 运行 malloc/free 测试
    Stats malloc_stats;
    run_benchmark("Standard malloc/free", ops_per_thread, malloc_alloc, malloc_dealloc, malloc_stats); // <--- 传入引用

    // --- 4. 对比结果 (简单对比) ---
    std::cout << "\n--- Benchmark Comparison ---" << std::endl;
    double pool_ops_sec = (pool_stats.total_allocs.load() + pool_stats.total_deallocs.load()) * 1000.0 / RUN_DURATION.count();
    double malloc_ops_sec = (malloc_stats.total_allocs.load() + malloc_stats.total_deallocs.load()) * 1000.0 / RUN_DURATION.count();

    double pool_avg_alloc_us = (pool_stats.successful_allocs.load() == 0) ? 0 : (double)pool_stats.total_alloc_latency_ns.load() / pool_stats.successful_allocs.load() / 1000.0;
    double malloc_avg_alloc_us = (malloc_stats.successful_allocs.load() == 0) ? 0 : (double)malloc_stats.total_alloc_latency_ns.load() / malloc_stats.successful_allocs.load() / 1000.0;
    double pool_p99_alloc_us = calculate_p99_latency(pool_stats.alloc_latencies) / 1000.0;
    double malloc_p99_alloc_us = calculate_p99_latency(malloc_stats.alloc_latencies) / 1000.0;


    std::cout << std::fixed << std::setprecision(2);
    std::cout << "Metric                 | Custom Pool         | Standard malloc/free | Improvement" << std::endl;
    std::cout << "-----------------------|---------------------|----------------------|-------------" << std::endl;
    std::cout << "Ops/Sec                | " << std::setw(19) << pool_ops_sec
              << " | " << std::setw(20) << malloc_ops_sec
              << " | " << std::setw(10) << (pool_ops_sec / malloc_ops_sec * 100.0) << "%" << std::endl;
    std::cout << "Avg Alloc Latency (us) | " << std::setw(19) << pool_avg_alloc_us
              << " | " << std::setw(20) << malloc_avg_alloc_us
              << " | " << std::setw(9) << (malloc_avg_alloc_us / pool_avg_alloc_us * 100.0) << "%" << std::endl; // Lower is better
    std::cout << "P99 Alloc Latency (us) | " << std::setw(19) << pool_p99_alloc_us
              << " | " << std::setw(20) << malloc_p99_alloc_us
              << " | " << std::setw(9) << (malloc_p99_alloc_us / pool_p99_alloc_us * 100.0) << "%" << std::endl; // Lower is better
    // Add similar lines for deallocation latency if desired
    std::cout << "Peak Memory (MB)       | " << std::setw(19) << (pool_stats.peak_memory_usage.load() / 1024.0 / 1024.0)
              << " | " << std::setw(20) << (malloc_stats.peak_memory_usage.load() / 1024.0 / 1024.0)
              << " | " << std::endl; // Lower is generally better

    std::cout << "======================================================" << std::endl;

    return 0;
}