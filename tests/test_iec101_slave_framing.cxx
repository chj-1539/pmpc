//=============================================================================
// test_iec101_slave_framing.cxx
//
// 回归 L11 (code review)：iec101_slave PortThread 里可变帧接受条件
//     pos >= (size_t)buf[1] + 2 && buf[pos-1] == IEC101_END
// 若攻击者发 `[0x68, 0x00, 0x16, ...]`（LEN=0），pos>=2 就成立、buf[2]=0x16
// 恰好是 END，触发 HandleFrame(buf, 3)。HandleFrame 内部虽然 len<5 return
// 保护，但仍是一次浪费的误判，且长度字段=0 是明显畸形帧。
//
// 修复：抽出 IsCompleteVariableFrame / IsCompleteFixedFrame inline 静态，
// 加上 buf[1] >= 4 的下限（CTRL+ADDR(2)+CS = 4 是可变帧 payload 最小值）。
//=============================================================================

#include "mini_gtest.h"
#include "iec101_slave.h"
#include <vector>

namespace {

TEST(Iec101FramingTest, ValidFixedFrame) {
    // START(0x10), CTRL, ADDR, CS, END(0x16) = 5 字节
    std::vector<uint8_t> f{0x10, 0x03, 0x01, 0x04, 0x16};
    EXPECT_TRUE(Iec101Slave::IsCompleteFixedFrame(f.data(), f.size()));
}

TEST(Iec101FramingTest, IncompleteFixedFrameRejected) {
    std::vector<uint8_t> f{0x10, 0x03, 0x01};
    EXPECT_FALSE(Iec101Slave::IsCompleteFixedFrame(f.data(), f.size()));
}

TEST(Iec101FramingTest, FixedFrameWrongStart) {
    std::vector<uint8_t> f{0x68, 0x03, 0x01, 0x04, 0x16};
    EXPECT_FALSE(Iec101Slave::IsCompleteFixedFrame(f.data(), f.size()));
}

TEST(Iec101FramingTest, FixedFrameWrongEnd) {
    std::vector<uint8_t> f{0x10, 0x03, 0x01, 0x04, 0xFF};
    EXPECT_FALSE(Iec101Slave::IsCompleteFixedFrame(f.data(), f.size()));
}

// L11 关键回归：可变帧 LEN=0 应被拒绝
TEST(Iec101FramingTest, VariableFrameLenZeroRejected) {
    // [START_VAR=0x68, LEN=0x00, END=0x16] 老代码用 pos=3 会误接受
    std::vector<uint8_t> f{0x68, 0x00, 0x16};
    EXPECT_FALSE(Iec101Slave::IsCompleteVariableFrame(f.data(), f.size()));
}

TEST(Iec101FramingTest, VariableFrameLenTooShortRejected) {
    // LEN=3（< 4 下限）
    std::vector<uint8_t> f{0x68, 0x03, 0xAA, 0xBB, 0xCC, 0x16};
    EXPECT_FALSE(Iec101Slave::IsCompleteVariableFrame(f.data(), f.size()));
}

TEST(Iec101FramingTest, VariableFrameMinValid) {
    // LEN=4，最小合法可变帧：START LEN [CTRL ADDR(2) CS] END = 7 字节
    // pos >= LEN+2 = 6，加上 END = 7
    std::vector<uint8_t> f{0x68, 0x04, 0x53, 0x01, 0x00, 0x54, 0x16};
    EXPECT_TRUE(Iec101Slave::IsCompleteVariableFrame(f.data(), f.size()));
}

TEST(Iec101FramingTest, VariableFrameIncompleteBufferRejected) {
    // LEN=4，但 pos 只有 5（还差 END）
    std::vector<uint8_t> f{0x68, 0x04, 0x53, 0x01, 0x00};
    EXPECT_FALSE(Iec101Slave::IsCompleteVariableFrame(f.data(), f.size()));
}

TEST(Iec101FramingTest, VariableFrameWrongStart) {
    std::vector<uint8_t> f{0x10, 0x04, 0x53, 0x01, 0x00, 0x54, 0x16};
    EXPECT_FALSE(Iec101Slave::IsCompleteVariableFrame(f.data(), f.size()));
}

TEST(Iec101FramingTest, VariableFrameWrongEnd) {
    std::vector<uint8_t> f{0x68, 0x04, 0x53, 0x01, 0x00, 0x54, 0xFF};
    EXPECT_FALSE(Iec101Slave::IsCompleteVariableFrame(f.data(), f.size()));
}

TEST(Iec101FramingTest, VariableFrameLongerPayloadValid) {
    // LEN=10，payload 有 10 字节，总长 12
    std::vector<uint8_t> f{
        0x68, 0x0A,
        0x53, 0x01, 0x00,             // CTRL ADDR
        0x64, 0x01, 0x06, 0x00,       // ASDU header
        0x01, 0x00,                    // IOA (partial)
        0x11,                          // CS (dummy)
        0x16
    };
    // 期望：pos=13 >= LEN+2=12, buf[12]=0x16 = END
    // 检查一下 f.size() == 13？我数错了
    ASSERT_EQ(f.size(), static_cast<size_t>(13));
    EXPECT_TRUE(Iec101Slave::IsCompleteVariableFrame(f.data(), f.size()));
}

} // namespace
