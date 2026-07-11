//=============================================================================
// test_cdt_master_parser.cxx
//
// 回归 C2：cdt_master.cxx 中同步头检测第一行判断被吞入 // 单行注释：
//   `// Sync header detection            if (pos == 0 && byte != SYNC_BYTE) continue;`
// 于是 pos=0 时任意字节都被写入 buf[0]，破坏后续帧解析。
//
// 修复把同步头状态机抽出为 CdtMaster::AdvanceSyncHeader 纯函数，本测试
// 直接驱动该函数，断言：
//   1. 非 EB 首字节会被丢弃；
//   2. 完整 EB 90 序列可推进到 pos=6；
//   3. 中途噪声能正确重同步。
//=============================================================================

#include "mini_gtest.h"
#include "cdt_master.h"

namespace {

// SYNC_BYTE = 0xEB, SYNC_BYTE2 = 0x90（在 cdt_master.cxx 内部）
constexpr uint8_t EB = 0xEB;
constexpr uint8_t NN = 0x90;

TEST(CdtParserTest, RejectsFrameWithoutEbSync) {
    // pos=0 且收到非 EB 字节，应保持 pos=0（不接受该字节）。
    // 这是 C2 修复的关键：以前这个 case 会前进到 pos=1 并写入 buf[0]。
    EXPECT_EQ(CdtMaster::AdvanceSyncHeader(0, 0x00), static_cast<size_t>(0));
    EXPECT_EQ(CdtMaster::AdvanceSyncHeader(0, 0xFF), static_cast<size_t>(0));
    EXPECT_EQ(CdtMaster::AdvanceSyncHeader(0, NN),   static_cast<size_t>(0));  // 90 不是 EB
    EXPECT_EQ(CdtMaster::AdvanceSyncHeader(0, 0xAA), static_cast<size_t>(0));
}

TEST(CdtParserTest, AcceptsValidEbFirstByte) {
    EXPECT_EQ(CdtMaster::AdvanceSyncHeader(0, EB), static_cast<size_t>(1));
}

TEST(CdtParserTest, FullSyncSequenceReachesSix) {
    // EB 90 EB 90 EB 90 → pos 依次 1 2 3 4 5 6
    size_t pos = 0;
    const uint8_t seq[] = {EB, NN, EB, NN, EB, NN};
    const size_t expected[] = {1, 2, 3, 4, 5, 6};
    for (size_t i = 0; i < 6; ++i) {
        pos = CdtMaster::AdvanceSyncHeader(pos, seq[i]);
        EXPECT_EQ(pos, expected[i]);
    }
}

TEST(CdtParserTest, GarbageBytesBeforeEbAreSkipped) {
    // 前面几个字节垃圾（0x00, 0xFF, 0xAA），然后开始正常同步。
    size_t pos = 0;
    pos = CdtMaster::AdvanceSyncHeader(pos, 0x00); EXPECT_EQ(pos, static_cast<size_t>(0));
    pos = CdtMaster::AdvanceSyncHeader(pos, 0xFF); EXPECT_EQ(pos, static_cast<size_t>(0));
    pos = CdtMaster::AdvanceSyncHeader(pos, 0xAA); EXPECT_EQ(pos, static_cast<size_t>(0));
    // 现在正常同步应能继续
    pos = CdtMaster::AdvanceSyncHeader(pos, EB); EXPECT_EQ(pos, static_cast<size_t>(1));
    pos = CdtMaster::AdvanceSyncHeader(pos, NN); EXPECT_EQ(pos, static_cast<size_t>(2));
}

TEST(CdtParserTest, ResyncOnMisplacedEb) {
    // 场景：EB EB —— 第二个字节应作为 90 位，但收到 EB。
    // 原有语义：把第二个 EB 视作"新同步头起点"，pos 保持 1。
    size_t pos = 0;
    pos = CdtMaster::AdvanceSyncHeader(pos, EB); EXPECT_EQ(pos, static_cast<size_t>(1));
    pos = CdtMaster::AdvanceSyncHeader(pos, EB); EXPECT_EQ(pos, static_cast<size_t>(1));
    // 后续正常 90 应能接上
    pos = CdtMaster::AdvanceSyncHeader(pos, NN); EXPECT_EQ(pos, static_cast<size_t>(2));
}

TEST(CdtParserTest, ResetOnWrongSyncByte) {
    // 场景：EB 00 —— 90 位收到既不是 90 也不是 EB 的字节，应重置到 0。
    size_t pos = 0;
    pos = CdtMaster::AdvanceSyncHeader(pos, EB);   EXPECT_EQ(pos, static_cast<size_t>(1));
    pos = CdtMaster::AdvanceSyncHeader(pos, 0x00); EXPECT_EQ(pos, static_cast<size_t>(0));
}

TEST(CdtParserTest, PastSyncHeaderAlwaysAdvances) {
    // pos >= 6 属于帧体接收阶段，任何字节都应被接受（返回 pos+1）。
    EXPECT_EQ(CdtMaster::AdvanceSyncHeader(6,  0x00), static_cast<size_t>(7));
    EXPECT_EQ(CdtMaster::AdvanceSyncHeader(7,  0xFF), static_cast<size_t>(8));
    EXPECT_EQ(CdtMaster::AdvanceSyncHeader(50, 0x42), static_cast<size_t>(51));
}

} // namespace
