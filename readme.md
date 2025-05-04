# memory_pool 内存池项目详解

> 为了防止因本人的表达能力不够而导致看不懂本项目，因此该readme在我本人草稿的基本上，使用了 Gemini 2.5 Pro Preview 03-25 模型进行润色与重新表达

本项目是一个基于C++实现的高性能内存池，旨在减少频繁调用系统内存分配（如 `malloc`/`free` 或 `new`/`delete`）带来的开销，特别是在多线程环境中。它采用了经典的三层缓存结构设计。

参考项目： https://github.com/youngyangyang04/memory-pool
参考文档：https://blog.csdn.net/m0_62782700/article/details/135443352

## 核心特点

*   **三层缓存结构：** 线程缓存 (Thread Cache)、中心缓存 (Central Cache)、页缓存 (Page Cache)，逐层降低锁竞争，提高分配效率。
*   **小对象优化：** 主要针对小于等于 `16KB` 的小对象进行缓存优化。
*   **对齐保证：** 分配的内存地址按 `sizeof(void*)` (通常是8字节) 对齐。
*   **页管理：** 支持从操作系统按页 (`4KB`) 申请内存，并缓存空闲页。
*   **页合并：** 回收页时，会自动尝试与前后相邻的空闲页合并，减少内存碎片。
*   **动态调整：** 线程缓存向中心缓存请求内存时，会根据历史使用情况动态调整一次请求的内存块数量。v2版本在Release模式下，中心缓存向页缓存请求页的数量也会动态调整。
*   **并发控制：** 在中心缓存和页缓存层使用锁（`std::atomic_flag` 自旋锁或 `std::mutex`）保证线程安全。
*   **大对象直通：** 大于 `16KB` 的内存请求会绕过缓存，直接使用 `malloc`/`free` (v1, v2 页缓存实现) 或 `mmap`/`munmap` (v2 页缓存实现，虽然代码里写的是 `malloc`/`free`, 但注释和通常实践倾向于 `mmap`)。

## 版本说明

本项目包含 `v1` 和 `v2` 两个版本。

*   **v1 版本：** 实现了内存池的核心架构和逻辑。使用了相对直观的数据结构（如 `std::list` 管理空闲内存块），**推荐新手从此版本开始阅读**，以理解内存池的基本工作原理和设计思想。
*   **v2 版本：** 在 v1 的基础上进行了性能优化。主要改动包括：
    *   使用**侵入式链表**（`std::byte*` 指针）替代 `std::list` 来管理空闲内存块，减少了额外节点分配的开销和内存占用，提高了缓存局部性。
    *   优化了 `page_span` 的实现，在 Release 模式下使用简单的计数器替代 `std::bitset`，减少开销。Debug 模式下保留 `std::bitset` 用于校验。
    *   引入了 `atomic_flag_guard` 实现 RAII 风格的锁管理。
    *   在 Release 模式下，中心缓存向页缓存请求内存的页面数量也实现了动态调整。

**建议学习路径：** 先理解 v1 版本的整体架构和各个组件的职责与交互，再学习 v2 版本是如何在保持核心架构不变的前提下进行性能优化的。

## 项目架构

内存池采用三层架构，如下图所示：

```
+-------------------+      +-------------------+      +-------------------+      +-----------------+
|     用户代码       | ---> |   Thread Cache    | ---> |   Central Cache   | ---> |    Page Cache   | ---> 操作系统内存
| (Allocate/Deallocate)|      | (每个线程独有)    |      | (所有线程共享)    |      | (所有线程共享)    |      (mmap/munmap)
+-------------------+      +-------------------+      +-------------------+      +-----------------+
      ^       |                  ^       |                  ^       |                  ^       |
      |-------| (直接返回缓存)     |-------| (批量获取/归还)  |-------| (按页获取/归还)  |-------|

```

