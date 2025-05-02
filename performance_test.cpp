// 这个benchmark由 gemini 2.5 pro preview 03-25 生成，https://aistudio.google.com/prompts/new_chat
#include <iostream>          // 用于输入输出流
#include <vector>            // 用于 std::vector 容器
#include <thread>            // 用于 std::thread 多线程支持
#include <chrono>            // 用于时间测量 (high_resolution_clock)
#include <random>            // 用于随机数生成
#include <numeric>           // 用于 std::accumulate (虽然此处未使用，但相关头文件可能有用)
#include <atomic>            // 用于原子操作，保证线程安全统计
#include <memory>            // 用于智能指针和 PMR (多态内存资源)
#include <mutex>             // 用于互斥锁 (std::mutex)
#include <algorithm>         // 用于 std::sort, std::min 等算法
#include <iomanip>           // 用于格式化输出 (setw, setprecision, fixed)
#include <list>              // 使用 std::list 方便随机移除元素
#include <stdexcept>         // 用于 std::bad_alloc 异常
#include <memory_resource>   // C++17/20/23 PMR 特性

// --- 依赖外部文件 ---
// 确保 "memory_pool.h" 在正确的包含路径下，并提供了正确的接口
// 编译命令示例: g++ -std=c++23 -pthread -O3 your_file_name.cpp -o benchmark
// 如果 memory_pool.h 依赖其他库，也需要链接它们
#include "memory_pool.h"     // 假设自定义内存池头文件可用

// --- 配置参数 ---
const unsigned int NUM_THREADS = std::thread::hardware_concurrency(); // 线程数，使用硬件支持的最大并发数
const size_t NUM_OPERATIONS_PER_THREAD = 200000; // 每个线程执行的操作总数
const size_t MIN_ALLOC_SIZE = 1024;                  // 最小分配内存块大小 (字节)
const size_t MAX_ALLOC_SIZE = 4 * 1024;                // 最大分配内存块大小 (字节)，覆盖常见的小对象大小
const int ALLOC_PERCENTAGE = 60;                  // 分配操作所占的百分比
const unsigned int RANDOM_SEED = 54321;           // 固定的随机种子，确保每次运行结果可复现
// std::pmr::memory_resource 要求的默认对齐方式
const size_t DEFAULT_ALIGNMENT = alignof(std::max_align_t);

// --- 统计数据结构 ---
struct Stats {
    std::atomic<size_t> total_allocs{0};         // 总尝试分配次数
    std::atomic<size_t> successful_allocs{0};    // 成功分配次数
    std::atomic<size_t> failed_allocs{0};        // 失败分配次数
    std::atomic<size_t> total_deallocs{0};       // 总尝试释放次数 (在基准测试计时内)
    std::atomic<long long> total_alloc_latency_ns{0}; // 总分配延迟 (纳秒)
    std::atomic<long long> total_dealloc_latency_ns{0};// 总释放延迟 (纳秒)
    std::atomic<size_t> peak_memory_usage{0};    // 峰值内存使用量 (各线程峰值之和，近似值)
    // std::atomic<size_t> current_memory_usage{0}; // (可选) 追踪瞬时内存使用量，无锁情况下全局不精确

    // 用于计算 P99 延迟
    std::vector<long long> alloc_latencies;       // 存储所有单次分配延迟
    std::vector<long long> dealloc_latencies;     // 存储所有单次释放延迟
    std::mutex latency_mutex;                     // 保护延迟向量在合并时线程安全

    // 存储最终计算结果，避免重复计算和数据丢失
    long long total_duration_ms = 0;              // 基准测试总运行时间 (毫秒)
    double ops_per_sec = 0.0;                     // 每秒操作数 (成功分配 + 成功释放)
    long long p99_alloc_latency_ns = 0;           // P99 分配延迟 (纳秒)
    long long p99_dealloc_latency_ns = 0;         // P99 释放延迟 (纳秒)

