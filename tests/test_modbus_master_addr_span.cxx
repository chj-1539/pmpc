//=============================================================================
// test_modbus_master_addr_span.cxx
//
// 回归 H6 (code review)：ReadAndDispatch 里合并 DI/AI 地址范围时，老代码
// 保留了一段死代码：
//     if (qty < maxQty) qty = maxQty;
// 从数学上讲 qty = maxAddr - minAddr + 1 已经 ≥ 每条单独条目的 qty，
// fallback 永远不生效。抽到 ComputeAddrSpan 静态模板后，测试确认：
//   * 单条目：范围 = 该条目自身
//   * 多条目：连续覆盖 min..max，qty ≥ 每条 qty
//   * 无序输入不影响结果
//   * 空输入 → (0, 0)
//=============================================================================

#include "mini_gtest.h"
#include "modbus_tcp_master.h"
#include <vector>

namespace {

struct Item { uint16_t addr; uint16_t qty; };

TEST(AddrSpanTest, EmptyReturnsZero) {
    std::vector<Item> items;
    auto s = ModbusTcpMaster::ComputeAddrSpan(items.begin(), items.end());
    EXPECT_EQ(s.minAddr, static_cast<uint16_t>(0));
    EXPECT_EQ(s.qty,     static_cast<uint16_t>(0));
}

TEST(AddrSpanTest, SingleItemEqualsSelf) {
    std::vector<Item> items{ {100, 5} };
    auto s = ModbusTcpMaster::ComputeAddrSpan(items.begin(), items.end());
    EXPECT_EQ(s.minAddr, static_cast<uint16_t>(100));
    EXPECT_EQ(s.qty,     static_cast<uint16_t>(5));
}

TEST(AddrSpanTest, ContiguousItemsMerge) {
    std::vector<Item> items{ {0, 5}, {5, 3}, {8, 2} };
    auto s = ModbusTcpMaster::ComputeAddrSpan(items.begin(), items.end());
    EXPECT_EQ(s.minAddr, static_cast<uint16_t>(0));
    EXPECT_EQ(s.qty,     static_cast<uint16_t>(10));   // 0..9
}

TEST(AddrSpanTest, GappedItemsStillCoveredContinuously) {
    // Modbus 合并读会一次读整段，即使中间有 gap
    std::vector<Item> items{ {0, 5}, {100, 3} };
    auto s = ModbusTcpMaster::ComputeAddrSpan(items.begin(), items.end());
    EXPECT_EQ(s.minAddr, static_cast<uint16_t>(0));
    EXPECT_EQ(s.qty,     static_cast<uint16_t>(103));   // 0..102
}

TEST(AddrSpanTest, UnsortedInputSameResult) {
    // 无序输入不影响结果（H6 审计报告点出的怀疑点）
    std::vector<Item> items{ {100, 3}, {0, 5}, {10, 20} };
    auto s = ModbusTcpMaster::ComputeAddrSpan(items.begin(), items.end());
    EXPECT_EQ(s.minAddr, static_cast<uint16_t>(0));
    EXPECT_EQ(s.qty,     static_cast<uint16_t>(103));   // 0..102
}

TEST(AddrSpanTest, ItemsWithLargeQtyBoundaryCorrect) {
    // 单条大 qty 项：{5, 20} → [5..24]；再加 {10, 1} 不改边界
    std::vector<Item> items{ {5, 20}, {10, 1} };
    auto s = ModbusTcpMaster::ComputeAddrSpan(items.begin(), items.end());
    EXPECT_EQ(s.minAddr, static_cast<uint16_t>(5));
    EXPECT_EQ(s.qty,     static_cast<uint16_t>(20));
}

TEST(AddrSpanTest, QtyIsAtLeastMaxIndividualQty) {
    // H6 死代码想避免的场景：单条 qty 大于合并 span？数学上不可能：
    // qty = (maxAddr - minAddr + 1) >= 任何单条的 (addr + q - 1) - addr + 1 = q
    // 用 fuzz-like 组合验证不变式
    struct Case { std::vector<Item> items; };
    std::vector<Case> cases{
        {{{100, 1}}},
        {{{0, 1000}}},
        {{{50, 5}, {60, 5}, {40, 3}}},
        {{{0, 100}, {200, 50}, {150, 10}}},
    };
    for (const auto& c : cases) {
        auto s = ModbusTcpMaster::ComputeAddrSpan(c.items.begin(), c.items.end());
        for (auto& it : c.items) {
            EXPECT_GE(s.qty, it.qty);
        }
    }
}

} // namespace