1.  **线程缓存 (Thread Cache):**
    *   **目标：** 无锁（线程独享），快速满足小内存请求。
    *   **职责：** 每个线程持有一个独立的 Thread Cache。当线程请求内存时，优先从自己的 Thread Cache 获取。当 Thread Cache 没有足够内存或缓存过多时，会与 Central Cache 进行批量交互。
    *   **优点：** 绝大多数内存分配/释放在本线程内完成，无锁竞争，速度极快。
2.  **中心缓存 (Central Cache):**
    *   **目标：** 作为 Thread Cache 的上一级，平衡不同 Thread Cache 的内存使用，减少与 Page Cache 的交互。
    *   **职责：** 所有线程共享一个 Central Cache。当 Thread Cache 需要内存时，向 Central Cache 批量申请。当 Thread Cache 归还内存时，也批量归还给 Central Cache。Central Cache 需要管理不同大小规格的空闲内存块列表。当自身内存不足时，向 Page Cache 申请。当回收的内存块可以凑成完整的页时，归还给 Page Cache。
    *   **并发：** 需要加锁（本项目使用 `std::atomic_flag` 自旋锁，按内存规格分别加锁，降低冲突）。
3.  **页缓存 (Page Cache):**
    *   **目标：** 管理以页（Page）为单位的大块内存，作为 Central Cache 的后备，负责与操作系统交互。
    *   **职责：** 所有线程共享一个 Page Cache。它按页向操作系统申请内存（如使用 `mmap`），并将大块内存（Span）按需切分给 Central Cache。回收来自 Central Cache 的页时，会尝试合并相邻的空闲页，以减少碎片。
    *   **并发：** 需要加锁（本项目使用 `std::mutex`）。

---

## v1 版本详解

### 1. 工具类 (Utility Classes)

这些类为内存池的核心组件提供基础支持。

*   **`memory_span`:**
    *   **作用：** 类似于 `std::span<std::byte>`，用于表示一段连续的内存区域。它包含一个起始地址 `m_data` (`std::byte*`) 和一个大小 `m_size` (`std::size_t`)。
    *   **设计原因：**
        *   封装了原始指针和大小，方便管理和传递内存块。
        *   重载了比较运算符 (`<=>`, `==`)，使其可以作为 `std::map` 或 `std::set` 的键或元素，这对于 `page_cache` 中的页面管理至关重要（需要根据地址排序和查找）。
        *   提供了 `subspan` 方法，方便地将一个大的 `memory_span` 切割成小的 `memory_span`，这在从 Page Cache 获取页面并切割成小块时非常有用。

*   **`size_utils`:**
    *   **作用：** 定义内存池中使用到的常量和辅助函数。
    *   **关键成员：**
        *   `ALIGNMENT`: 内存对齐值，通常为 `sizeof(void*)` (8字节)。所有分配的内存大小都会向上对齐到这个值的倍数。 **设计原因：** 保证返回的内存地址符合硬件要求，避免对齐问题，同时也方便管理，将内存请求归类到固定的“桶”中。最小分配单元也是 `ALIGNMENT`。
        *   `PAGE_SIZE`: 页大小，通常为 `4096` 字节 (4KB)。这是 Page Cache 与操作系统交互的基本单位。**设计原因：** 与操作系统内存管理单位保持一致，提高效率。
        *   `MAX_CACHED_UNIT_SIZE`: 内存池主要优化（缓存）的对象大小上限，本项目设为 `16KB`。**设计原因：** 区分小对象和“大”对象。大于此值的对象被认为是“大”对象，通常直接向系统申请，不经过 Thread Cache 和 Central Cache 的复杂缓存逻辑，避免缓存管理成本超过收益。
        *   `CACHE_LINE_SIZE`: **这个命名可能有点误导(本人备注：想的是LINE的个数，就取名为LINE_SIZE)，它实际代表的是 Central Cache 和 Thread Cache 中自由链表数组的大小**，计算方式是 `MAX_CACHED_UNIT_SIZE / ALIGNMENT`。这个值决定了我们要管理多少个不同规格大小的内存块的“桶”（free list）。例如，如果 `MAX_CACHED_UNIT_SIZE` 是 16KB，`ALIGNMENT` 是 8B，那么 `CACHE_LINE_SIZE` 就是 2048。这意味着我们需要 2048 个桶，分别管理 8B, 16B, 24B, ..., 16KB 大小的内存块。
        *   `align(size)`: 将给定大小 `size` 向上对齐到 `ALIGNMENT` 的倍数。
        *   `get_index(size)`: 根据对齐后的大小计算它应该属于哪个自由链表（桶）的索引。计算方式是 `align(size) / ALIGNMENT - 1`。例如，请求 10B，对齐后是 16B，索引是 `16 / 8 - 1 = 1`。请求 8B，对齐后是 8B，索引是 `8 / 8 - 1 = 0`。

