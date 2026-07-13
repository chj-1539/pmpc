//=============================================================================
// test_dlt645_frame_codec.cxx
//
// 回归第二轮 DLT-1 / DLT-2 / DLT-3：DLT 645-1997/2007 帧编解码合规。
//
// DLT-1: DATA 段（DI + Value）发送前每字节 +0x33，接收后 -0x33。
//        老代码完全未做此变换 → 任何真实电表都会拒绝或返回被误解的 BCD。
// DLT-2: CS 校验和范围从第一个起始符 0x68 起（含）到 CS 前一字节。
//        老代码 `CheckSum(frame + 1, pos - 1)` 起点少一字节。
// DLT-3: PollDevice 收帧后必须验 CS、比对地址回声、比对 DI —— 多表挂同
//        485 时防止窜表。老代码全部跳过。
//
// 抽 4 个 inline static helper 到 dlt645_master.h：
//   CalcCS(data, len)            — CS 计算，供请求端 build + 应答端 verify
//   EncodeData(data, len)        — DATA 就地 +0x33
//   DecodeData(data, len)        — DATA 就地 -0x33
//   AddrEquals(respAddr6, addr)  — 6 字节小端地址回比
//=============================================================================

#include "mini_gtest.h"
#include "dlt645_master.h"
#include <cstring>

// ── DLT-1: ±0x33 数据加解码 ───────────────────────────────────────────────

TEST(DltDataCodec, EncodeIsInverseOfDecode) {
    uint8_t buf[8] = { 0x00, 0x01, 0x02, 0x03, 0xFC, 0xFD, 0xFE, 0xFF };
    uint8_t orig[8]; std::memcpy(orig, buf, 8);

    Dlt645Master::EncodeData(buf, 8);
    // 逐字节应 = 原 + 0x33（含溢出回绕）
    for (int i = 0; i < 8; i++)
        EXPECT_EQ(buf[i], static_cast<uint8_t>(orig[i] + 0x33));

    Dlt645Master::DecodeData(buf, 8);
    for (int i = 0; i < 8; i++)
        EXPECT_EQ(buf[i], orig[i]);
}

TEST(DltDataCodec, EncodeSpecExamples) {
    // 规约例：明文 [0x12 0x34 0x56 0x78] → 加密后 [0x45 0x67 0x89 0xAB]
    uint8_t buf[] = { 0x12, 0x34, 0x56, 0x78 };
    Dlt645Master::EncodeData(buf, 4);
    EXPECT_EQ(buf[0], 0x45);
    EXPECT_EQ(buf[1], 0x67);
    EXPECT_EQ(buf[2], 0x89);
    EXPECT_EQ(buf[3], 0xAB);
}

TEST(DltDataCodec, DecodeWrapAround) {
    // 加密结果溢出回绕：0xFF - 0x33 = 0xCC（无符号回绕）
    uint8_t buf[] = { 0x00, 0x32, 0x33, 0xCC };
    Dlt645Master::DecodeData(buf, 4);
    EXPECT_EQ(buf[0], 0xCD);   // 0x00 - 0x33 = 0xCD
    EXPECT_EQ(buf[1], 0xFF);   // 0x32 - 0x33 = 0xFF
    EXPECT_EQ(buf[2], 0x00);
    EXPECT_EQ(buf[3], 0x99);
}

TEST(DltDataCodec, ZeroLenNoop) {
    // 空长度不 crash
    uint8_t buf[1] = { 0xAA };
    Dlt645Master::EncodeData(buf, 0);
    EXPECT_EQ(buf[0], 0xAA);
    Dlt645Master::DecodeData(buf, 0);
    EXPECT_EQ(buf[0], 0xAA);
}

// ── DLT-2: CS 求和范围（含起始符） ────────────────────────────────────────

TEST(DltCS, IncludesStartByte) {
    // 简单 3 字节例：0x68 + 0x01 + 0x02 = 0x6B
    uint8_t data[] = { 0x68, 0x01, 0x02 };
    EXPECT_EQ(Dlt645Master::CalcCS(data, 3), 0x6B);
}

TEST(DltCS, OverflowMod256) {
    // 4 * 0x80 = 0x200 → mod 256 = 0x00
    uint8_t data[] = { 0x80, 0x80, 0x80, 0x80 };
    EXPECT_EQ(Dlt645Master::CalcCS(data, 4), 0x00);
}

TEST(DltCS, RealRequestFrameChecksum) {
    // 完整读请求（明文 DI 未加密，仅演示）: 68 78 56 34 12 00 00 68 11 02 AA BB
    // CS 从 frame[0]=0x68 起累加到 frame[11]=0xBB
    uint8_t frame[] = { 0x68, 0x78, 0x56, 0x34, 0x12, 0x00, 0x00,
                        0x68, 0x11, 0x02, 0xAA, 0xBB };
    uint32_t sum = 0;
    for (uint8_t b : frame) sum += b;
    EXPECT_EQ(Dlt645Master::CalcCS(frame, sizeof(frame)), static_cast<uint8_t>(sum & 0xFF));
}

// ── DLT-3: 地址回比 ────────────────────────────────────────────────────────

TEST(DltAddrEquals, MatchLowByteFirstOrder) {
    // addr = 0x010001234567 → 6 字节低字节先送 = 67 45 23 01 00 01
    uint8_t respAddr[] = { 0x67, 0x45, 0x23, 0x01, 0x00, 0x01 };
    EXPECT_TRUE(Dlt645Master::AddrEquals(respAddr, 0x010001234567ULL));
}

TEST(DltAddrEquals, MismatchedByteRejects) {
    // 只改一个字节就应识别为不匹配
    uint8_t respAddr[] = { 0x67, 0x45, 0x23, 0x01, 0x00, 0x02 };
    EXPECT_FALSE(Dlt645Master::AddrEquals(respAddr, 0x010001234567ULL));
}

TEST(DltAddrEquals, BroadcastAddressAllZero) {
    // 全 0（广播地址）也能对比
    uint8_t respAddr[] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    EXPECT_TRUE(Dlt645Master::AddrEquals(respAddr, 0ULL));
    EXPECT_FALSE(Dlt645Master::AddrEquals(respAddr, 1ULL));
}

TEST(DltAddrEquals, BroadcastAAAllOnes) {
    // 广播地址 AA AA AA AA AA AA
    uint8_t respAddr[] = { 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA };
    EXPECT_TRUE(Dlt645Master::AddrEquals(respAddr, 0xAAAAAAAAAAAAULL));
}

// ── 端到端：加密后再解密 + CS 校验 ──────────────────────────────────────────

TEST(DltFrameRoundTrip, DIEncodeAndVerify) {
    // 模拟主站发出的 DI 明文 0xB633 (2007 有功总电能第 0 号)。加密 → 电表侧
    // 应解出原 DI。
    uint8_t di[] = { 0x33, 0xB6, 0x00, 0x00 };
    uint8_t di_orig[4]; std::memcpy(di_orig, di, 4);
    Dlt645Master::EncodeData(di, 4);
    // 加密后每字节 = 原 + 0x33
    for (int i = 0; i < 4; i++) EXPECT_EQ(di[i], static_cast<uint8_t>(di_orig[i] + 0x33));

    Dlt645Master::DecodeData(di, 4);
    for (int i = 0; i < 4; i++) EXPECT_EQ(di[i], di_orig[i]);
}
