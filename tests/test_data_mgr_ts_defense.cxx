//=============================================================================
// test_data_mgr_ts_defense.cxx
//
// 回归 H8 (code review)：采集模块调用 SetDi 时忘传 ts（例如某些 offline
// 分支写 tsMs=0），data_recorder 订阅 DIChange 后会把 ts_ms=0 写进
// rt_status/di_log。pmpc 的 pt=1 是通讯状态位，SetDi 已经 gated 掉不发事件；
// 但业务点 (pt≠1) 若也传 0 就会污染数据库。
//
// 修复：SetDi 里对 pt≠1 且 ts==0 的情况自动填 NowMs()。测试断言：
//   1. pt=1 传 ts=0 不变（保留通讯状态位语义）
//   2. pt≠1 传 ts=0 → 事件中 tsMs 被填成 NowMs 附近的值
//   3. GetDi 读回来的 tsMs 也是填过的
//=============================================================================

#include "mini_gtest.h"
#include "pmpc_test_fixture.h"
#include "pmpc.h"
#include "event_bus.h"
#include "str_util.h"
#include <atomic>
#include <cstdio>
#include <fstream>

using pmpc::testing::GlobalStateFixture;

namespace {

class DataMgrTsDefenseTest : public GlobalStateFixture {
protected:
    void SetUp() override {
        GlobalStateFixture::SetUp();
        cfgPath_ = "test_data_mgr_ts_defense_cfg.ini";
        std::ofstream f(cfgPath_);
        f << "[Channel_1]\n" << "Dev_1=3,0,0,0\n";   // DI 3 业务点 + pt=1 通讯状态
        f.close();
        RemoteDataMgr::Instance().LoadConfig(cfgPath_);
    }
    void TearDown() override {
        std::remove(cfgPath_.c_str());
        GlobalStateFixture::TearDown();
    }
    std::string cfgPath_;
};

TEST_F(DataMgrTsDefenseTest, Pt1WithTsZeroKeepsZeroAndDoesNotPublish) {
    auto& mgr = RemoteDataMgr::Instance();
    std::atomic<int> events{0};
    auto tok = EventBus::Subscribe<DIChange>([&](const DIChange&){ events++; });

    // pt=1 通讯状态位：ts=0 保留，且不发 DIChange
    ASSERT_TRUE(mgr.SetDi(1, 1, 1, true, 0, true));
    DiPoint p;
    ASSERT_TRUE(mgr.GetDi(1, 1, 1, p));
    EXPECT_EQ(p.tsMs, static_cast<uint64_t>(0));
    EXPECT_EQ(events.load(), 0);

    EventBus::Unsubscribe<DIChange>(tok);
}

// H8 关键回归：业务点 (pt≠1) 传 ts=0 → 自动填 NowMs()
TEST_F(DataMgrTsDefenseTest, BusinessPointWithTsZeroFillsNowMs) {
    auto& mgr = RemoteDataMgr::Instance();
    uint64_t received_ts = 0;
    auto tok = EventBus::Subscribe<DIChange>(
        [&](const DIChange& e){ received_ts = e.tsMs; });

    uint64_t t_before = NowMs();
    // 传 ts=0；SetDi 应把它填成 NowMs 附近
    ASSERT_TRUE(mgr.SetDi(1, 1, 2, true, 0, true));
    uint64_t t_after = NowMs();

    EXPECT_GE(received_ts, t_before);
    EXPECT_LE(received_ts, t_after);

    // GetDi 读回的 tsMs 应等于事件里的
    DiPoint p;
    ASSERT_TRUE(mgr.GetDi(1, 1, 2, p));
    EXPECT_EQ(p.tsMs, received_ts);

    EventBus::Unsubscribe<DIChange>(tok);
}

TEST_F(DataMgrTsDefenseTest, BusinessPointWithNonZeroTsUnchanged) {
    auto& mgr = RemoteDataMgr::Instance();
    uint64_t received_ts = 0;
    auto tok = EventBus::Subscribe<DIChange>(
        [&](const DIChange& e){ received_ts = e.tsMs; });

    // 显式传合法 ts：应原样透传
    ASSERT_TRUE(mgr.SetDi(1, 1, 2, true, 123456789ull, true));
    EXPECT_EQ(received_ts, static_cast<uint64_t>(123456789ull));

    DiPoint p;
    ASSERT_TRUE(mgr.GetDi(1, 1, 2, p));
    EXPECT_EQ(p.tsMs, static_cast<uint64_t>(123456789ull));

    EventBus::Unsubscribe<DIChange>(tok);
}

} // namespace