*   **`page_span` (v1 实现):**

```cpp
    class page_span {
        // ... (构造函数, 比较运算符等)
        static constexpr size_t MAX_UNIT_COUNT = size_utils::PAGE_SIZE / size_utils::ALIGNMENT; // 512 for 4KB page / 8B align
        bool is_empty();
        void allocate(memory_span memory);
        void deallocate(memory_span memory);
        bool is_valid_unit_span(memory_span memory);
        memory_span get_memory_span(); // 获取管理的整块内存
    private:
        const memory_span m_memory;    // 管理的从 Page Cache 获取的整块内存
        const size_t m_unit_size;      // 切分后每个小内存块的大小
        std::bitset<MAX_UNIT_COUNT> m_allocated_map; // 位图，标记哪些小块被分配出去了
    };
```

*   **作用：** 用于 Central Cache 管理从 Page Cache 获取的一大块内存 (`m_memory`)。这块大内存会被切分成多个 `m_unit_size` 大小的单元。`page_span` 负责跟踪这些单元的分配状态。
*   **设计原因 (v1)：**
    *   `m_allocated_map`: 使用 `std::bitset` 来跟踪每个小单元的分配状态。位图的索引对应小单元在 `m_memory` 中的偏移量。`1` 表示已分配给 Thread Cache，`0` 表示空闲（在 Central Cache 的自由链表中）。
    *   **优点：** 在 Debug 模式下非常有用。`allocate` 和 `deallocate` 操作可以通过检查 `bitset` 中对应位的值来断言（`assert`）内存块是否被重复分配或重复释放，有助于调试和保证正确性。`is_empty()` 可以快速判断是否所有小单元都已回收（`bitset.none()`）。
    *   **缺点：** `std::bitset` 的大小是编译时固定的 (`MAX_UNIT_COUNT`)。这意味着 Central Cache 从 Page Cache 申请的一块内存，最多只能管理 `MAX_UNIT_COUNT` 个小单元。如果 `m_memory.size() / m_unit_size` 大于 `MAX_UNIT_COUNT`，多余的部分就无法被这个 `page_span` 管理，造成浪费（尽管本项目实现中似乎总是申请能放下 `MAX_UNIT_COUNT` 个单元的页面数）。同时，即使在 Release 模式下，维护 `bitset` 也有一定的开销。


### 2. 页缓存 (Page Cache - `page_cache`)

*   **职责：** 内存池的最底层，负责向操作系统申请/释放大块内存（按页），管理这些大块内存（称为 Span），并将它们按需提供给 Central Cache。处理超过 `MAX_CACHED_UNIT_SIZE` 的大对象分配。

*   **核心数据结构：**

```cpp
    class page_cache {
        // ... (接口函数: allocate_page, deallocate_page, allocate_unit, deallocate_unit, stop)
    private:
        std::map<size_t, std::set<memory_span>> free_page_store; // 按页数存储空闲 span
        std::map<std::byte*, memory_span> free_page_map;         // 按起始地址存储空闲 span
        std::vector<memory_span> page_vector;                    // 记录所有从系统申请的内存，用于最终释放
        bool m_stop = false;
        std::mutex m_mutex;                                     // 控制并发访问
    };
```

