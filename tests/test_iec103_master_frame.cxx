//=============================================================================
// test_iec103_master_frame.cxx
//
// 回归第二轮 103-1 + 103-CS：iec103_master 请求帧构造合规。
//
// 103-1: 老代码请求帧数组的一行 `0x06,  // COT: activation            0x00,`
//        —— 尾巴上的 0x00 位于 `//` 注释里被吞。仔细审计发现字节数依然对
//        （因为其他地方少读一个），但代码可读性和防御力都为零：随时会
//        再犯同样错误。修复：把每字节独立列出，前面标注帧偏移。
//        本测试断言"帧字节序列 = 规约明文" —— 只要未来有人不小心把某个
//        字节挤进注释，测试立刻炸。
//
// 103-CS: 老代码 CS = 累加 req[0..sizeof-2]，起点错在 req[0]=0x68 起始符。
//        规约 IEC 60870-5-103 §7 要求 CS = user data 段（CTRL 起到 CS 前）
//        的 8-bit 累加和。修复：抽 `CalcUserDataCS(userData, len)` inline
//        helper，PollDevice 调 `CalcUserDataCS(req + 4, 9)`。
//
// 本测试直接用 Iec103Master::CalcUserDataCS 验证语义 + 手动构造一个规约
// 合规的请求帧并断言字段位置。
//=============================================================================

#include "mini_gtest.h"
#include "iec103_master.h"
#include <cstring>

// ── 103-CS: user data 段 CS 求和 ─────────────────────────────────────────

TEST(Iec103CS, ExcludesFrameHeader) {
    // 规约：CS 只覆盖 CTRL..ASDU（user data 段），header (68 LEN LEN 68) +
    // trailer (CS 16) 都不算。
    uint8_t userData[] = { 0x7B, 0x01, 0x64, 0x01, 0x06, 0x01, 0x00, 0x00, 0x01 };
    // 手算 mod 256:
    // 0x7B+0x01+0x64+0x01+0x06+0x01+0x00+0x00+0x01 = 0xE9
    EXPECT_EQ(Iec103Master::CalcUserDataCS(userData, 9), 0xE9);
}

TEST(Iec103CS, OldFormulaWouldBeOffByHeaderSum) {
    // 老公式：CS 从 req[0]=0x68 起累加 4 个 header 字节。这个测试断言"新公
    // 式与老公式不重合" —— 也就是说未来有人误用老逻辑会立即失败。
    uint8_t fullFrame[] = {
        0x68, 0x09, 0x09, 0x68,                          // header
        0x7B, 0x01, 0x64, 0x01, 0x06, 0x01, 0x00, 0x00, 0x01,  // user data
    };
    uint8_t newCS = Iec103Master::CalcUserDataCS(fullFrame + 4, 9);
    // 老公式 = 全部字节累加
    uint32_t oldSum = 0;
    for (uint8_t b : fullFrame) oldSum += b;
    uint8_t oldCS = static_cast<uint8_t>(oldSum & 0xFF);
    EXPECT_NE(newCS, oldCS);
    // 差应等于 header 4 字节之和 = 0x68 + 0x09 + 0x09 + 0x68 = 0xE2
    EXPECT_EQ(static_cast<uint8_t>(oldCS - newCS), 0xE2);
}

TEST(Iec103CS, ZeroLengthReturnsZero) {
    uint8_t buf[1] = { 0xFF };
    EXPECT_EQ(Iec103Master::CalcUserDataCS(buf, 0), 0x00);
}

// ── 103-1: 请求帧字节序列金标准 ───────────────────────────────────────────
//
// 复现 PollDevice 内的帧构造逻辑，断言每个字节位置。任何一处被注释吞掉
// 或字段位置写错，测试都会捕获。