    // 清理统计数据，用于开始新的基准测试运行
    void clear() {
        total_allocs = 0;
        successful_allocs = 0;
        failed_allocs = 0;
        total_deallocs = 0;
        total_alloc_latency_ns = 0;
        total_dealloc_latency_ns = 0;
        peak_memory_usage = 0;
        // current_memory_usage = 0; // 如果使用瞬时追踪，也需重置
        alloc_latencies.clear();
        dealloc_latencies.clear();
        total_duration_ms = 0;
        ops_per_sec = 0.0;
        p99_alloc_latency_ns = 0;
        p99_dealloc_latency_ns = 0;
        // 注意: 互斥锁不需要重置
    }
};

// --- 操作类型枚举 ---
enum class OpType { ALLOCATE, DEALLOCATE };

// --- 预生成的操作序列结构 ---
struct Operation {
    OpType type; // 操作类型 (分配或释放)
    size_t size; // 操作大小 (分配时使用)
    // 如果需要更复杂的场景，可以在这里添加对齐要求等信息
};

// --- 工作线程函数模板 ---
// AllocFunc: 分配函数签名 void*(size_t) 或能抛出 std::bad_alloc
// DeallocFunc: 释放函数签名 void(void*, size_t)
template <typename AllocFunc, typename DeallocFunc>
void worker_thread(
    int thread_id,                           // 线程ID
    const std::vector<Operation>& operations,// 该线程要执行的固定操作序列 (常量引用)
    AllocFunc allocate_func,                 // 分配函数 (或函数对象)
    DeallocFunc deallocate_func,             // 释放函数 (或函数对象)
    Stats& global_stats)                     // 全局统计数据 (引用传递，用于更新)
{
    // --- 线程本地状态和统计 ---
    size_t local_allocs = 0;
    size_t local_successful_allocs = 0;
    size_t local_failed_allocs = 0;
    size_t local_deallocs = 0;
    long long local_alloc_latency_ns = 0;
    long long local_dealloc_latency_ns = 0;
    size_t local_current_memory = 0; // 本线程当前分配的总内存
    size_t local_peak_memory = 0;    // 本线程运行期间的峰值内存
    std::vector<long long> local_alloc_latencies_vec; // 本线程的分配延迟记录
    std::vector<long long> local_dealloc_latencies_vec; // 本线程的释放延迟记录
    // 预分配空间以减少重分配开销
    local_alloc_latencies_vec.reserve(operations.size() * ALLOC_PERCENTAGE / 100 + 1); // +1 避免除零或完全没有分配
    local_dealloc_latencies_vec.reserve(operations.size() * (100 - ALLOC_PERCENTAGE) / 100 + 1); // +1

    // 存储本线程当前持有的分配信息 (指针, 大小)
    // 使用 std::list 便于高效地随机移除元素 (模拟真实场景中的任意释放)
    std::list<std::pair<void*, size_t>> allocations;

    // 本地随机数生成器，用于选择要释放的内存块
    std::mt19937 local_rng(RANDOM_SEED + thread_id); // 基于全局种子和线程ID，确保每个线程有不同的但确定的随机序列

    // --- 按预定顺序执行操作 ---
    for (const auto& op : operations) {
        if (op.type == OpType::ALLOCATE) {
            local_allocs++;
            void* ptr = nullptr;
            bool success = false;
            auto alloc_start = std::chrono::high_resolution_clock::now(); // 记录开始时间
            try {
                // 调用传入的分配函数
                ptr = allocate_func(op.size);
                // 如果分配函数在失败时不抛出异常而是返回 nullptr，需要检查
                if (ptr) {
                   success = true;
                }
                 // 如果 allocate_func 抛出 std::bad_alloc，将在下面捕获
            } catch (const std::bad_alloc&) {
                success = false; // 分配失败
                 ptr = nullptr;
                 // 可以根据需要取消注释下面的调试信息
                 // std::cerr << "线程 " << thread_id << ": 捕获到 std::bad_alloc 分配失败！大小：" << op.size << std::endl;
            } catch (const std::exception& e) {
                success = false;
                 ptr = nullptr;
                 // std::cerr << "线程 " << thread_id << ": 分配期间捕获到其他标准异常: " << e.what() << std::endl;
            } catch (...) {
                 success = false;
                 ptr = nullptr;
                // std::cerr << "线程 " << thread_id << ": 分配期间捕获到未知异常！" << std::endl;
            }
            auto alloc_end = std::chrono::high_resolution_clock::now(); // 记录结束时间
            auto latency = std::chrono::duration_cast<std::chrono::nanoseconds>(alloc_end - alloc_start).count();
            local_alloc_latency_ns += latency;        // 累加总延迟
            local_alloc_latencies_vec.push_back(latency); // 记录单次延迟


            if (success && ptr) { // 检查成功标志和指针非空
                local_successful_allocs++;
                allocations.push_back({ptr, op.size}); // 记录分配信息
                local_current_memory += op.size;       // 更新本地内存使用量
                // 更新本地峰值内存
                if (local_current_memory > local_peak_memory) {
                     local_peak_memory = local_current_memory;
                }
                // --- (可选) 更精确的全局峰值追踪，会增加原子操作开销 ---
                // size_t current_global = global_stats.current_memory_usage.fetch_add(op.size, std::memory_order_relaxed) + op.size;
                // size_t current_peak = global_stats.peak_memory_usage.load(std::memory_order_relaxed);
                // while (current_global > current_peak) {
                //     if (global_stats.peak_memory_usage.compare_exchange_weak(current_peak, current_global, std::memory_order_relaxed)) break;
                //     // CAS失败意味着其他线程更新了峰值，重新读取并尝试
                // }
                // --- 结束可选追踪 ---
            } else {
                local_failed_allocs++;
            }
        } else { // DEALLOCATE (释放操作)
            if (!allocations.empty()) { // 确保有东西可释放
                // 随机选择一个已分配的内存块进行释放
                std::uniform_int_distribution<size_t> dist(0, allocations.size() - 1);
                size_t index_to_remove = dist(local_rng);
                auto it = allocations.begin();
                std::advance(it, index_to_remove); // 移动迭代器到随机选择的位置
                void* ptr_to_free = it->first;     // 获取待释放指针
                size_t size_to_free = it->second;  // 获取待释放大小 (某些释放函数需要)

                local_deallocs++;
                auto dealloc_start = std::chrono::high_resolution_clock::now();
                try {
                    deallocate_func(ptr_to_free, size_to_free); // 调用传入的释放函数
                } catch(const std::exception& e) {
                    // 可以选择性地处理释放过程中的异常
                    // std::cerr << "线程 " << thread_id << ": 释放期间捕获到标准异常: " << e.what() << std::endl;
                } catch(...) {
                    // std::cerr << "线程 " << thread_id << ": 释放期间捕获到未知异常！" << std::endl;
                }
                auto dealloc_end = std::chrono::high_resolution_clock::now();
                auto latency = std::chrono::duration_cast<std::chrono::nanoseconds>(dealloc_end - dealloc_start).count();
                local_dealloc_latency_ns += latency;        // 累加总延迟
                local_dealloc_latencies_vec.push_back(latency); // 记录单次延迟

                local_current_memory -= size_to_free; // 更新本地内存使用量
                // global_stats.current_memory_usage.fetch_sub(size_to_free, std::memory_order_relaxed); // 配合可选的全局追踪
                allocations.erase(it); // 从列表中移除已释放的块
            }
            // else: 如果列表为空，尝试释放则忽略 (本次基准测试设计如此)
        }
    } // 操作循环结束

    // --- 清理阶段：释放所有剩余的内存块 ---
    // 注意：此阶段的操作不计入基准测试的计时和延迟统计
    size_t remaining_deallocs_cleanup = 0;
    for (const auto& alloc_info : allocations) {
        try {
             deallocate_func(alloc_info.first, alloc_info.second);
             // global_stats.current_memory_usage.fetch_sub(alloc_info.second, std::memory_order_relaxed); // 配合可选的全局追踪
        } catch (const std::exception& e) {
             // std::cerr << "线程 " << thread_id << ": 清理阶段释放时捕获到标准异常: " << e.what() << std::endl;
        } catch (...) {
            // std::cerr << "线程 " << thread_id << ": 清理阶段释放时捕获到未知异常！" << std::endl;
        }
        remaining_deallocs_cleanup++;
    }
    allocations.clear(); // 清空列表
    // 可以取消注释下面的调试信息
    // if (remaining_deallocs_cleanup > 0) {
    //     std::cout << "线程 " << thread_id << " 清理了 " << remaining_deallocs_cleanup << " 个剩余分配。" << std::endl;
    // }
     if (local_current_memory != 0 && remaining_deallocs_cleanup > 0) {
         // 如果清理后内存计数不为零，可能表示清理时发生异常或计算错误
         // std::cerr << "线程 " << thread_id << " 警告: 清理后 local_current_memory 值为 " << local_current_memory << std::endl;
     }


    // --- 原子地更新全局统计数据 ---
    // 使用 memory_order_relaxed 因为这些是独立的计数器，不需要强制的顺序约束
    global_stats.total_allocs.fetch_add(local_allocs, std::memory_order_relaxed);
    global_stats.successful_allocs.fetch_add(local_successful_allocs, std::memory_order_relaxed);
    global_stats.failed_allocs.fetch_add(local_failed_allocs, std::memory_order_relaxed);
    // 决定是否将清理阶段的释放计入总数。这里我们只统计基准测试计时内的释放。
    global_stats.total_deallocs.fetch_add(local_deallocs, std::memory_order_relaxed);
    global_stats.total_alloc_latency_ns.fetch_add(local_alloc_latency_ns, std::memory_order_relaxed);
    global_stats.total_dealloc_latency_ns.fetch_add(local_dealloc_latency_ns, std::memory_order_relaxed);

    // 更新全局峰值内存使用量 (近似值：所有线程各自峰值之和)
    // 这个值反映了整个运行期间各个线程造成的总内存压力，
    // 而不是所有线程在某一时刻同时达到的最大内存总和。
    global_stats.peak_memory_usage.fetch_add(local_peak_memory, std::memory_order_relaxed);

    // --- 合并延迟数据到全局向量 (需要加锁保护) ---
    {
        std::lock_guard<std::mutex> lock(global_stats.latency_mutex); // RAII 锁
        // 将本地延迟向量的数据追加到全局向量
        global_stats.alloc_latencies.insert(global_stats.alloc_latencies.end(),
                                            local_alloc_latencies_vec.begin(), local_alloc_latencies_vec.end());
        global_stats.dealloc_latencies.insert(global_stats.dealloc_latencies.end(),
                                              local_dealloc_latencies_vec.begin(), local_dealloc_latencies_vec.end());
    } // 锁在此处自动释放
}

