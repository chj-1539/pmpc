//=============================================================================
// test_data_mgr_char.cxx — 特征化测试（Phase B）
//
// 这些测试**不修复 bug**，只用断言锁定当前（有争议）行为，防止悄悄漂移。
// 每个 TEST 都携带 TODO(<bug-id>) 注释，未来行为真正修好时需要翻转断言。
//
// 覆盖：
//   * C3 (code review): Set{Ai,Do,Ao} 无差别发布 EventBus，即使值相同
//   * H11:              aoChangeQueue_ 只有生产者、无消费者，会累积到 10000 上限
//   * L4:               [Channel_N] 重复 section 被静默合并
//=============================================================================

#include "mini_gtest.h"
#include "pmpc_test_fixture.h"
#include "pmpc.h"
#include "event_bus.h"
#include <atomic>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

using pmpc::testing::GlobalStateFixture;

namespace {

class DataMgrCharTest : public GlobalStateFixture {
protected:
    void SetUp() override {
        GlobalStateFixture::SetUp();
        cfgPath_ = "test_data_mgr_char_cfg.ini";
    }
    void TearDown() override {
        std::remove(cfgPath_.c_str());
        GlobalStateFixture::TearDown();
    }

    void LoadSingleDevConfig() {
        std::ofstream f(cfgPath_);
        f << "[Channel_1]\n" << "Dev_1=2,3,2,3\n";   // DI 2 / AI 3 / DO 2 / AO 3
        f.close();
        RemoteDataMgr::Instance().LoadConfig(cfgPath_);
    }

    std::string cfgPath_;
};

// ─── C3 特征化 ─────────────────────────────────────────────────────
// 现状：SetAi 每次调用都发 AIChange，无论值是否变化。SetDi 已经做了变化
// 门控（bug #2 修复），这里锁定 SetAi 的老行为。
// TODO(C3): 与 SetDi 对齐，只在 value 真变化时发布。届时翻转断言。
TEST_F(DataMgrCharTest, TODO_SetAiAlwaysPublishesEvenOnSameValue) {
    LoadSingleDevConfig();
    std::atomic<int> events{0};
    auto tok = EventBus::Subscribe<AIChange>(
        [&](const AIChange&) { events++; });

    auto& mgr = RemoteDataMgr::Instance();
    ASSERT_TRUE(mgr.SetAi(1, 1, 1, 42.0));
    EXPECT_EQ(events.load(), 1);
    ASSERT_TRUE(mgr.SetAi(1, 1, 1, 42.0));   // 相同值
    // 当前行为：仍然发布。TODO(C3): 应改为不发。
    EXPECT_EQ(events.load(), 2);
    ASSERT_TRUE(mgr.SetAi(1, 1, 1, 42.0));   // 再一次
    EXPECT_EQ(events.load(), 3);

    EventBus::Unsubscribe<AIChange>(tok);
}

// 现状：SetDoMaster 每次调用都发 DOChange，无论值是否变化。
// TODO(C3): 应仅在 masterVal 变化时发。
TEST_F(DataMgrCharTest, TODO_SetDoMasterAlwaysPublishesEvenOnSameValue) {
    LoadSingleDevConfig();
    std::atomic<int> events{0};
    auto tok = EventBus::Subscribe<DOChange>(
        [&](const DOChange&) { events++; });

    auto& mgr = RemoteDataMgr::Instance();
    ASSERT_TRUE(mgr.SetDoMaster(1, 1, 1, true));
    EXPECT_EQ(events.load(), 1);
    ASSERT_TRUE(mgr.SetDoMaster(1, 1, 1, true));   // 同值
    EXPECT_EQ(events.load(), 2);   // TODO(C3): 应仍为 1

    EventBus::Unsubscribe<DOChange>(tok);
}

// 现状：SetAo 每次调用都发 AOChange。
// TODO(C3): 应仅在 value 变化时发。
TEST_F(DataMgrCharTest, TODO_SetAoAlwaysPublishesEvenOnSameValue) {
    LoadSingleDevConfig();
    std::atomic<int> events{0};
    auto tok = EventBus::Subscribe<AOChange>(
        [&](const AOChange&) { events++; });

    auto& mgr = RemoteDataMgr::Instance();
    ASSERT_TRUE(mgr.SetAo(1, 1, 1, 3.14));
    EXPECT_EQ(events.load(), 1);
    ASSERT_TRUE(mgr.SetAo(1, 1, 1, 3.14));   // 同值
    EXPECT_EQ(events.load(), 2);   // TODO(C3): 应仍为 1

    EventBus::Unsubscribe<AOChange>(tok);
}

// ─── H11 特征化 ─────────────────────────────────────────────────────
// aoChangeQueue_ 原设计供冗余同步通道消费；当前代码里没有任何消费者，只
// 有 SetAo 一个生产者。修复历史里应该由冗余模块 pop 掉，但那段代码没写。
// 结果：SetAo 累积到 10000 上限，pop_front 挤掉最老的。
// TODO(H11): 引入真正消费者（冗余同步通道或明确的 drain API）。
TEST_F(DataMgrCharTest, TODO_AoChangeQueueGrowsWithoutConsumer) {
    LoadSingleDevConfig();
    auto& mgr = RemoteDataMgr::Instance();

    EXPECT_EQ(mgr.PendingAoChangeCount(), static_cast<size_t>(0));

    for (int i = 0; i < 100; ++i)
        ASSERT_TRUE(mgr.SetAo(1, 1, 1, static_cast<double>(i)));

    // 100 次 SetAo，无人消费 → 队列长度 100
    EXPECT_EQ(mgr.PendingAoChangeCount(), static_cast<size_t>(100));
}

// 现状：SetAo 越过 10000 上限后，pop_front 挤掉最老的。数据静默丢失。
// TODO(H11): 应有告警或阻塞，别静默丢。
TEST_F(DataMgrCharTest, TODO_AoChangeQueueCapsAt10000AndDropsOldest) {
    LoadSingleDevConfig();
    auto& mgr = RemoteDataMgr::Instance();

    for (int i = 0; i < 10500; ++i)
        ASSERT_TRUE(mgr.SetAo(1, 1, 1, static_cast<double>(i)));

    // 上限 10000，pop_front 掉了最老的 500
    EXPECT_EQ(mgr.PendingAoChangeCount(), static_cast<size_t>(10000));
}

// ─── L4 修复回归 ───────────────────────────────────────────────────
// 重复的 [Channel_N] 节头：新行为是拒绝第二个及以后的同名节头，报 error，
// 并跳过其后的 Dev_M 行，避免被误合并到第一个通道。
TEST_F(DataMgrCharTest, DuplicateChannelSectionRejectsExtras) {
    // 两个 [Channel_1]，各带不同设备 —— 期望只有第一个的 Dev_1 生效
    std::ofstream f(cfgPath_);
    f << "[Channel_1]\n" << "Dev_1=1,1,0,0\n";
    f << "[Channel_1]\n" << "Dev_2=2,2,0,0\n";   // 应被拒绝：所在段被忽略
    f.close();
    RemoteDataMgr::Instance().LoadConfig(cfgPath_);

    auto& mgr = RemoteDataMgr::Instance();
    auto devIds = mgr.GetDeviceIds(1);
    // 新行为：只有第一段的 Dev_1 生效
    ASSERT_EQ(devIds.size(), static_cast<size_t>(1));
    EXPECT_EQ(devIds[0], static_cast<uint16_t>(1));

    // 通道总数也只有 1 个
    auto chIds = mgr.GetChannelIds();
    EXPECT_EQ(chIds.size(), static_cast<size_t>(1));
}

} // namespace