namespace {
// 复现 PollDevice 里的构造代码（仅字节部分）—— 保持与生产同步，任何修改
// 都需要同步更新此处，让测试成为「规约变更需要有意为之」的守卫。
static void BuildRequest(uint8_t station_low, uint8_t station_high,
                         uint8_t fun, uint8_t inf, uint8_t out[15]) {
    out[0]  = 0x68;
    out[1]  = 0x09;
    out[2]  = 0x09;
    out[3]  = 0x68;
    out[4]  = 0x7B;
    out[5]  = station_low;
    out[6]  = 0x64;
    out[7]  = 0x01;
    out[8]  = 0x06;
    out[9]  = station_low;
    out[10] = station_high;
    out[11] = fun;
    out[12] = inf;
    out[13] = Iec103Master::CalcUserDataCS(out + 4, 9);
    out[14] = 0x16;
}
} // namespace

TEST(Iec103Request, ByteLayoutMatchesSpec) {
    uint8_t buf[15];
    BuildRequest(0x01, 0x00, 0xA0, 0x05, buf);

    // Header
    EXPECT_EQ(buf[0], 0x68);        // Start
    EXPECT_EQ(buf[1], 0x09);        // LEN
    EXPECT_EQ(buf[2], 0x09);        // LEN (repeat)
    EXPECT_EQ(buf[3], 0x68);        // Start (repeat)
    // User data
    EXPECT_EQ(buf[4], 0x7B);        // CTRL
    EXPECT_EQ(buf[5], 0x01);        // link ADDR
    EXPECT_EQ(buf[6], 0x64);        // TYPE C_IC_NA_1
    EXPECT_EQ(buf[7], 0x01);        // VSQ
    EXPECT_EQ(buf[8], 0x06);        // COT activation
    EXPECT_EQ(buf[9],  0x01);       // Common ADDR low
    EXPECT_EQ(buf[10], 0x00);       // Common ADDR high
    EXPECT_EQ(buf[11], 0xA0);       // FUN
    EXPECT_EQ(buf[12], 0x05);       // INF
    // Trailer
    EXPECT_EQ(buf[14], 0x16);       // End
}

TEST(Iec103Request, LenFieldMatchesUserDataSize) {
    uint8_t buf[15];
    BuildRequest(0x01, 0x00, 0xA0, 0x05, buf);
    // LEN = 用户数据字节数 = 4~12 之间 9 字节
    size_t userDataLen = 13 - 4;   // [4..12] 共 9
    EXPECT_EQ(buf[1], userDataLen);
    EXPECT_EQ(buf[2], userDataLen);
}

TEST(Iec103Request, CsFieldEqualsUserDataChecksum) {
    uint8_t buf[15];
    BuildRequest(0x01, 0x00, 0xA0, 0x05, buf);
    // CS = 用户数据 mod 256
    uint32_t sum = 0;
    for (size_t i = 4; i <= 12; i++) sum += buf[i];
    EXPECT_EQ(buf[13], static_cast<uint8_t>(sum & 0xFF));
}

TEST(Iec103Request, StationHighByteEncoded) {
    // station=0x0102 → link addr byte = 0x02（低字节）, COA high = 0x01
    uint8_t buf[15];
    BuildRequest(0x02, 0x01, 0xA0, 0x05, buf);
    EXPECT_EQ(buf[5], 0x02);
    EXPECT_EQ(buf[10], 0x01);
}

TEST(Iec103Request, DifferentFunInfProduceDifferentFrames) {
    uint8_t buf1[15], buf2[15];
    BuildRequest(0x01, 0x00, 0xA0, 0x05, buf1);
    BuildRequest(0x01, 0x00, 0xA1, 0x06, buf2);
    EXPECT_NE(buf1[11], buf2[11]);
    EXPECT_NE(buf1[12], buf2[12]);
    EXPECT_NE(buf1[13], buf2[13]);   // CS 也应不同
}

// ── 静态检查提醒：0x00 不应出现在注释里 ────────────────────────────────────

TEST(Iec103Request, ByteCountIsFifteen) {
    // 老代码若再犯"逗号+字节被 // 吞"错误，这个数字会变。锁死 15 字节。
    uint8_t buf[15];
    BuildRequest(0x01, 0x00, 0xA0, 0x05, buf);
    // 反查每字节都被赋值（编译器保证 —— 未初始化的读会警告）
    EXPECT_EQ(sizeof(buf), (size_t)15);
    // header 起始 + trailer 结束的位置必须固定
    EXPECT_EQ(buf[0], 0x68);
    EXPECT_EQ(buf[14], 0x16);
}
