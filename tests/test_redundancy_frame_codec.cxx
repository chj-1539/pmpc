//=============================================================================
// test_redundancy_frame_codec.cxx
//
// 回归 C4：以前心跳帧只带 role/ts，peerPriority_ 永远是 0，导致
// CheckFailover 的双主/等优先决策失效。
//
// 拆解后 pmpc::redundancy::BuildHeartbeatFrame / ParseHeartbeatFrame
// 携带 priority；本测试断言：
//   1. build 出的心跳帧含 priority 字段，roundtrip 无损；
//   2. 老格式帧（14 字节，无 priority）仍可解析，priority 回退为 0；
//   3. BuildSyncFrame / ParseSyncFrame roundtrip 保真（含 IEEE754 double）。
//=============================================================================

#include "mini_gtest.h"
#include "redundancy.h"
#include <cstring>
#include <vector>

using namespace pmpc::redundancy;

namespace {

TEST(RedundancyFrameTest, HeartbeatRoundTripPreservesPriority) {
    // 修复 C4 的关键 assert：priority 从 build 到 parse 无损
    auto f = BuildHeartbeatFrame(RedundRole::Master, 200, 0x1122334455667788ull);
    ASSERT_EQ(f.size(), static_cast<size_t>(16));  // START+FUN+LEN(2)+ROLE+TS(8)+PRI(2)+END
    EXPECT_EQ(f[0], HB_START);
    EXPECT_EQ(f[1], HB_FUN);
    EXPECT_EQ(f[2], static_cast<uint8_t>(11));      // LEN low = payload length
    EXPECT_EQ(f[3], static_cast<uint8_t>(0));       // LEN high
    EXPECT_EQ(f.back(), HB_END);

    HeartbeatInfo info;
    ASSERT_TRUE(ParseHeartbeatFrame(f.data(), f.size(), info));
    EXPECT_EQ(static_cast<int>(info.role),  static_cast<int>(RedundRole::Master));
    EXPECT_EQ(info.tsMs,     0x1122334455667788ull);
    EXPECT_EQ(info.priority, static_cast<uint16_t>(200));
}

TEST(RedundancyFrameTest, HeartbeatRoundTripStandbyZeroPriority) {
    auto f = BuildHeartbeatFrame(RedundRole::Standby, 0, 42);
    HeartbeatInfo info;
    ASSERT_TRUE(ParseHeartbeatFrame(f.data(), f.size(), info));
    EXPECT_EQ(static_cast<int>(info.role),  static_cast<int>(RedundRole::Standby));
    EXPECT_EQ(info.tsMs,     static_cast<uint64_t>(42));
    EXPECT_EQ(info.priority, static_cast<uint16_t>(0));
}

TEST(RedundancyFrameTest, HeartbeatParseAcceptsLegacyLen8Frame) {
    // 旧对端发的 14 字节帧（LEN=8，无 priority）
    std::vector<uint8_t> legacy{
        HB_START, HB_FUN, 0x08, 0x00,
        static_cast<uint8_t>(RedundRole::Master),
        0xAA, 0xBB, 0xCC, 0xDD, 0x00, 0x00, 0x00, 0x00,
        HB_END
    };
    HeartbeatInfo info;
    ASSERT_TRUE(ParseHeartbeatFrame(legacy.data(), legacy.size(), info));
    EXPECT_EQ(static_cast<int>(info.role), static_cast<int>(RedundRole::Master));
    EXPECT_EQ(info.tsMs, static_cast<uint64_t>(0xDDCCBBAA));
    EXPECT_EQ(info.priority, static_cast<uint16_t>(0));   // 老格式回退为 0
}

TEST(RedundancyFrameTest, HeartbeatRejectsInvalidStart) {
    std::vector<uint8_t> bad{
        0x00, HB_FUN, 0x0A, 0x00,
        static_cast<uint8_t>(RedundRole::Master),
        0,0,0,0,0,0,0,0, 100,0, HB_END
    };
    HeartbeatInfo info;
    EXPECT_FALSE(ParseHeartbeatFrame(bad.data(), bad.size(), info));
}

TEST(RedundancyFrameTest, HeartbeatRejectsMissingEnd) {
    auto f = BuildHeartbeatFrame(RedundRole::Master, 100, 5000);
    f.back() = 0x00;
    HeartbeatInfo info;
    EXPECT_FALSE(ParseHeartbeatFrame(f.data(), f.size(), info));
}

TEST(RedundancyFrameTest, HeartbeatRejectsInvalidLen) {
    // LEN 字段是 7，既不是 8 也不是 11，应拒绝
    std::vector<uint8_t> bad{
        HB_START, HB_FUN, 0x07, 0x00,
        static_cast<uint8_t>(RedundRole::Master),
        0,0,0,0,0,0,0,0, HB_END
    };
    HeartbeatInfo info;
    EXPECT_FALSE(ParseHeartbeatFrame(bad.data(), bad.size(), info));
}

TEST(RedundancyFrameTest, HeartbeatRejectsTruncated) {
    auto f = BuildHeartbeatFrame(RedundRole::Master, 100, 5000);
    HeartbeatInfo info;
    EXPECT_FALSE(ParseHeartbeatFrame(f.data(), 5, info));  // 头都没读全
}

TEST(RedundancyFrameTest, SyncRoundTripPreservesDoubleAndTs) {
    ChangeEvent ev;
    ev.type    = ChangeEvent::AI_CHANGE;
    ev.channel = 3;
    ev.device  = 7;
    ev.point   = 42;
    ev.value   = 0;                  // 对 AI 使用 dvalue，value 无关
    ev.dvalue  = 3.14159265358979;
    ev.tsMs    = 0xDEADBEEFCAFEBABEull;

    auto f = BuildSyncFrame(ev);
    ASSERT_EQ(f.size(), static_cast<size_t>(28));

    ChangeEvent parsed{};
    ASSERT_TRUE(ParseSyncFrame(f.data(), f.size(), parsed));
    EXPECT_EQ(parsed.type,    ev.type);
    EXPECT_EQ(parsed.channel, ev.channel);
    EXPECT_EQ(parsed.device,  ev.device);
    EXPECT_EQ(parsed.point,   ev.point);
    // 用 memcmp 精确比较 IEEE754 位模式，避免浮点比较漂移
    uint64_t bitsA, bitsB;
    std::memcpy(&bitsA, &parsed.dvalue, 8);
    std::memcpy(&bitsB, &ev.dvalue,     8);
    EXPECT_EQ(bitsA, bitsB);
    EXPECT_EQ(parsed.tsMs,    ev.tsMs);
}

TEST(RedundancyFrameTest, SyncRejectsShortFrame) {
    std::vector<uint8_t> buf(20, 0);
    buf[0] = SYNC_START; buf[1] = SYNC_FUN;
    ChangeEvent ev;
    EXPECT_FALSE(ParseSyncFrame(buf.data(), buf.size(), ev));
}

TEST(RedundancyFrameTest, SyncRejectsBadStart) {
    ChangeEvent ev; ev.type = 1; ev.tsMs = 0;
    auto f = BuildSyncFrame(ev);
    f[0] = 0x00;
    ChangeEvent parsed;
    EXPECT_FALSE(ParseSyncFrame(f.data(), f.size(), parsed));
}

} // namespace
