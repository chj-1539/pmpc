//=============================================================================
// test_iec104_slave_remote_ctrl.cxx
//
// 回归 H5 (code review)：iec104_slave HandleFrame 的 C_SC_NA_1 分支曾在
// cmdVal 不匹配任何 mapping 时"fallback 到 doMap[0]"，把不匹配的请求变
// 成对第一个 entry 的写入。任何 cmdVal 都能触发 SetDoMaster。
//
// 修复：抽出 Iec104Slave::DecideRemoteControlTargets 纯函数，只返回 val
// 严格匹配的 entries；调用方走 negative ack。
//=============================================================================

#include "mini_gtest.h"
#include "iec104_slave.h"
#include <vector>

namespace {

using Target = Iec104Slave::RemoteControlTarget;

SlaveDOMapping M(int val, uint16_t ch, uint16_t dev, uint16_t pt) {
    SlaveDOMapping m;
    m.val = val; m.ch = ch; m.dev = dev; m.point = pt;
    return m;
}

TEST(Iec104SlaveRemoteCtrlTest, ExactMatchSingleEntry) {
    std::vector<SlaveDOMapping> mapping{
        M(0, 1, 1, 10),
        M(1, 1, 1, 11),
    };
    auto out = Iec104Slave::DecideRemoteControlTargets(mapping, /*cmdVal=*/1);
    ASSERT_EQ(out.size(), static_cast<size_t>(1));
    EXPECT_EQ(out[0].ch,    static_cast<uint16_t>(1));
    EXPECT_EQ(out[0].point, static_cast<uint16_t>(11));
    EXPECT_TRUE(out[0].doVal);
}

TEST(Iec104SlaveRemoteCtrlTest, ExactMatchOffCmd) {
    std::vector<SlaveDOMapping> mapping{
        M(0, 1, 1, 10),
        M(1, 1, 1, 11),
    };
    auto out = Iec104Slave::DecideRemoteControlTargets(mapping, 0);
    ASSERT_EQ(out.size(), static_cast<size_t>(1));
    EXPECT_EQ(out[0].point, static_cast<uint16_t>(10));
    EXPECT_FALSE(out[0].doVal);
}

// H5 关键回归：cmdVal 不匹配 → 空返回，绝不 fallback 到第一个 entry
TEST(Iec104SlaveRemoteCtrlTest, NoMatchReturnsEmpty) {
    std::vector<SlaveDOMapping> mapping{
        M(0, 1, 1, 10),
        M(1, 1, 1, 11),
    };
    // 老代码：cmdVal=5 会 fallback 到 doMap[0]（val=0），错误地写 SetDoMaster
    auto out = Iec104Slave::DecideRemoteControlTargets(mapping, /*cmdVal=*/5);
    EXPECT_TRUE(out.empty());

    auto out2 = Iec104Slave::DecideRemoteControlTargets(mapping, /*cmdVal=*/0x55);
    EXPECT_TRUE(out2.empty());

    auto out3 = Iec104Slave::DecideRemoteControlTargets(mapping, /*cmdVal=*/-1);
    EXPECT_TRUE(out3.empty());
}

// 空 mapping → 空返回
TEST(Iec104SlaveRemoteCtrlTest, EmptyMappingReturnsEmpty) {
    std::vector<SlaveDOMapping> mapping;
    auto out = Iec104Slave::DecideRemoteControlTargets(mapping, 0);
    EXPECT_TRUE(out.empty());

    auto out2 = Iec104Slave::DecideRemoteControlTargets(mapping, 1);
    EXPECT_TRUE(out2.empty());
}

// 多个相同 val 映射（合闸链）都要触发
TEST(Iec104SlaveRemoteCtrlTest, MultipleEntriesWithSameValAllFire) {
    std::vector<SlaveDOMapping> mapping{
        M(1, 1, 1, 10),
        M(1, 1, 1, 11),
        M(1, 1, 2, 20),
        M(0, 1, 1, 12),   // 不匹配
    };
    auto out = Iec104Slave::DecideRemoteControlTargets(mapping, 1);
    ASSERT_EQ(out.size(), static_cast<size_t>(3));
    EXPECT_EQ(out[0].point, static_cast<uint16_t>(10));
    EXPECT_EQ(out[1].point, static_cast<uint16_t>(11));
    EXPECT_EQ(out[2].point, static_cast<uint16_t>(20));
    for (auto& t : out) EXPECT_TRUE(t.doVal);
}

// doVal 语义：cmdVal!=0 → true；cmdVal==0 → false
TEST(Iec104SlaveRemoteCtrlTest, DoValFollowsCmdValTruthiness) {
    std::vector<SlaveDOMapping> mapping{ M(1, 1, 1, 10) };
    auto out = Iec104Slave::DecideRemoteControlTargets(mapping, 1);
    ASSERT_EQ(out.size(), static_cast<size_t>(1));
    EXPECT_TRUE(out[0].doVal);

    std::vector<SlaveDOMapping> mapping2{ M(0, 1, 1, 10) };
    auto out2 = Iec104Slave::DecideRemoteControlTargets(mapping2, 0);
    ASSERT_EQ(out2.size(), static_cast<size_t>(1));
    EXPECT_FALSE(out2[0].doVal);
}

} // namespace