// --- 辅助函数：计算 P99 延迟 ---
// 输入的向量会被修改 (排序)
long long calculate_p99_latency(std::vector<long long>& latencies) {
    if (latencies.empty()) {
        return 0; // 没有数据则返回 0
    }
    // 直接对传入的向量进行排序 (如果后续不需要原始顺序)
    // 如果需要保留原始顺序，应该先复制向量再排序
    std::sort(latencies.begin(), latencies.end());
    // 计算 P99 位置的索引
    size_t index = static_cast<size_t>(latencies.size() * 0.99);
    // 确保索引有效，特别是在样本量很小的情况下 (至少为0，不超过 size()-1)
    index = std::min(index, latencies.size() - 1);
    return latencies[index];
}

// --- 运行基准测试函数模板 ---
template <typename AllocFunc, typename DeallocFunc>
void run_benchmark(const std::string& name,                           // 基准测试名称
                   const std::vector<std::vector<Operation>>& ops_per_thread, // 每个线程的操作序列
                   AllocFunc allocate_func,                            // 分配函数
                   DeallocFunc deallocate_func,                        // 释放函数
                   Stats& stats)                                       // 统计结果对象 (引用传递)
{
    std::cout << "\n--- 开始运行基准测试: " << name << " ---" << std::endl;
    std::cout << "线程数: " << NUM_THREADS
              << ", 每个线程的操作数: " << NUM_OPERATIONS_PER_THREAD << std::endl;

    stats.clear(); // 确保每次运行前清空统计数据
    std::vector<std::thread> threads; // 存储线程对象
    threads.reserve(NUM_THREADS);     // 预分配空间

    auto benchmark_start_time = std::chrono::high_resolution_clock::now(); // 记录基准测试开始时间

    // --- 创建并启动工作线程 ---
    for (unsigned int i = 0; i < NUM_THREADS; ++i) {
        // 使用 emplace_back 直接在 vector 中构造线程对象，避免拷贝
        threads.emplace_back(worker_thread<AllocFunc, DeallocFunc>, // 线程函数模板实例
                             i,                                  // 线程 ID
                             std::cref(ops_per_thread[i]),       // 操作序列 (常量引用)
                             allocate_func,                      // 分配函数
                             deallocate_func,                    // 释放函数
                             std::ref(stats));                   // 全局统计对象 (引用包装器)
    }

    // --- 等待所有线程完成 ---
    for (auto& t : threads) {
        if (t.joinable()) { // 确保线程是可加入的
            t.join();       // 等待线程结束
        }
    }

    auto benchmark_end_time = std::chrono::high_resolution_clock::now(); // 记录基准测试结束时间
    auto total_duration = std::chrono::duration_cast<std::chrono::milliseconds>(benchmark_end_time - benchmark_start_time);
    stats.total_duration_ms = total_duration.count(); // 存储总时长

    // --- 计算并打印统计结果 ---
    size_t successful_allocs_count = stats.successful_allocs.load();
    size_t total_deallocs_count = stats.total_deallocs.load();
    size_t total_ops_executed = successful_allocs_count + total_deallocs_count; // 成功执行的操作总数
    size_t total_ops_attempted = stats.total_allocs.load() + total_deallocs_count; // 尝试执行的操作总数 (假设每次dealloc都尝试了)

    // 计算 Ops/Sec (每秒操作数)
    stats.ops_per_sec = (total_ops_executed == 0 || stats.total_duration_ms == 0) ? 0.0 :
                      (static_cast<double>(total_ops_executed) * 1000.0 / stats.total_duration_ms);

    // 计算平均延迟 (微秒)
    double avg_alloc_latency_us = (successful_allocs_count == 0) ? 0.0 :
        (static_cast<double>(stats.total_alloc_latency_ns.load()) / successful_allocs_count / 1000.0);
    double avg_dealloc_latency_us = (total_deallocs_count == 0) ? 0.0 :
        (static_cast<double>(stats.total_dealloc_latency_ns.load()) / total_deallocs_count / 1000.0);

    // 计算 P99 延迟 (会修改 stats 中的延迟向量)
    stats.p99_alloc_latency_ns = calculate_p99_latency(stats.alloc_latencies);
    stats.p99_dealloc_latency_ns = calculate_p99_latency(stats.dealloc_latencies);
    // 转换为微秒
    double p99_alloc_latency_us = static_cast<double>(stats.p99_alloc_latency_ns) / 1000.0;
    double p99_dealloc_latency_us = static_cast<double>(stats.p99_dealloc_latency_ns) / 1000.0;

    // 设置输出格式
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "总耗时:               " << stats.total_duration_ms << " ms" << std::endl;
    std::cout << "总尝试操作数:         " << total_ops_attempted << std::endl;
    std::cout << "总成功操作数:         " << total_ops_executed << std::endl;
    std::cout << "每秒操作数 (Ops/Sec): " << stats.ops_per_sec << std::endl;
    std::cout << "成功分配次数:         " << successful_allocs_count << std::endl;
    std::cout << "失败分配次数:         " << stats.failed_allocs.load() << std::endl;
    std::cout << "成功释放次数:         " << total_deallocs_count << std::endl;
    std::cout << "平均分配延迟:         " << avg_alloc_latency_us << " us" << std::endl;
    std::cout << "P99 分配延迟:         " << p99_alloc_latency_us << " us" << std::endl;
    std::cout << "平均释放延迟:         " << avg_dealloc_latency_us << " us" << std::endl;
    std::cout << "P99 释放延迟:         " << p99_dealloc_latency_us << " us" << std::endl;
    // 注意: 峰值内存使用量是各线程内部峰值的累加，是总内存压力的近似估计，
    //       可能高于系统在任一时刻的实际峰值内存占用。
    std::cout << "峰值内存 (线程峰值和):" << (static_cast<double>(stats.peak_memory_usage.load()) / 1024.0 / 1024.0) << " MB" << std::endl;
    std::cout << "--- 基准测试结束: " << name << " ---" << std::endl;
}

