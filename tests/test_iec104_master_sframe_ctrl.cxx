//=============================================================================
// test_iec104_master_sframe_ctrl.cxx
//
// 回归 CR-4（第二轮代码审查）：Iec104Master::SendSFrame 原来硬编码
// `uint32_t ctrl = 0x00010000` —— 展开成字节流是 [0x00, 0x00, 0x01, 0x00]，
// 而 IEC 60870-5-104 规约 S 帧格式:
//   octet1 = 0x01  (S-frame indicator, bit0=1)
//   octet2 = 0x00
//   octet3 = rNr[0..6] << 1        (bit0 保留)
//   octet4 = rNr[7..14]            (bit7 保留)
// 老代码 octet1=0x00 完全不是 S 帧标记，且 rNr 位错乱。严格从站按规约
// 解 ctrl 会认不出，且从站发出 12 个未确认 I 帧后停发（k=12 窗口满）。
//
// 修复：新增 static inline EncodeSFrameCtrl(uint32_t rNr)：
//     return 0x01 | ((rNr & 0x7FFF) << 17);
// 并让 SendSFrame 接收 recvSeq 参数；HandleIFrame 里把 recvSeq 一路传下去。
//=============================================================================

#include "mini_gtest.h"
#include "iec104_master.h"

namespace {

constexpr auto Enc = &Iec104Master::EncodeSFrameCtrl;

// 便利：从 32 位 ctrl 拆出 4 个字节（LE 顺序）
static void UnpackBytes(uint32_t ctrl, uint8_t out[4]) {
    out[0] = static_cast<uint8_t>(ctrl & 0xFF);
    out[1] = static_cast<uint8_t>((ctrl >> 8)  & 0xFF);
    out[2] = static_cast<uint8_t>((ctrl >> 16) & 0xFF);
    out[3] = static_cast<uint8_t>((ctrl >> 24) & 0xFF);
}

// ── octet1 = 0x01 (S-frame indicator) ──────────────────────────────────────

TEST(SFrameCtrl, Octet1IsSFrameIndicator) {
    uint8_t b[4];
    UnpackBytes(Enc(0), b);
    EXPECT_EQ(b[0], 0x01);
    UnpackBytes(Enc(123), b);
    EXPECT_EQ(b[0], 0x01);
    UnpackBytes(Enc(32767), b);
    EXPECT_EQ(b[0], 0x01);
}

// ── octet2 恒 0x00 ─────────────────────────────────────────────────────────

TEST(SFrameCtrl, Octet2IsZero) {
    uint8_t b[4];
    UnpackBytes(Enc(0), b);
    EXPECT_EQ(b[1], 0x00);
    UnpackBytes(Enc(0x7FFF), b);
    EXPECT_EQ(b[1], 0x00);
}

// ── rNr=0 全零（老 bug 相反的 baseline）───────────────────────────────────

TEST(SFrameCtrl, RNrZeroYieldsOnlyIndicator) {
    // 修复后 rNr=0 → ctrl = 0x00000001（对比老代码 0x00010000 差异明显）
    EXPECT_EQ(Enc(0), 0x00000001u);
}

// ── rNr 各种值：octet3 = bit0 保留, rNr LSB 在 bit1 ────────────────────────

TEST(SFrameCtrl, RNrOneShiftedToBit17) {
    // rNr=1 → bit17 = 1 → ctrl = 0x00020001 → byte3 = 0x02
    uint32_t ctrl = Enc(1);
    EXPECT_EQ(ctrl, 0x00020001u);
    uint8_t b[4];
    UnpackBytes(ctrl, b);
    EXPECT_EQ(b[2], 0x02);   // bit1 of octet3 = rNr LSB
    EXPECT_EQ(b[3], 0x00);
}

TEST(SFrameCtrl, RNrSmallValues) {
    // rNr=2 → bit18=1 → octet3 = 0x04
    uint8_t b[4];
    UnpackBytes(Enc(2), b);
    EXPECT_EQ(b[2], 0x04);
    // rNr=3 → bits 17,18 = 1 → octet3 = 0x06
    UnpackBytes(Enc(3), b);
    EXPECT_EQ(b[2], 0x06);
    // rNr=127 → octet3 bits 1..7 全 1 → octet3 = 0xFE，octet4=0x00
    UnpackBytes(Enc(127), b);
    EXPECT_EQ(b[2], 0xFE);
    EXPECT_EQ(b[3], 0x00);
}

TEST(SFrameCtrl, RNrCrossesOctetBoundary) {
    // rNr=128 → bit24=1 → octet4 = 0x01
    uint8_t b[4];
    UnpackBytes(Enc(128), b);
    EXPECT_EQ(b[2], 0x00);   // octet3 bit1..7 全 0
    EXPECT_EQ(b[3], 0x01);   // octet4 bit0 = 1
    // rNr=256 → bit25=1 → octet4 = 0x02
    UnpackBytes(Enc(256), b);
    EXPECT_EQ(b[3], 0x02);
}

TEST(SFrameCtrl, RNrMaxValueRespectsRange) {
    // rNr 是 15 位（0..32767）。给它一个正好的边界值。
    uint32_t ctrl = Enc(0x7FFF);
    // rNr[0..6]=0x7F → octet3 = 0xFE, rNr[7..14]=0xFF → octet4 = 0xFF
    uint8_t b[4];
    UnpackBytes(ctrl, b);
    EXPECT_EQ(b[0], 0x01);
    EXPECT_EQ(b[1], 0x00);
    EXPECT_EQ(b[2], 0xFE);
    EXPECT_EQ(b[3], 0xFF);
}

TEST(SFrameCtrl, RNrHigherBitsMaskedOff) {
    // 超过 15 位的高位应被掩掉，避免溢出到 bit>31（reserved）
    uint32_t ctrl_15bit = Enc(0x7FFF);
    uint32_t ctrl_over  = Enc(0x8000);
    // bit 15 应被 & 0x7FFF 掩掉；结果与 rNr=0 一样只剩 indicator
    EXPECT_EQ(ctrl_over, 0x00000001u);
    (void)ctrl_15bit;
}

// ── 与 EncodeIFrameCtrl 对偶：I 帧 recvSeq 与 S 帧 rNr 编码位置一致 ────────

TEST(SFrameVsIFrameCtrl, RecvSeqEncodedAtSamePosition) {
    // 对同一 recvSeq，I 帧 ctrl 的 bit17..31 与 S 帧 ctrl 的 bit17..31 相同
    // （只差 bit0 indicator 与 bit1..15 的 sendSeq）
    const uint32_t rSeq = 42;
    uint32_t iCtrl = Iec104Master::EncodeIFrameCtrl(0, rSeq);
    uint32_t sCtrl = Iec104Master::EncodeSFrameCtrl(rSeq);
    uint32_t iRHigh = iCtrl & 0xFFFE0000u;   // 取 bit17..31
    uint32_t sRHigh = sCtrl & 0xFFFE0000u;
    EXPECT_EQ(iRHigh, sRHigh);
    // 且 DecodeRecvSeq 应能对二者都还原出正确的 rSeq
    EXPECT_EQ(Iec104Master::DecodeRecvSeq(iCtrl), rSeq);
    EXPECT_EQ(Iec104Master::DecodeRecvSeq(sCtrl), rSeq);
}

// ── 保证老代码的错误值不再出现 ────────────────────────────────────────────

TEST(SFrameCtrl, OldHardcodedValueIsGone) {
    // 老代码 `uint32_t ctrl = 0x00010000` 会让 octet1=0x00（不是 S 帧标记！）、
    // octet3=0x01（=rNr bit15）。我们检查新公式对任意 rNr 都不会碰撞到
    // 老的错值。
    for (uint32_t r = 0; r < 32768u; r += 137) {
        EXPECT_NE(Enc(r), 0x00010000u);
    }
}

} // namespace