*   `free_page_store`: `map<页数, set<对应页数的空闲span>>`。
    *   **设计原因：** 当 Central Cache 请求 `N` 页内存时，可以快速查找：
        1.  是否存在正好 `N` 页的空闲 span (`free_page_store[N]`)。
        2.  如果不存在，则查找是否存在大于 `N` 页的空闲 span（使用 `map::lower_bound(N)` 找到第一个页数 >= N 的条目）。
    *   **为何用 `map` 而非 `array`?** 如你草稿所说，`map` 的 `lower_bound` 提供了 `O(log M)` (M为不同空闲页数的种类数) 的查找效率，而 `array` 需要线性扫描 `O(MaxPages)`。对于可能存在各种大小空闲页的情况，`map` 更高效。`set` 用于存储相同页数的多个 span，并按地址排序。
*   `free_page_map`: `map<起始地址, span>`。
    *   **设计原因：** 用于快速合并空闲页。当回收一个 span 时：
        1.  可以通过 `map::upper_bound(span.data())` 找到地址刚好在回收 span *之后* 的空闲 span。检查它是否紧邻回收 span 的尾部，如果是则合并。
        2.  可以通过 `upper_bound` 找到地址在回收 span 之后的第一个，然后 `--it` (如果 `it != begin()`) 得到地址刚好在回收 span *之前* 的空闲 span。检查它的尾部是否紧邻回收 span 的头部，如果是则合并。
    *   `map` 的有序性使得这种基于地址的邻近查找非常高效 (`O(log F)`, F为空闲 span 总数)。
*   `page_vector`: 存储所有通过 `system_allocate_memory` (内部调用 `mmap`) 从操作系统获取的内存块信息。**设计原因：** 确保在内存池析构时（或调用 `stop()` 时），能够将所有申请的内存通过 `system_deallocate_memory` (`munmap`) 归还给操作系统，防止内存泄漏。
*   `m_mutex`: 由于 Page Cache 是所有线程共享的，其内部数据结构的操作（查找、插入、删除、合并 span）必须是原子的，因此需要互斥锁来保护。

*   **分配逻辑 (`allocate_page`)**:
    1.  加锁。
    2.  在 `free_page_store` 中查找足够大的空闲 span (优先精确匹配，其次找更大的)。
    3.  如果找到，则从 `free_page_store` 和 `free_page_map` 中移除该 span。如果找到的 span 比请求的大，切割出所需部分返回，剩余部分重新插入两个 map 中。
    4.  如果没找到，调用 `system_allocate_memory` 向操作系统申请一大块内存（至少 `PAGE_ALLOCATE_COUNT` 页或请求页数，取较大者）。将申请到的内存记录到 `page_vector`。切割出所需部分返回，如果还有剩余，将剩余部分插入两个 map。
    5.  解锁。

*   **回收逻辑 (`deallocate_page`)**:
    1.  加锁。
    2.  尝试与前、后相邻的空闲 span 合并（利用 `free_page_map` 进行查找）。如果合并成功，要从 `free_page_store` 和 `free_page_map` 中移除被合并的旧 span。
    3.  将最终（可能合并后的）span 插入 `free_page_store` 和 `free_page_map`。
    4.  解锁。

*   **大对象处理 (`allocate_unit`/`deallocate_unit`)**: 直接调用 `malloc`/`free` (或 `mmap`/`munmap` conceptually) 处理大于 `MAX_CACHED_UNIT_SIZE` 的请求，不经过缓存。

### 3. 中心缓存 (Central Cache - `central_cache`)

*   **职责：** 作为 Thread Cache 和 Page Cache 的桥梁。管理按 `size_utils::CACHE_LINE_SIZE` 个不同规格大小划分的空闲内存块列表（Free List）。响应 Thread Cache 的批量内存请求，如果自身列表为空，则向 Page Cache 申请新的页（Span），并将其切分成小块放入对应 Free List。接收 Thread Cache 归还的内存块，并判断其所属的 `page_span` 是否已经完全空闲，如果是，则将整个 `page_span` 对应的内存归还给 Page Cache。

*   **核心数据结构 (v1):**