// --- 主函数 ---
int main() {
    std::cout << "启动高并发内存分配器基准测试..." << std::endl;
    std::cout << "使用 C++23 标准特性。" << std::endl;
    std::cout << "======================================================" << std::endl;
    std::cout << "当前基准测试配置:" << std::endl;
    std::cout << "  线程数 (NUM_THREADS):              " << NUM_THREADS << std::endl;
    std::cout << "  每线程操作数 (NUM_OPERATIONS):   " << NUM_OPERATIONS_PER_THREAD << std::endl;
    std::cout << "  分配操作比例 (ALLOC_PERCENTAGE): " << ALLOC_PERCENTAGE << "%" << std::endl;
    std::cout << "  最小分配大小 (MIN_ALLOC_SIZE):     " << MIN_ALLOC_SIZE << " B" << std::endl;
    std::cout << "  最大分配大小 (MAX_ALLOC_SIZE):     " << MAX_ALLOC_SIZE << " B" << std::endl;
    std::cout << "  随机种子 (RANDOM_SEED):            " << RANDOM_SEED << std::endl;
    std::cout << "  PMR对齐要求 (DEFAULT_ALIGNMENT): " << DEFAULT_ALIGNMENT << " B" << std::endl;
    std::cout << "======================================================" << std::endl;

    // --- 1. 生成确定性的操作序列 ---
    std::cout << "正在为 " << NUM_THREADS << " 个线程生成每个线程 "
              << NUM_OPERATIONS_PER_THREAD << " 个操作..." << std::endl;
    std::vector<std::vector<Operation>> ops_per_thread(NUM_THREADS);
    std::mt19937 master_rng(RANDOM_SEED); // 主随机数生成器，用于生成操作序列
    std::uniform_int_distribution<size_t> size_dist(MIN_ALLOC_SIZE, MAX_ALLOC_SIZE); // 分配大小分布
    std::uniform_int_distribution<int> op_dist(1, 100); // 操作类型分布 (1-100)

    for (unsigned int i = 0; i < NUM_THREADS; ++i) {
        ops_per_thread[i].reserve(NUM_OPERATIONS_PER_THREAD);
        // 使用主生成器确保整个操作集是确定的
        // 如果需要每个线程有独立的随机性（但不易复现），可以为每个线程创建单独的生成器
        for (size_t j = 0; j < NUM_OPERATIONS_PER_THREAD; ++j) {
            Operation op;
            op.size = size_dist(master_rng); // 随机生成大小
            if (op_dist(master_rng) <= ALLOC_PERCENTAGE) {
                op.type = OpType::ALLOCATE; // 按比例决定是分配还是释放
            } else {
                op.type = OpType::DEALLOCATE;
                // 释放操作的大小在工作线程中会根据记录的分配信息确定，这里生成的大小会被忽略
            }
            ops_per_thread[i].push_back(op);
        }
    }
    std::cout << "操作序列生成完毕。" << std::endl;


    // --- 2. 定义分配器接口的包装器 ---

    // 自定义 memory_pool 的包装器
    // 假设 memory_pool::memory_pool::allocate 返回 std::optional<void*> 或 void*
    // 并且在失败时返回空 optional 或 nullptr (或者抛出异常，由 worker 捕获)
    auto memory_pool_alloc = [](size_t size) -> void* {
        try {
            // 注意: 这里假设你的 memory_pool 实现是 memory_pool::memory_pool::allocate
            // 如果是 namespace memory_pool { void* allocate(...); }
            // 则应该是 memory_pool::allocate(size);
            auto result = memory_pool::memory_pool::allocate(size);

            // 根据实际返回类型调整 (示例：如果返回 optional)
            // if constexpr (std::is_same_v<decltype(result), std::optional<void*>>) {
            //     return result.value_or(nullptr);
            // } else {
                 // 假设它返回 void*，并且可能通过抛出异常或返回nullptr来表示失败
                 // 如果是 optional<T*>.value()，失败时也会抛异常
                 if (result.has_value()) return result.value();
                 else return nullptr; // 假设 optional 为空表示失败
            // }
        } catch (const std::bad_alloc&) {
            // 如果内存池内部实现可能抛出 bad_alloc，在这里捕获并返回 nullptr
            // 或者让 worker_thread 中的 catch 捕获
             throw; // 重新抛出，让 worker 捕获并统计
        } catch (...) {
            // 处理其他可能的异常
            // std::cerr << "Custom pool allocation wrapper caught unknown exception." << std::endl;
            throw; // 重新抛出
        }
    };
    auto memory_pool_dealloc = [](void* p, size_t s) {
        // 同样，确认接口是否为 memory_pool::memory_pool::deallocate
        memory_pool::memory_pool::deallocate(p, s);
    };

    // 标准 malloc/free 的包装器
    auto malloc_alloc = [](size_t size) -> void* {
        return malloc(size); // 失败时返回 nullptr
    };
    auto malloc_dealloc = [](void* p, size_t /* ignored_size */) {
        free(p); // free 不需要大小参数
    };

    // C++23 std::pmr::synchronized_pool_resource 的包装器
    // 创建资源实例。它管理内部池，并在池耗尽或分配大块内存时使用上游资源 (默认是 new/delete)
    std::pmr::synchronized_pool_resource pmr_pool_resource; // 使用默认选项

    auto pmr_alloc = [&](size_t size) -> void* {
        // PMR 的 allocate 在失败时会抛出 std::bad_alloc
        return pmr_pool_resource.allocate(size, DEFAULT_ALIGNMENT);
    };
    auto pmr_dealloc = [&](void* p, size_t size) {
        // PMR 的 deallocate 需要大小和对齐参数
        pmr_pool_resource.deallocate(p, size, DEFAULT_ALIGNMENT);
    };


    // --- 3. 运行各个基准测试 ---
    // 运行自定义内存池基准测试
    Stats pool_stats;
    run_benchmark("自定义内存池 (Custom Memory Pool)", ops_per_thread, memory_pool_alloc, memory_pool_dealloc, pool_stats);

    // 运行标准 malloc/free 基准测试
    Stats malloc_stats;
    run_benchmark("标准库 malloc/free", ops_per_thread, malloc_alloc, malloc_dealloc, malloc_stats);

    // 运行 C++ PMR 同步池资源基准测试
    Stats pmr_stats;
    run_benchmark("标准库 std::pmr::synchronized_pool_resource", ops_per_thread, pmr_alloc, pmr_dealloc, pmr_stats);


    // --- 4. 对比结果 ---
    std::cout << "\n--- 基准测试结果对比 ---" << std::endl;
    std::cout << std::fixed << std::setprecision(2); // 设置浮点数输出精度

    // 定义表格列宽
    const int name_w = 32; // 指标名称列宽
    const int val_w = 18;  // 数据值列宽

    // 打印表头
    std::cout << std::left << std::setw(name_w) << "指标" << " | "
              << std::right << std::setw(val_w) << "自定义内存池" << " | "
              << std::right << std::setw(val_w) << "malloc/free" << " | "
              << std::right << std::setw(val_w) << "std::pmr::sync" << " |" << std::endl;
    // 打印分隔线
    std::cout << std::string(name_w, '-') << "-|-"
              << std::string(val_w, '-') << "-|-"
              << std::string(val_w, '-') << "-|-"
              << std::string(val_w, '-') << "-|" << std::endl;

    // 辅助函数，用于打印一行数据
    // 使用 lambda 表达式简化代码
    auto print_row_double = [&](const std::string& metric_name,
                                double pool_val, double malloc_val, double pmr_val,
                                const std::string& unit = "") {
         std::cout << std::left << std::setw(name_w) << metric_name << " | "
                   << std::right << std::setw(val_w) << std::fixed << std::setprecision(2) << pool_val << unit << " | "
                   << std::right << std::setw(val_w) << std::fixed << std::setprecision(2) << malloc_val << unit << " | "
                   << std::right << std::setw(val_w) << std::fixed << std::setprecision(2) << pmr_val << unit << " |" << std::endl;
    };
    auto print_row_size = [&](const std::string& metric_name,
                              size_t pool_val, size_t malloc_val, size_t pmr_val,
                              const std::string& unit = "") {
         std::cout << std::left << std::setw(name_w) << metric_name << " | "
                   << std::right << std::setw(val_w) << pool_val << unit << " | "
                   << std::right << std::setw(val_w) << malloc_val << unit << " | "
                   << std::right << std::setw(val_w) << pmr_val << unit << " |" << std::endl;
    };

    // 打印对比数据行
    print_row_double("每秒操作数 (Ops/Sec,越高越好)", pool_stats.ops_per_sec, malloc_stats.ops_per_sec, pmr_stats.ops_per_sec);
    std::cout << std::string(name_w, '-') << "-|-" << std::string(val_w, '-') << "-|-" << std::string(val_w, '-') << "-|-" << std::string(val_w, '-') << "-|" << std::endl; // 分隔符

    print_row_double("平均分配延迟 (us, 越低越好)",
                     (pool_stats.successful_allocs.load() == 0) ? 0.0 : (static_cast<double>(pool_stats.total_alloc_latency_ns.load()) / pool_stats.successful_allocs.load() / 1000.0),
                     (malloc_stats.successful_allocs.load() == 0) ? 0.0 : (static_cast<double>(malloc_stats.total_alloc_latency_ns.load()) / malloc_stats.successful_allocs.load() / 1000.0),
                     (pmr_stats.successful_allocs.load() == 0) ? 0.0 : (static_cast<double>(pmr_stats.total_alloc_latency_ns.load()) / pmr_stats.successful_allocs.load() / 1000.0));

    print_row_double("P99 分配延迟 (us, 越低越好)",
                     static_cast<double>(pool_stats.p99_alloc_latency_ns) / 1000.0,
                     static_cast<double>(malloc_stats.p99_alloc_latency_ns) / 1000.0,
                     static_cast<double>(pmr_stats.p99_alloc_latency_ns) / 1000.0);

    print_row_double("平均释放延迟 (us, 越低越好)",
                     (pool_stats.total_deallocs.load() == 0) ? 0.0 : (static_cast<double>(pool_stats.total_dealloc_latency_ns.load()) / pool_stats.total_deallocs.load() / 1000.0),
                     (malloc_stats.total_deallocs.load() == 0) ? 0.0 : (static_cast<double>(malloc_stats.total_dealloc_latency_ns.load()) / malloc_stats.total_deallocs.load() / 1000.0),
                     (pmr_stats.total_deallocs.load() == 0) ? 0.0 : (static_cast<double>(pmr_stats.total_dealloc_latency_ns.load()) / pmr_stats.total_deallocs.load() / 1000.0));

    print_row_double("P99 释放延迟 (us, 越低越好)",
                     static_cast<double>(pool_stats.p99_dealloc_latency_ns) / 1000.0,
                     static_cast<double>(malloc_stats.p99_dealloc_latency_ns) / 1000.0,
                     static_cast<double>(pmr_stats.p99_dealloc_latency_ns) / 1000.0);
     std::cout << std::string(name_w, '-') << "-|-" << std::string(val_w, '-') << "-|-" << std::string(val_w, '-') << "-|-" << std::string(val_w, '-') << "-|" << std::endl; // 分隔符

    print_row_double("峰值内存 (MB, 线程峰值和)",
                     pool_stats.peak_memory_usage.load() / 1024.0 / 1024.0,
                     malloc_stats.peak_memory_usage.load() / 1024.0 / 1024.0,
                     pmr_stats.peak_memory_usage.load() / 1024.0 / 1024.0);

    print_row_size("成功分配次数", pool_stats.successful_allocs.load(), malloc_stats.successful_allocs.load(), pmr_stats.successful_allocs.load());
    print_row_size("失败分配次数", pool_stats.failed_allocs.load(), malloc_stats.failed_allocs.load(), pmr_stats.failed_allocs.load());
    print_row_size("成功释放次数", pool_stats.total_deallocs.load(), malloc_stats.total_deallocs.load(), pmr_stats.total_deallocs.load());

    // 打印表格结束线
    std::cout << std::string(name_w + 3 + val_w + 3 + val_w + 3 + val_w + 2, '-') << std::endl;
    std::cout << "注意: Ops/Sec 来自各基准测试的实际运行时间。延迟越低越好。" << std::endl;
    std::cout << "     峰值内存是所有线程各自内部峰值内存使用量的总和（近似值）。" << std::endl;
    std::cout << "======================================================" << std::endl;


    return 0; // 程序正常退出
}