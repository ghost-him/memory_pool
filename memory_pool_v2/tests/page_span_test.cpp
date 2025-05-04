// 该内容由 gemini 2.5 pro preview 03-25 生成，https://aistudio.google.com/prompts/new_chat
#include "gtest/gtest.h"
#include "../memory_pool_v2/utils.h" // 包含你要测试的头文件
#include <vector>             // 用于轻松管理测试内存
#include <memory>             // 用于 unique_ptr (可选，但推荐)
#include <numeric>            // 用于 std::iota (可选，用于填充测试数据)
#include <cstddef>            // size_t, byte

using namespace memory_pool_v2;

// 定义一个测试 Fixture，方便设置和清理共享资源
class PageSpanTest : public ::testing::Test {
protected:
    // 定义测试中使用的常量
    // 注意：为了测试，我们不一定严格使用 size_utils::PAGE_SIZE
    // 可以选择一个更小、更容易管理的大小，或者就用标准大小
    static constexpr size_t test_page_size = 1024; // 假设用一个较小的页面大小测试
    static constexpr size_t test_unit_size = 64;   // 假设每个单元大小为 64 字节
    // 计算基于测试设置的单元数量
    static constexpr size_t test_unit_count = test_page_size / test_unit_size;

    std::vector<std::byte> memory_buffer; // 存储实际的内存块
    memory_span page_mem_span{nullptr, 0}; // 指向 buffer 的 memory_span
    std::unique_ptr<page_span> page;      // 指向 page_span 对象的智能指针

    // 在每个测试用例开始前执行
    void SetUp() override {
        // 1. 分配内存缓冲区
        memory_buffer.resize(test_page_size);
        // 可选：用特定模式填充，以便调试时观察
        // std::iota(memory_buffer.begin(), memory_buffer.end(), std::byte{0});

        // 2. 创建指向缓冲区的 memory_span
        page_mem_span = memory_span{memory_buffer.data(), memory_buffer.size()};

        // 3. 创建 page_span 对象进行测试
        page = std::make_unique<page_span>(page_mem_span, test_unit_size);
    }

    // 在每个测试用例结束后执行 (可选，std::vector 和 unique_ptr 会自动清理)
    void TearDown() override {
        // 内存会自动释放
    }

    // 辅助函数：根据单元索引获取对应的 memory_span
    memory_span get_unit_memory_span(size_t index) const {
        if (index >= test_unit_count) {
            // 返回一个无效的 span，用于测试边界条件
            return memory_span{nullptr, 0};
        }
        std::byte* unit_start_ptr = page_mem_span.data() + index * test_unit_size;
        return memory_span{unit_start_ptr, test_unit_size};
    }
};

// 测试 1: 初始状态检查
TEST_F(PageSpanTest, InitialStateIsEmpty) {
    // 新创建的 page_span 应该是空的 (没有任何单元被分配)
    EXPECT_TRUE(page->is_empty());
    // 检查 data() 返回的是否是缓冲区的起始地址
    EXPECT_EQ(page->data(), memory_buffer.data());
}

// 测试 2: 验证 is_valid_unit_span 方法
TEST_F(PageSpanTest, IsValidUnitSpanCheck) {
    // 检查第一个单元是否有效
    memory_span first_unit = get_unit_memory_span(0);
    EXPECT_TRUE(page->is_valid_unit_span(first_unit));

    // 检查最后一个单元是否有效
    memory_span last_unit = get_unit_memory_span(test_unit_count - 1);
    EXPECT_TRUE(page->is_valid_unit_span(last_unit));

    // 检查一个大小错误的 span (比 unit_size 小)
    memory_span wrong_size_small = memory_span{first_unit.data(), test_unit_size / 2};
    EXPECT_FALSE(page->is_valid_unit_span(wrong_size_small));

    // 检查一个大小错误的 span (比 unit_size 大)
    memory_span wrong_size_large = memory_span{first_unit.data(), test_unit_size * 2};
    EXPECT_FALSE(page->is_valid_unit_span(wrong_size_large));

    // 检查一个起始地址在页面之前但在页面内的 span (理论上不可能通过 get_unit_memory_span 创建，手动构造)
    memory_span before_start = memory_span{page->data() - test_unit_size, test_unit_size};
    EXPECT_FALSE(page->is_valid_unit_span(before_start));

    // 检查一个起始地址在页面内但未对齐 unit_size 的 span
    memory_span misaligned_start = memory_span{page->data() + test_unit_size / 2, test_unit_size};
    EXPECT_FALSE(page->is_valid_unit_span(misaligned_start));

    // 检查一个跨越页面边界的 span (起始有效，但结束无效)
    memory_span out_of_bounds = memory_span{last_unit.data(), test_unit_size * 2}; // 即使大小正确，结束地址也会超界
    // 我们的 is_valid_unit_span 实现会先检查大小，再检查地址范围
    EXPECT_FALSE(page->is_valid_unit_span(out_of_bounds));

     // 检查一个刚好在页面结束地址之外的 span
    memory_span just_after_end = memory_span{page->data() + test_page_size, test_unit_size};
    EXPECT_FALSE(page->is_valid_unit_span(just_after_end));

    // 检查一个空指针 span
    memory_span null_span = memory_span{nullptr, test_unit_size};
    EXPECT_FALSE(page->is_valid_unit_span(null_span));
}