```cpp
    class central_cache {
        // ... (接口函数: allocate, deallocate)
    private:
        std::array<std::list<memory_span>, size_utils::CACHE_LINE_SIZE> m_free_array; // 按规格大小组织的空闲块列表
        std::array<std::atomic_flag, size_utils::CACHE_LINE_SIZE> m_status;          // 每个规格列表一个自旋锁
        std::array<std::map<std::byte*, page_span>, size_utils::CACHE_LINE_SIZE> m_page_set; // 按规格管理 page_span
    };
```

*   `m_free_array`: `array<list<memory_span>, N>`，`N` 是 `CACHE_LINE_SIZE`。数组的索引 `i` 对应大小为 `(i+1) * ALIGNMENT` 的内存块。`m_free_array[i]` 是一个 `std::list`，存储着所有当前空闲的、大小为 `(i+1) * ALIGNMENT` 的 `memory_span`。
    *   **设计原因：** 使用 `std::array` 提供 O(1) 时间访问特定大小的自由链表。使用 `std::list` 管理空闲块，插入和删除（通常在头部）是 O(1)。
*   `m_status`: `array<atomic_flag, N>`。每个规格的自由链表 (`m_free_array[i]`) 对应一个 `atomic_flag` 自旋锁。
    *   **设计原因：** 实现分桶锁（细粒度锁）。不同大小规格的内存分配/回收可以并行进行，只有当多个线程同时操作 *相同大小* 的内存块时才会发生锁竞争，提高了并发性能。相比于对整个 Central Cache 使用一个 `std::mutex`，这种方式冲突概率更低。`atomic_flag` 通常比 `mutex` 更轻量，适用于短时间的锁定。
*   `m_page_set`: `array<map<起始地址, page_span>, N>`。同样按内存规格 `i` 分桶。`m_page_set[i]` 是一个 `map`，存储了所有用于生成大小为 `(i+1) * ALIGNMENT` 内存块的 `page_span` 信息，按 `page_span` 的起始地址排序。
    *   **设计原因：** 当回收一个大小为 `S`（对应索引 `i`）的 `memory_span` 时，需要知道它属于哪个 `page_span`，以便在该 `page_span` 中将其标记为已回收（调用 `page_span::deallocate`）。利用 `map` 的 `upper_bound` 特性，可以快速（`O(log P)`, P 为该规格下的 page_span 数量）找到包含给定 `memory_span` 地址的 `page_span`（查找 `upper_bound(memory_span.data())`，然后 `--it`）。

*   **分配逻辑 (`allocate`)**:
    1.  根据 `memory_size` 计算索引 `index`。
    2.  对 `m_status[index]` 加锁（自旋等待）。
    3.  检查 `m_free_array[index]` 的长度是否满足请求的 `block_count`。
    4.  如果足够：从 `m_free_array[index]` 头部取出 `block_count` 个 `memory_span`。对于每个取出的 span，调用 `record_allocated_memory_span`（内部会找到对应的 `page_span` 并调用 `page_span::allocate` 进行标记）。将取出的 span 放入结果列表。
    5.  如果不够：
        a.  计算需要向 Page Cache 申请多少页（`allocate_page_count`）。v1 中计算方式比较直接，确保能放下 `page_span::MAX_UNIT_COUNT` 个单元。
        b.  调用 `page_cache::allocate_page` 获取一个大的 `memory_span` (`page_memory`)。
        c.  创建一个新的 `page_span` 对象来管理 `page_memory`，并将这个 `page_span` 存入 `m_page_set[index]`。
        d.  将 `page_memory` 切割成 `memory_size` 大小的单元。将所需 `block_count` 个单元标记为已分配（在 `page_span` 中标记）并放入结果列表。将剩余的单元放入 `m_free_array[index]`。
    6.  解锁。
    7.  返回结果列表 `std::list<memory_span>`。

