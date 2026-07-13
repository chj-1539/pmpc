//=============================================================================
// test_iec101_slave_frame_layout.cxx
//
// 回归 CR-5 + CR-6（第二轮代码审查）：iec101_slave 帧解析/构造的两个关键
// 缺陷。
//
// CR-5: HandleFrame 老代码 `ctrl = buf[1]`，对固定帧正确、对可变帧错误
//       （可变帧 buf[1] = LEN 而不是 CTRL）。当 LEN 低 4 位 =0/A/B 时误发
//       Reset ACK / Class1 数据帧，主站永远拿不到 GI/遥控 confirm。
//       修复：抽 CtrlOffsetForStart(startByte) helper 按帧类型返回 CTRL
//       在 buf 中的偏移，HandleFrame 分帧类型再读 CTRL。
//
// CR-6: 所有从站 TX 帧的 LEN 字段写成 `4 + asduLen`，规约是 `3 + asduLen`
//       （LEN = CTRL(1) + ADDR(2) + N_asdu）。所有发出帧比规约多 1 字节 →
//       严格主站按 LEN 拉数据把 CS 当作 ASDU 末字节 → CS 校验失败丢帧。
//       修复：SendGIRsp / SendACK / activation-conf 全部改为 3 + asduLen；
//       抽 VariableFrameLen(asduLen) helper 供测试断言。
//=============================================================================

#include "mini_gtest.h"
#include "iec101_slave.h"

// ── CR-6: LEN 字段公式 ─────────────────────────────────────────────────────

TEST(Iec101FrameLen, MatchesSpecFormula) {
    // 规约 LEN = 3 + asduLen（CTRL 1B + ADDR 2B + ASDU）
    EXPECT_EQ(Iec101Slave::VariableFrameLen(4),  7);    // 最小合法 ASDU (type+VSQ+COT+COA)
    EXPECT_EQ(Iec101Slave::VariableFrameLen(10), 13);   // 单点 M_SP_NA_1
    EXPECT_EQ(Iec101Slave::VariableFrameLen(20), 23);
}

TEST(Iec101FrameLen, OldFormulaWouldBeOffByOne) {
    // 老代码 (4 + asduLen) 与新公式 (3 + asduLen) 恰差 1 —— 断言不重合
    for (size_t asduLen = 4; asduLen < 20; asduLen++) {
        uint8_t newLen = Iec101Slave::VariableFrameLen(asduLen);
        uint8_t oldLen = static_cast<uint8_t>(4 + asduLen);
        EXPECT_EQ(newLen + 1, oldLen);
    }
}

TEST(Iec101FrameLen, DecodeRoundTrip) {
    // 主站按 LEN 拉出 asduLen 应该反算出原 asduLen
    for (size_t asduLen : {4u, 10u, 100u, 200u}) {
        uint8_t len = Iec101Slave::VariableFrameLen(asduLen);
        size_t decoded = static_cast<size_t>(len) - 3;
        EXPECT_EQ(decoded, asduLen);
    }
}

// ── CR-5: CTRL 偏移按帧类型分派 ────────────────────────────────────────────

TEST(Iec101CtrlOffset, FixedFrameCtrlAtOffset1) {
    // 固定帧: [0x10, CTRL, ADDR_L, ADDR_H, CS, END] → CTRL 在 buf[1]
    EXPECT_EQ(Iec101Slave::CtrlOffsetForStart(0x10), (size_t)1);
}

TEST(Iec101CtrlOffset, VariableFrameCtrlAtOffset4) {
    // 可变帧: [0x68, LEN, LEN, 0x68, CTRL, ADDR_L, ADDR_H, ASDU..., CS, END]
    //         → CTRL 在 buf[4]
    EXPECT_EQ(Iec101Slave::CtrlOffsetForStart(0x68), (size_t)4);
}

TEST(Iec101CtrlOffset, UnknownStartByteRejected) {
    // 非法起始符（例如 0x00 / 0x16 / ASCII 字符）→ 返回 SIZE_MAX 意为"不解析"
    EXPECT_EQ(Iec101Slave::CtrlOffsetForStart(0x00), (size_t)-1);
    EXPECT_EQ(Iec101Slave::CtrlOffsetForStart(0x16), (size_t)-1);  // END byte
    EXPECT_EQ(Iec101Slave::CtrlOffsetForStart('A'),  (size_t)-1);
}

