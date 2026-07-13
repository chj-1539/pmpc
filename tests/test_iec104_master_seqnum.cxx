//=============================================================================
// test_iec104_master_seqnum.cxx
//
// 回归 M2 (code review)：Iec104Master::SendIFrame 曾把 ctrl 恒设为 0，意味着
// 所有 I 帧的 sNr/rNr 都是 0。严格从站在收满 k=12 个未确认 I 帧后会拒绝
// 后续 I 帧，导致 GI / 遥控超时。
//
// 修复：SendIFrame 现在接收 (sendSeq&, recvSeq)，用 EncodeIFrameCtrl 生成
// ctrl 字段 (bit0=0 表 I 帧, bits[15:1]=sNr, bits[31:17]=rNr)。ChannelThread
// 每连接维护 sendSeq / recvSeq，每收 I 帧 recvSeq++，每发 I 帧 sendSeq++。
//
// 这里直接测编解码 helper——它决定了序号在 ctrl 字段里的位布局。
//=============================================================================

#include "mini_gtest.h"
#include "iec104_master.h"

namespace {

TEST(Iec104SeqTest, EncodeZeroSNrZeroRNr) {
    EXPECT_EQ(Iec104Master::EncodeIFrameCtrl(0, 0), 0u);
}

TEST(Iec104SeqTest, EncodeSNrOne) {
    // sNr=1 → bit[1] = 1 → ctrl = 0x00000002
    EXPECT_EQ(Iec104Master::EncodeIFrameCtrl(1, 0), 0x00000002u);
}

TEST(Iec104SeqTest, EncodeRNrOne) {
    // rNr=1 → bit[17] = 1 → ctrl = 0x00020000
    EXPECT_EQ(Iec104Master::EncodeIFrameCtrl(0, 1), 0x00020000u);
}

TEST(Iec104SeqTest, EncodeBothMax15Bits) {
    // sNr=0x7FFF, rNr=0x7FFF → bits[15:1] 全 1 & bits[31:17] 全 1
    // sNr 部分：0x7FFF << 1 = 0xFFFE
    // rNr 部分：0x7FFF << 17 = 0xFFFE0000
    EXPECT_EQ(Iec104Master::EncodeIFrameCtrl(0x7FFF, 0x7FFF), 0xFFFEFFFEu);
}

TEST(Iec104SeqTest, IFrameBit0Zero) {
    // I 帧标志：bit0 必须为 0。sNr=1 时 ctrl bit0 应仍为 0。
    uint32_t ctrl = Iec104Master::EncodeIFrameCtrl(1, 0);
    EXPECT_EQ(ctrl & 0x1u, 0u);
}

TEST(Iec104SeqTest, IFrameBit16Zero) {
    // bit16 也必须为 0（与 S 帧区分）。rNr=1 时 bit16 应仍为 0。
    uint32_t ctrl = Iec104Master::EncodeIFrameCtrl(0, 1);
    EXPECT_EQ(ctrl & 0x10000u, 0u);
}

TEST(Iec104SeqTest, DecodeRoundTrip) {
    // 编码 → 解码 → 数值不变
    for (uint32_t s : {0u, 1u, 42u, 12u, 0x7FFEu, 0x7FFFu}) {
        for (uint32_t r : {0u, 1u, 100u, 0x7FFFu}) {
            uint32_t c = Iec104Master::EncodeIFrameCtrl(s, r);
            EXPECT_EQ(Iec104Master::DecodeSendSeq(c), s);
            EXPECT_EQ(Iec104Master::DecodeRecvSeq(c), r);
        }
    }
}

TEST(Iec104SeqTest, DecodeIgnoresLowBits) {
    // Decode 从 bit1 起提取 sNr；ctrl 里其它位不影响
    // 假设：外部干扰 bit0 = 1（不合法 I 帧位）；Decode 仍能取 sNr
    EXPECT_EQ(Iec104Master::DecodeSendSeq(0x00000003u), 1u);
    EXPECT_EQ(Iec104Master::DecodeSendSeq(0x0000000Bu), 5u);
}

TEST(Iec104SeqTest, DecodeIgnoresBit16) {
    // Decode rNr 从 bit17 起；bit16 = 1（不合法）不影响
    EXPECT_EQ(Iec104Master::DecodeRecvSeq(0x00030000u), 1u);
}

// 修复前的病症：sendSeq 恒为 0，每次调用 SendIFrame 都 ctrl=0
TEST(Iec104SeqTest, IncrementPatternDrivesCtrlValues) {
    uint32_t s = 0;
    // 模拟 4 次调用 SendIFrame（不真发，只算 ctrl）
    for (uint32_t expected = 0; expected < 4; ++expected) {
        uint32_t c = Iec104Master::EncodeIFrameCtrl(s, 0);
        EXPECT_EQ(Iec104Master::DecodeSendSeq(c), expected);
        s = (s + 1) & 0x7FFF;
    }
    // sendSeq 现在应为 4
    EXPECT_EQ(s, 4u);
}

TEST(Iec104SeqTest, WrapAt15BitBoundary) {
    // sendSeq 到 0x7FFF 后应回绕到 0
    uint32_t s = 0x7FFE;
    s = (s + 1) & 0x7FFF; EXPECT_EQ(s, 0x7FFFu);
    s = (s + 1) & 0x7FFF; EXPECT_EQ(s, 0u);
}

} // namespace