*   **回收逻辑 (`deallocate`)**:
    1.  获取待回收列表 `memories` 中第一个元素的 `memory_size`，计算索引 `index`。
    2.  对 `m_status[index]` 加锁。
    3.  遍历 `memories` 列表中的每个 `memory_span`:
        a.  将其加入 `m_free_array[index]` 的头部。
        b.  使用 `m_page_set[index]` 找到它所属的 `page_span`。
        c.  调用该 `page_span` 的 `deallocate` 方法进行标记。
        d.  检查该 `page_span` 是否 `is_empty()`（即它管理的所有小单元都已回收）。
        e.  如果 `is_empty()`:
            i.  从 `m_free_array[index]` 中移除所有属于该 `page_span` 的内存块（需要遍历链表）。
            ii. 获取该 `page_span` 管理的完整内存 `page_memory = page_span.get_memory_span()`。
            iii.从 `m_page_set[index]` 中移除该 `page_span`。
            iv. 调用 `page_cache::deallocate_page(page_memory)` 将完整内存归还给 Page Cache。
    4.  解锁。

### 4. 线程缓存 (Thread Cache - `thread_cache`)

*   **职责：** 每个线程独享的缓存，提供最快速的内存分配和回收。存储少量常用大小的空闲内存块。当缓存不足时向 Central Cache 批量申请，当缓存过多时向 Central Cache 批量归还。
*   **核心数据结构 (v1):**

```cpp
    class thread_cache {
        // ... (接口函数: allocate, deallocate)
    private:
        std::array<std::list<memory_span>, size_utils::CACHE_LINE_SIZE> m_free_cache; // 按规格大小组织的空闲块列表
        std::array<size_t, size_utils::CACHE_LINE_SIZE> m_next_allocate_count;       // 动态调整下次申请数量
        static constexpr size_t MAX_FREE_BYTES_PER_LISTS = 256 * 1024;              // 每个规格列表缓存的内存上限
    };
```

*   `m_free_cache`: 结构与 Central Cache 的 `m_free_array` 类似，`array<list<memory_span>, N>`。`m_free_cache[i]` 存储了当前线程缓存的大小为 `(i+1) * ALIGNMENT` 的空闲 `memory_span`。
*   `m_next_allocate_count`: `array<size_t, N>`。记录了下次向 Central Cache 请求索引为 `i` 的内存块时，应该请求的数量。这个值是动态调整的。
*   `MAX_FREE_BYTES_PER_LISTS`: 定义了每个规格大小的自由链表 (`m_free_cache[i]`) 最多可以缓存多少字节的内存。**设计原因：** 防止某个线程缓存过多的某种大小的内存块，导致内存浪费或占用过多资源。这是一个简单的启发式策略。

*   **分配逻辑 (`allocate`)**:
    1.  根据 `memory_size` 计算索引 `index`。
    2.  检查 `m_free_cache[index]` 是否为空。
    3.  如果不为空：从 `m_free_cache[index]` 头部取出一个 `memory_span`，返回其 `data()` 指针。
    4.  如果为空：
        a.  调用 `compute_allocate_count(memory_size)` 计算本次应向 Central Cache 申请多少个块 (`block_count`)。这个函数会参考 `m_next_allocate_count[index]` 并动态调整它（通常是指数增长，有上限）。
        b.  调用 `central_cache::allocate(memory_size, block_count)` 从 Central Cache 获取一批 `memory_span` (返回 `optional<list<memory_span>>`)。
        c.  如果获取成功：取列表中的第一个 `memory_span` 作为本次分配的结果返回。将列表中剩余的 `memory_span` 全部放入 `m_free_cache[index]`。
        d.  如果获取失败（Central Cache 也无法分配），返回 `std::nullopt`。