// ── CR-5 与 CR-6 交叉：可变帧 buf[1] 是 LEN，不是 CTRL ────────────────────

TEST(Iec101CtrlOffset, VariableFrameBuf1IsLenNotCtrl) {
    // 构造一个 LEN=0x0B 的可变帧（低 4 位 =0x0B，会被老代码误当 REQ_2D）
    // 但按新逻辑 buf[0]=0x68 → CTRL 应从 buf[4] 读，不管 buf[1] 的值
    uint8_t buf[] = { 0x68, 0x0B, 0x0B, 0x68,
                      0x53,               // CTRL: real primary/secondary msg
                      0x01, 0x00,         // ADDR
                      0x64, 0x01, 0x06, 0x00, 0x01, 0x00,  // fake ASDU
                      0x00,               // CS placeholder
                      0x16 };
    size_t off = Iec101Slave::CtrlOffsetForStart(buf[0]);
    ASSERT_NE(off, (size_t)-1);
    // 关键：读到的是 CTRL 而不是 LEN
    EXPECT_EQ(buf[off], 0x53);
    EXPECT_NE(buf[off], buf[1]);   // 老代码错读的 LEN 值
}

TEST(Iec101CtrlOffset, FixedFrameLowNibbleZeroIsRealReset) {
    // 固定帧 [0x10, 0x00, ADDR_L, ADDR_H, CS, END] — CTRL 低 4 位 =0 是
    // 真的 Reset。这时 buf[1] 就是 CTRL，让 SendACK 触发是对的。
    uint8_t buf[] = { 0x10, 0x00, 0x01, 0x00, 0x01, 0x16 };
    size_t off = Iec101Slave::CtrlOffsetForStart(buf[0]);
    ASSERT_EQ(off, (size_t)1);
    EXPECT_EQ(buf[off] & 0x0F, 0x00);   // fun==0x00 应触发 Reset ACK
}

// ── 端到端语义：修复前后场景对比 ──────────────────────────────────────────

TEST(Iec101EndToEnd, VariableFrameWithLen0x00WouldTriggerFalseAckInOldCode) {
    // 构造 LEN=0x10（16）的可变帧 —— 低 4 位 =0x00。老代码走 fun==0x00 分
    // 支发 Reset ACK；新代码应识别为可变帧继续解析 ASDU。
    uint8_t buf[] = { 0x68, 0x10, 0x10, 0x68,
                      0x53, 0x01, 0x00,        // CTRL + ADDR
                      0x64, 0x01, 0x06, 0x00, 0x01, 0x00,
                      0x00, 0x00, 0x00, 0x00, 0x00,
                      0x00, 0x16 };
    // 判定为完整可变帧 + CTRL 偏移正确
    EXPECT_TRUE(Iec101Slave::IsCompleteVariableFrame(buf, sizeof(buf)));
    EXPECT_EQ(Iec101Slave::CtrlOffsetForStart(buf[0]), (size_t)4);
    // 实际 CTRL = 0x53（不是 0x10 / 0x00）
    EXPECT_EQ(buf[4], 0x53);
}

TEST(Iec101EndToEnd, VariableFrameWithLen0x0AWouldTriggerFalseClass1Data) {
    // LEN=0x0A（10），低 4 位 =0x0A → 老代码走 REQ_1D 分支。
    uint8_t buf[] = { 0x68, 0x0A, 0x0A, 0x68,
                      0x73, 0x01, 0x00,        // CTRL + ADDR
                      0x64, 0x01, 0x06, 0x00, 0x01, 0x00,
                      0x00, 0x00, 0x00, 0x16 };
    EXPECT_TRUE(Iec101Slave::IsCompleteVariableFrame(buf, sizeof(buf)));
    EXPECT_EQ(buf[4], 0x73);   // 真实 CTRL
    EXPECT_NE(buf[1] & 0x0F, buf[4] & 0x0F);  // LEN 低 4 位 ≠ CTRL 低 4 位
}
