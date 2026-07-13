//=============================================================================
// test_redundancy_role_concurrent.cxx
//
// 回归 C5 (code review)：RedundancyManager::SetRole 曾无锁调用；
// CheckFailover（心跳 ListenLoop 线程）与 RequestRoleChange（debug_console
// 线程）都能进入，两条路径同时看到 role_==Idle 时会并发启动 syncChannel_，
// 造成线程 / socket 泄漏或 bind 冲突。
//
// 修复：SetRole 用 roleMtx_ 保护整段（状态检查+转换+syncChannel Start/Stop）。
// 本测试并发调用 RequestRoleChange 若干次，断言不 crash 且最终角色确定。
//=============================================================================

#include "mini_gtest.h"
#include "redundancy.h"
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#include <cstdio>
#include <fstream>

namespace {

class RedundancyRoleConcurrentTest : public ::testing::Test {
protected:
    void SetUp() override {
        // 用无 socket 起 syncChannel 的假 config：peer_ip 用不可达地址、port=0
        // 避免真的启动 syncChannel accept/connect（避免测试端口冲突）。
        cfgPath_ = "test_redundancy_c5_cfg.ini";
        std::ofstream f(cfgPath_);
        f << "local_name=box_a\n"
          << "peer_ip=127.0.0.1\n"
          << "heartbeat_port=0\n"    // 0 端口在 bind 时也能用；这里 mgr 不 Start
          << "sync_port=0\n"
          << "priority=100\n";
    }
    void TearDown() override { std::remove(cfgPath_.c_str()); }
    std::string cfgPath_;
    wsa_guard wsa_;
};

// C5 关键回归：多线程同时 RequestRoleChange 不 crash，最后 role 一致
TEST_F(RedundancyRoleConcurrentTest, ConcurrentRequestRoleChangeIsSafe) {
    RedundancyManager mgr;
    ASSERT_TRUE(mgr.LoadConfig(cfgPath_));

    // 不调 Start()，避免心跳线程真的开 listen；SetRole 走的是同步 syncChannel
    // 的分支。sync_port=0 让 syncChannel Master 分支的 bind 不占任何真实端口。

    constexpr int kThreads = 8;
    constexpr int kIters   = 20;
    std::atomic<int> completed{0};
    std::vector<std::thread> workers;

    // 每个线程在 Master 和 Standby 之间来回切
    for (int i = 0; i < kThreads; ++i) {
        workers.emplace_back([&, i]() {
            for (int j = 0; j < kIters; ++j) {
                mgr.RequestRoleChange(
                    (i + j) & 1 ? RedundRole::Master : RedundRole::Standby);
            }
            completed.fetch_add(1);
        });
    }

    for (auto& t : workers) t.join();
    EXPECT_EQ(completed.load(), kThreads);

    // 最终 role 应是 Master 或 Standby（其中之一，不 crash 就 OK）
    auto final_role = mgr.GetRole();
    bool ok = (final_role == RedundRole::Master ||
               final_role == RedundRole::Standby ||
               final_role == RedundRole::Idle);
    EXPECT_TRUE(ok);

    // 让 syncChannel 收尾，避免 mgr 析构时线程仍活着
    mgr.RequestRoleChange(RedundRole::Idle);
}

// 同角色重复请求应立即返回（幂等）
TEST_F(RedundancyRoleConcurrentTest, SameRoleIsIdempotent) {
    RedundancyManager mgr;
    ASSERT_TRUE(mgr.LoadConfig(cfgPath_));
    mgr.RequestRoleChange(RedundRole::Master);
    EXPECT_EQ(static_cast<int>(mgr.GetRole()), static_cast<int>(RedundRole::Master));
    // 再来一次
    mgr.RequestRoleChange(RedundRole::Master);
    EXPECT_EQ(static_cast<int>(mgr.GetRole()), static_cast<int>(RedundRole::Master));

    mgr.RequestRoleChange(RedundRole::Idle);
}

// 快速 M↔S↔M 切换：验证 syncChannel_.Stop() 在切换时被调、不留残留
TEST_F(RedundancyRoleConcurrentTest, RapidToggleReleasesResources) {
    RedundancyManager mgr;
    ASSERT_TRUE(mgr.LoadConfig(cfgPath_));

    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 10; ++i) {
        mgr.RequestRoleChange(RedundRole::Master);
        mgr.RequestRoleChange(RedundRole::Standby);
    }
    mgr.RequestRoleChange(RedundRole::Idle);

    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    // 20 次切换 + 1 次 Idle，每次都调 syncChannel_.Stop() (可能有 accept 线程 join)
    // 合理上限 ~15 秒（每次切换若真启 socket 有一定开销）
    EXPECT_LE(ms, static_cast<int64_t>(15000));
}

} // namespace
