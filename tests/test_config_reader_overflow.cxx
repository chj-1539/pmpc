//=============================================================================
// test_config_reader_overflow.cxx
//
// 回归 M13：pmpc_config_reader.cxx 之前用 uint16_t 做 DI/AI/DO/AO 计数循环变量：
//   for (uint16_t i = 1; i <= diCnt + 1; i++) ...
// 若 diCnt=65535，diCnt+1 在 uint16_t 语境下溢出成 0 → 循环一次都不执行，
// 静默丢掉所有点位。同样问题存在于 AI/DO/AO 循环（i=65535 后自增回绕成 0 →
// 死循环）。
//
// 修复：把循环变量提到 uint32_t，diTotal 显式在 uint32_t 中相加。
// 本测试通过加载极端边界配置，断言实际分配的点数正确。
//=============================================================================

#include "mini_gtest.h"
#include "pmpc_test_fixture.h"
#include "pmpc.h"
#include <fstream>
#include <cstdio>
#include <string>

using pmpc::testing::GlobalStateFixture;

namespace {

class ConfigReaderOverflowTest : public GlobalStateFixture {
protected:
    void TearDown() override {
        GlobalStateFixture::TearDown();
        for (auto& p : tmpFiles_) std::remove(p.c_str());
        tmpFiles_.clear();
    }

    /// 生成一个只含一个通道一个设备的临时 point_cfg.ini。
    /// 返回文件路径。
    std::string WriteCfg(const std::string& tag, uint32_t di, uint32_t ai,
                         uint32_t doc, uint32_t ao) {
        std::string path = "test_cfg_overflow_" + tag + ".ini";
        std::ofstream f(path);
        f << "[Channel_1]\n"
          << "Dev_1=" << di << "," << ai << "," << doc << "," << ao << "\n";
        tmpFiles_.push_back(path);
        return path;
    }

    std::vector<std::string> tmpFiles_;
};

// 边界：diCnt=65534。分配总数 65535（业务 65534 + 通讯状态位 1），pointNo 最大 65535。
// 修复前（uint16_t 循环变量）在这个规模下 `i <= diCnt+1` 也可能出问题
// (65534+1 = 65535 恰好边界)，修复后应能全部分配。
TEST_F(ConfigReaderOverflowTest, DiCountEquals65534LoadsAllPoints) {
    std::string cfg = WriteCfg("di65534", 65534, 0, 0, 0);
    RemoteDataMgr::Instance().LoadConfig(cfg);

    std::vector<DiPoint> list;
    ASSERT_TRUE(RemoteDataMgr::Instance().GetDiList(1, 1, list));
    EXPECT_EQ(list.size(), static_cast<size_t>(65535));
    ASSERT_GE(list.size(), static_cast<size_t>(2));
    EXPECT_EQ(list.front().pointNo, static_cast<uint16_t>(1));
    EXPECT_EQ(list.back().pointNo,  static_cast<uint16_t>(65535));
}

// 边界：diCnt=65535 会导致 pointNo 溢出（65535+1=65536 但 pointNo 是 uint16_t）。
// 修复后 config_reader 应主动拒绝该设备，不静默地生成损坏数据。
TEST_F(ConfigReaderOverflowTest, DiCount65535IsRejected) {
    std::string cfg = WriteCfg("di65535", 65535, 0, 0, 0);
    RemoteDataMgr::Instance().LoadConfig(cfg);

    // 设备应被拒绝（config reader 内部记 error 并 continue）；LoadConfig 仍返
    // 回 true 因为整体解析没崩，但该设备不存在。
    std::vector<DiPoint> list;
    EXPECT_FALSE(RemoteDataMgr::Instance().GetDiList(1, 1, list));
}

// 边界：diCnt=0。业务点数 0，仍应分配一个通讯状态位 pt=1。
TEST_F(ConfigReaderOverflowTest, DiCountEqualsZeroLoadsOnlyCommStatus) {
    std::string cfg = WriteCfg("di0", 0, 0, 0, 0);
    RemoteDataMgr::Instance().LoadConfig(cfg);

    std::vector<DiPoint> list;
    ASSERT_TRUE(RemoteDataMgr::Instance().GetDiList(1, 1, list));
    EXPECT_EQ(list.size(), static_cast<size_t>(1));
    ASSERT_EQ(list.size(), static_cast<size_t>(1));
    EXPECT_EQ(list.front().pointNo, static_cast<uint16_t>(1));
}

// 边界：aiCnt=65535。修复前 i=65535 自增回绕成 0，条件 0<=65535 恒真 → 死循环。
// 修复后应分配 65535 个 AI 点。
// 注：跑这个测试会分配约 65535 * sizeof(AiPoint) 字节；单次 << 10MB，可接受。
TEST_F(ConfigReaderOverflowTest, AiCountEquals65535NoInfiniteLoop) {
    std::string cfg = WriteCfg("ai65535", 0, 65535, 0, 0);
    RemoteDataMgr::Instance().LoadConfig(cfg);

    std::vector<AiPoint> list;
    ASSERT_TRUE(RemoteDataMgr::Instance().GetAiList(1, 1, list));
    EXPECT_EQ(list.size(), static_cast<size_t>(65535));
}

// 常规 case：确认修复没有破坏正常路径。
TEST_F(ConfigReaderOverflowTest, NormalCountsUnaffected) {
    std::string cfg = WriteCfg("normal", 3, 2, 2, 1);
    RemoteDataMgr::Instance().LoadConfig(cfg);

    std::vector<DiPoint> di;
    std::vector<AiPoint> ai;
    std::vector<DoPoint> doList;
    std::vector<AoPoint> ao;
    ASSERT_TRUE(RemoteDataMgr::Instance().GetDiList(1, 1, di));
    ASSERT_TRUE(RemoteDataMgr::Instance().GetAiList(1, 1, ai));
    ASSERT_TRUE(RemoteDataMgr::Instance().GetDoList(1, 1, doList));
    ASSERT_TRUE(RemoteDataMgr::Instance().GetAoList(1, 1, ao));

    EXPECT_EQ(di.size(),     static_cast<size_t>(4));  // 3 + 1 通讯状态位
    EXPECT_EQ(ai.size(),     static_cast<size_t>(2));
    EXPECT_EQ(doList.size(), static_cast<size_t>(2));
    EXPECT_EQ(ao.size(),     static_cast<size_t>(1));
}

} // namespace