*   **回收逻辑 (`deallocate`)**:
    1.  根据 `start_p` (指针) 和 `memory_size` 创建一个 `memory_span`。
    2.  计算索引 `index`。
    3.  将这个 `memory_span` 加入 `m_free_cache[index]` 的头部。
    4.  检查 `m_free_cache[index]` 中缓存的总字节数（`m_free_cache[index].size() * memory_size`）是否超过了 `MAX_FREE_BYTES_PER_LISTS`。
    5.  如果超过：
        a.  计算需要归还给 Central Cache 的数量（例如，超过部分的一半 `m_free_cache[index].size() / 2`）。
        b.  从 `m_free_cache[index]` 的尾部（或其他策略，代码实现是从中间开始）取出相应数量的 `memory_span`，放入一个新的列表 `memory_to_deallocate`。
        c.  调用 `central_cache::deallocate(std::move(memory_to_deallocate))` 将这些内存块归还给 Central Cache。
        d.  **调整下次申请数量：** 将 `m_next_allocate_count[index]` 减半。**设计原因：** 这是一种负反馈调节。如果缓存满了并触发回收，说明之前一次性申请得可能太多了，下次少申请一点，尝试达到一个平衡状态。

---

## v2 版本优化与差异

v2 版本保留了 v1 的三层架构和核心思想，但在实现细节上做了显著优化，主要目标是提升性能和减少内存开销。

### 1. 核心变化：自由链表存储 (Intrusive Linked List)

*   **v1 问题：** v1 在 Thread Cache (`m_free_cache`) 和 Central Cache (`m_free_array`) 中使用 `std::list<memory_span>` 来管理空闲内存块。`std::list` 的每个节点本身就需要额外的内存分配（存储前后指针和 `memory_span` 对象），这增加了内存开销和内存碎片，并且访问链表节点可能导致缓存不命中。
*   **v2 改进：** v2 改用**侵入式链表**。不再使用 `std::list`，而是直接使用 `std::byte*` 指针来表示自由链表的头。空闲的内存块本身被用来存储指向下一个空闲块的指针。

```cpp
    // Thread Cache (v2)
    std::array<std::byte*, size_utils::CACHE_LINE_SIZE> m_free_cache = {}; // 指向自由链表头
    std::array<size_t, size_utils::CACHE_LINE_SIZE> m_free_cache_size = {}; // 记录链表长度
    
    // Central Cache (v2)
    std::array<std::byte*, size_utils::CACHE_LINE_SIZE> m_free_array = {};
    std::array<size_t, size_utils::CACHE_LINE_SIZE> m_free_array_size = {};
```

当一个内存块（地址为 `ptr`，大小至少为 `sizeof(void*)`）被释放时，它的前 `sizeof(void*)` 字节被用来存储当前自由链表的头指针 `head`，然后 `ptr` 成为新的头指针：

```cpp
*(reinterpret_cast<std::byte**>(ptr)) = head;
head = ptr;
```

当需要分配一个内存块时，操作相反：

```cpp
std::byte* result = head;
head = *(reinterpret_cast<std::byte**>(head));
// result 指向的内存块可供使用
```

*   **优势：**
    *   **减少内存开销：** 没有了 `std::list` 节点的额外开销。
    *   **提高缓存局部性：** 指针存储在内存块内部，访问链表时的数据访问更集中。
    *   **更快的操作：** 直接的指针操作通常比 `std::list` 的方法调用更快。
*   **实现变化：**
    *   `allocate` 和 `deallocate` 函数现在直接操作 `std::byte*` 指针。
    *   批量归还/获取时，需要手动遍历这个侵入式链表来连接或断开。
    *   需要额外维护链表长度 (`m_free_cache_size`, `m_free_array_size`)，因为无法像 `std::list` 那样直接调用 `size()`。

### 2. `page_span` 优化

*   **v1 问题：** `std::bitset` 在 Release 模式下仍有开销，且限制了单个 Span 可管理的最大单元数。
*   **v2 改进：** 使用宏 `NDEBUG` 进行条件编译。
    *   **Debug 模式 (`#ifndef NDEBUG`)**: 仍然使用 v1 的 `std::bitset` 实现，保留其调试和校验能力。
    *   **Release 模式 (`#else`)**: 使用一个更简单的实现：
        ```cpp
        class page_span {
            // ...
            bool is_empty() { return m_allocated_unit_count == 0; }
            void allocate(memory_span memory) { m_allocated_unit_count++; }
            void deallocate(memory_span memory) { m_allocated_unit_count--; }
            // ...
        private:
            const memory_span m_memory;
            const size_t m_unit_size;
            size_t m_total_unit_count; // (在构造时计算 m_memory.size() / m_unit_size)
            size_t m_allocated_unit_count = 0; // 只记录分配出去的数量
        };
        ```
        **优势：** Release 模式下极大地减少了 `page_span` 的开销，`allocate`/`deallocate` 只是简单的计数器增减。不再受 `MAX_UNIT_COUNT` 的限制，可以管理任意数量的单元（只要内存足够）。
        **劣势：** 失去了 Debug 模式下对单个单元分配状态的精确跟踪和校验能力。