// 测试 3: 单个单元的分配和释放
TEST_F(PageSpanTest, AllocateDeallocateSingleUnit) {
    memory_span unit_to_test = get_unit_memory_span(1); // 选择第二个单元进行测试

    // 初始时应该是空的
    EXPECT_TRUE(page->is_empty());

    // 分配单元
    // 注意：allocate/deallocate 内部有 assert，但 GTest 通常在 Release 模式下运行，
    // assert 可能被禁用。我们主要测试状态变化和 is_empty() 的行为。
    // 如果需要测试断言本身，需要使用 EXPECT_DEATH 或 ASSERT_DEATH，并确保断言在测试构建中启用。
    ASSERT_NO_THROW(page->allocate(unit_to_test)); // 确认调用不抛出异常

    // 分配后，页面不应为空
    EXPECT_FALSE(page->is_empty());

    // 尝试再次分配同一个单元 (如果断言启用且处于Debug模式，这里可能会失败)
    // 在标准测试流程中，我们假设 allocate 不会因为重复分配而崩溃，但内部状态可能不一致
    // 这里我们不直接测试重复分配的断言，而是关注释放

    // 释放单元
    ASSERT_NO_THROW(page->deallocate(unit_to_test)); // 确认调用不抛出异常

    // 释放后，页面应该再次变为空
    EXPECT_TRUE(page->is_empty());

    // 尝试释放一个已经释放的单元 (如果断言启用且处于Debug模式，这里可能会失败)
    // 这里我们也不直接测试断言
}

// 测试 4: 分配多个单元
TEST_F(PageSpanTest, AllocateMultipleUnits) {
    memory_span unit0 = get_unit_memory_span(0);
    memory_span unit3 = get_unit_memory_span(3);
    memory_span unit_last = get_unit_memory_span(test_unit_count - 1);

    EXPECT_TRUE(page->is_empty());

    ASSERT_NO_THROW(page->allocate(unit0));
    EXPECT_FALSE(page->is_empty());

    ASSERT_NO_THROW(page->allocate(unit3));
    EXPECT_FALSE(page->is_empty()); // 仍然不为空

    ASSERT_NO_THROW(page->allocate(unit_last));
    EXPECT_FALSE(page->is_empty()); // 仍然不为空

    // 验证：如果再分配一个已经分配的单元会怎样？（行为依赖于 assert）

    // 现在释放它们
    ASSERT_NO_THROW(page->deallocate(unit3));
    EXPECT_FALSE(page->is_empty()); // 还不为空

    ASSERT_NO_THROW(page->deallocate(unit_last));
    EXPECT_FALSE(page->is_empty()); // 还不为空

    ASSERT_NO_THROW(page->deallocate(unit0));
    EXPECT_TRUE(page->is_empty()); // 现在应该空了
}

// 测试 5: 分配所有单元
TEST_F(PageSpanTest, AllocateAllUnits) {
    EXPECT_TRUE(page->is_empty());

    // 分配所有单元
    for (size_t i = 0; i < test_unit_count; ++i) {
        memory_span current_unit = get_unit_memory_span(i);
        ASSERT_NO_THROW(page->allocate(current_unit));
        EXPECT_FALSE(page->is_empty()); // 在分配过程中，永远不该是空的
    }

    // 此时页面仍然不为空
    EXPECT_FALSE(page->is_empty());

    // 释放所有单元
    for (size_t i = 0; i < test_unit_count; ++i) {
        memory_span current_unit = get_unit_memory_span(i);
        EXPECT_FALSE(page->is_empty()); // 在释放最后一个之前，不该是空的
        ASSERT_NO_THROW(page->deallocate(current_unit));
    }

    // 所有单元释放后，页面应该为空
    EXPECT_TRUE(page->is_empty());
}

// 测试 6: 比较操作符 (<=>)
TEST_F(PageSpanTest, ComparisonOperator) {
    // 创建第二个缓冲区和 page_span 用于比较
    std::vector<std::byte> buffer1(test_page_size);
    memory_span span1{buffer1.data(), buffer1.size()};
    page_span page1(span1, test_unit_size);

    std::vector<std::byte> buffer2(test_page_size);
    memory_span span2{buffer2.data(), buffer2.size()};
    page_span page2(span2, test_unit_size);

    // 比较基于内存地址
    if (buffer1.data() < buffer2.data()) {
        EXPECT_TRUE((page1 <=> page2) < 0);
        EXPECT_TRUE((page2 <=> page1) > 0);
    } else if (buffer1.data() > buffer2.data()) {
        EXPECT_TRUE((page1 <=> page2) > 0);
        EXPECT_TRUE((page2 <=> page1) < 0);
    } else {
        // 地址相等的情况非常罕见，除非指向同一块内存
        EXPECT_TRUE((page1 <=> page2) == 0);
        EXPECT_TRUE((page2 <=> page1) == 0);
    }

    // 与自身的比较应该是相等的
    EXPECT_TRUE((page1 <=> page1) == 0);
    EXPECT_TRUE((page2 <=> page2) == 0);
}

// 你可以根据需要添加更多测试，例如：
// - 测试边界条件下的 allocate/deallocate
// - 如果你的 page_span 设计为可以处理跨越多个物理页的情况（虽然当前代码不支持），需要相应测试
// - 使用 EXPECT_DEATH / ASSERT_DEATH 测试断言（需要配置 CMake 和编译选项以启用断言并在测试中生效）