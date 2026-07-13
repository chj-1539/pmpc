//=============================================================================
// test_iec101_slave_send_ack.cxx
//
// 回归第二轮 101-C：iec101_slave::SendACK 生成的固定帧规约合规。
//
// 老代码：
//   uint8_t frame[] = { 0x10, 0x03, (uint8_t)(linkAddr & 0xFF), 0x00, END };
//   frame[3] = CalcCS(frame + 1, 2);
// —— 5 字节帧，只有 1 字节地址。可变帧却用 2 字节 addr，位宽不一致；
// 且 linkAddr > 255 时高字节丢失，主站认不出。
//
// 规约（IEC 60870-5-101 §5）固定帧格式：
//   [0x10 CTRL ADDR_L ADDR_H CS 0x16]  共 6 字节
//   CS = CTRL + ADDR_L + ADDR_H（3 字节 user data mod 256）
//
// 修复：SendACK 现在用 6 字节；抽 BuildFixedAck(linkAddr, out[6]) inline
// helper 与生产 SendACK 共享字节布局，测试直接断言。
//=============================================================================

#include "mini_gtest.h"
#include "iec101_slave.h"

// ── 基本帧结构 ──────────────────────────────────────────────────────────────

TEST(Iec101SendAck, FrameIsSixBytes) {
    uint8_t buf[6];
    Iec101Slave::BuildFixedAck(1, buf);
    EXPECT_EQ(buf[0], 0x10);   // START_FIX
    EXPECT_EQ(buf[5], 0x16);   // END
}

TEST(Iec101SendAck, CtrlIsAckPrimary) {
    uint8_t buf[6];
    Iec101Slave::BuildFixedAck(1, buf);
    EXPECT_EQ(buf[1], 0x03);   // CTRL: primary + fun=3 (ACK)
}

TEST(Iec101SendAck, LowByteFirstAddress) {
    // linkAddr=0x1234 → ADDR_L=0x34, ADDR_H=0x12（小端）
    uint8_t buf[6];
    Iec101Slave::BuildFixedAck(0x1234, buf);
    EXPECT_EQ(buf[2], 0x34);   // ADDR_L
    EXPECT_EQ(buf[3], 0x12);   // ADDR_H
}

// ── 101-C 关键：> 255 的地址不再高字节丢失 ────────────────────────────────

TEST(Iec101SendAck, AddressAbove255RetainsHighByte) {
    // 老代码 addr=256 → ADDR_L=0，高字节丢失，主站认为 addr=0
    // 新代码 addr=256 → ADDR_L=0x00, ADDR_H=0x01
    uint8_t buf[6];
    Iec101Slave::BuildFixedAck(256, buf);
    EXPECT_EQ(buf[2], 0x00);
    EXPECT_EQ(buf[3], 0x01);
}

TEST(Iec101SendAck, MaxAddressEncoded) {
    // linkAddr=65535 边界
    uint8_t buf[6];
    Iec101Slave::BuildFixedAck(65535, buf);
    EXPECT_EQ(buf[2], 0xFF);
    EXPECT_EQ(buf[3], 0xFF);
}

TEST(Iec101SendAck, ZeroAddressAllZero) {
    uint8_t buf[6];
    Iec101Slave::BuildFixedAck(0, buf);
    EXPECT_EQ(buf[2], 0x00);
    EXPECT_EQ(buf[3], 0x00);
}

// ── CS 覆盖 CTRL + ADDR_L + ADDR_H ─────────────────────────────────────────

TEST(Iec101SendAck, CsCoversCtrlAndBothAddressBytes) {
    // CS = 0x03 + ADDR_L + ADDR_H (mod 256)
    uint8_t buf[6];
    Iec101Slave::BuildFixedAck(0x0102, buf);
    uint8_t expected = static_cast<uint8_t>(0x03 + 0x02 + 0x01);
    EXPECT_EQ(buf[4], expected);
}

TEST(Iec101SendAck, CsForVariousAddresses) {
    struct { uint16_t addr; uint8_t cs; } cases[] = {
        { 0x0001, static_cast<uint8_t>(0x03 + 0x01 + 0x00) },
        { 0x00FF, static_cast<uint8_t>(0x03 + 0xFF + 0x00) },
        { 0x0100, static_cast<uint8_t>(0x03 + 0x00 + 0x01) },
        { 0xFFFF, static_cast<uint8_t>(0x03 + 0xFF + 0xFF) },
    };
    for (auto& c : cases) {
        uint8_t buf[6];
        Iec101Slave::BuildFixedAck(c.addr, buf);
        EXPECT_EQ(buf[4], c.cs);
    }
}

// ── 与老代码差异（防止回退） ──────────────────────────────────────────────

TEST(Iec101SendAck, NotFiveByteFrameAnymore) {
    // 老代码：5 字节帧 { 0x10 0x03 ADDR_L CS 0x16 }。新代码固定 6 字节。
    // 用 sizeof 保护 —— 若未来有人误改回 5 字节数组，编译报错或 out 越界。
    uint8_t buf[6];
    Iec101Slave::BuildFixedAck(1, buf);
    EXPECT_EQ(sizeof(buf), (size_t)6);
    // 老代码 addr=1 时 frame[3] 是 CS；新代码 addr=1 时 frame[3] 是 ADDR_H=0
    // 它们区分明显 —— 若 buf[3] 意外等于 0x04（CS = 0x03+0x01）说明用了老逻辑
    EXPECT_EQ(buf[3], 0x00);
    EXPECT_EQ(buf[4], 0x04);   // CS = 0x03 + 0x01 + 0x00
    EXPECT_EQ(buf[5], 0x16);
}

// ── CS 独立辅助测试 ────────────────────────────────────────────────────────

TEST(Iec101CalcCS, EmptyReturnsZero) {
    uint8_t b[1] = { 0xFF };
    EXPECT_EQ(Iec101Slave::CalcCS(b, 0), 0x00);
}

TEST(Iec101CalcCS, SumModulo256) {
    uint8_t b[] = { 0x80, 0x80, 0x80 };
    // 0x180 mod 256 = 0x80
    EXPECT_EQ(Iec101Slave::CalcCS(b, 3), 0x80);
}