### 3. 中心缓存动态页面分配 (Release 模式)

*   **v1 问题：** Central Cache 向 Page Cache 请求页面的数量逻辑比较固定（基于 `page_span::MAX_UNIT_COUNT`）。
*   **v2 改进 (Release 模式):** 引入了 `m_next_allocate_memory_group_count` 数组。
    *   **作用：** 类似于 Thread Cache 的 `m_next_allocate_count`，用于动态调整下次向 Page Cache 请求多少“组”内存。一组内存的大小大致对应 Thread Cache 一个列表的容量上限 (`thread_cache::MAX_FREE_BYTES_PER_LISTS`)。
    *   **逻辑：** 当 Central Cache 需要向 Page Cache 申请内存时，根据 `m_next_allocate_memory_group_count[index]` 计算需要多少页。如果之后该 `page_span` 被完全回收并归还给 Page Cache，则将 `m_next_allocate_memory_group_count[index]` 减半（负反馈）。如果分配成功，则下次尝试申请更多页（`+1` 组）。
    *   **优势：** 尝试根据实际回收情况动态调整从 Page Cache 获取的内存量，避免一次性从系统获取过多或过少内存，更具适应性。

### 4. 并发工具 (`atomic_flag_guard`)

*   **v1 问题：** Central Cache 中使用 `atomic_flag` 的 `test_and_set` 和 `clear` 手动管理锁，容易忘记 `clear` 导致死锁。
*   **v2 改进：** 引入 `atomic_flag_guard` 类。
```cpp
    class atomic_flag_guard {
    public:
        explicit atomic_flag_guard(std::atomic_flag& flag); // 构造时加锁 (自旋)
        ~atomic_flag_guard(); // 析构时解锁 (clear)
    private:
        std::atomic_flag& m_flag;
    };
```
* **优势：** 利用 RAII (Resource Acquisition Is Initialization) 原则，确保在作用域结束时自动释放锁，代码更简洁、安全。用法：`atomic_flag_guard guard(m_status[index]);`

### 5. 其他

*   **命名空间：** v2 使用 `memory_pool_v2` 命名空间以区分。
*   **接口返回类型：** `central_cache::allocate` 返回 `std::byte*` (侵入式链表头) 而非 `std::list<memory_span>`。

---

## 性能考量

*   **v2 vs v1:** 由于使用了侵入式链表、优化的 `page_span` (Release) 以及更细致的动态调整策略，**v2 版本通常比 v1 版本性能更好**，内存开销也更低。
*   **vs 系统分配器 (`malloc`/`new`):**
    *   **小对象 & 高频分配：** 内存池（尤其是 v2）在这种场景下通常**显著优于**系统分配器，因为它避免了频繁的系统调用和锁竞争（大部分在 Thread Cache 完成）。
    *   **大对象：** 性能与系统分配器相当，因为内存池会直接调用底层分配接口。
    *   **中等大小对象（接近 `MAX_CACHED_UNIT_SIZE`）：** 性能对比可能取决于具体实现和工作负载。你观察到的在 4KB-8KB 区间 v2 性能更好的现象，可能是因为标准库实现对这个区间的处理方式与内存池不同，或者内存池的缓存策略在此区间恰好更有效。但需要更严谨的 Benchmark 来确认。
*   **多线程环境：** 三层架构的设计使得内存池在多线程环境下的伸缩性（Scalability）通常优于全局加锁的简单分配器。
