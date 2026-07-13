//=============================================================================
// test_redundancy_role_transitions.cxx
//
// 回归 C5 相关：pmpc::redundancy::DecideRole 是原 CheckFailover 里的角色
// 决策纯函数版本。这里用表驱动覆盖所有输入组合：peerAlive×role×peerRole×
// missedHeartbeats×priority 关系。
//=============================================================================

#include "mini_gtest.h"
#include "redundancy.h"

using namespace pmpc::redundancy;

namespace {

// ---- 心跳丢失分支 ----

TEST(RedundancyRoleTest, PeerDeadAndMissedBelowLimitStaysInRole) {
    // 少于阈值，还没决定升主
    EXPECT_EQ(static_cast<int>(DecideRole(
                  RedundRole::Standby, RedundRole::Idle, /*peerAlive*/false,
                  /*missed*/3, /*limit*/5, /*locPri*/100, /*peerPri*/50)),
              static_cast<int>(RedundRole::Standby));
    EXPECT_EQ(static_cast<int>(DecideRole(
                  RedundRole::Idle, RedundRole::Idle, false, 3, 5, 100, 50)),
              static_cast<int>(RedundRole::Idle));
}

TEST(RedundancyRoleTest, PeerDeadStandbyPromotesToMaster) {
    EXPECT_EQ(static_cast<int>(DecideRole(
                  RedundRole::Standby, RedundRole::Master, false,
                  /*missed*/5, /*limit*/5, 100, 200)),
              static_cast<int>(RedundRole::Master));
}

TEST(RedundancyRoleTest, PeerDeadIdlePromotesToMaster) {
    EXPECT_EQ(static_cast<int>(DecideRole(
                  RedundRole::Idle, RedundRole::Idle, false, 5, 5, 100, 0)),
              static_cast<int>(RedundRole::Master));
}

TEST(RedundancyRoleTest, PeerDeadMasterStaysMaster) {
    // 已经是 Master，peer 掉线不应主动降级
    EXPECT_EQ(static_cast<int>(DecideRole(
                  RedundRole::Master, RedundRole::Idle, false, 10, 5, 100, 50)),
              static_cast<int>(RedundRole::Master));
}

// ---- 对端在线：初次决策（本机 Idle）----

TEST(RedundancyRoleTest, PeerAliveIdleVsMasterBecomesStandby) {
    EXPECT_EQ(static_cast<int>(DecideRole(
                  RedundRole::Idle, RedundRole::Master, true, 0, 5, 100, 100)),
              static_cast<int>(RedundRole::Standby));
}

TEST(RedundancyRoleTest, PeerAliveHigherLocalPriorityBecomesMaster) {
    // 修复 C4 场景：等 peerPriority 有效值时，优先级决策生效
    EXPECT_EQ(static_cast<int>(DecideRole(
                  RedundRole::Idle, RedundRole::Idle, true, 0, 5,
                  /*locPri*/200, /*peerPri*/100)),
              static_cast<int>(RedundRole::Master));
}

TEST(RedundancyRoleTest, PeerAliveLowerLocalPriorityBecomesStandby) {
    EXPECT_EQ(static_cast<int>(DecideRole(
                  RedundRole::Idle, RedundRole::Idle, true, 0, 5, 50, 100)),
              static_cast<int>(RedundRole::Standby));
}

TEST(RedundancyRoleTest, PeerAliveEqualPriorityStaysStandbyAvoidsDualMaster) {
    // 等 priority：优先留 Standby，交给通信另一端把它自己升为 Master
    EXPECT_EQ(static_cast<int>(DecideRole(
                  RedundRole::Idle, RedundRole::Idle, true, 0, 5, 100, 100)),
              static_cast<int>(RedundRole::Standby));
}

// ---- 双主检测 ----

TEST(RedundancyRoleTest, DualMasterLowerPriorityDemotes) {
    EXPECT_EQ(static_cast<int>(DecideRole(
                  RedundRole::Master, RedundRole::Master, true, 0, 5,
                  /*locPri*/100, /*peerPri*/200)),
              static_cast<int>(RedundRole::Standby));
}

TEST(RedundancyRoleTest, DualMasterHigherPriorityKeepsMaster) {
    EXPECT_EQ(static_cast<int>(DecideRole(
                  RedundRole::Master, RedundRole::Master, true, 0, 5, 200, 100)),
              static_cast<int>(RedundRole::Master));
}

TEST(RedundancyRoleTest, DualMasterEqualPriorityWithoutTieBreakerDemotes) {
    // RD-1（第二轮）语义变更：等 priority 且没有 tie-breaker 时，本机不
    // 再"维持 Master"（旧行为容易导致真的双主，两侧都不降级）。改为
    // localWinsTie() 返回 false → 降 Standby。让运维必须显式配置本机名
    // 与对端名（或让部署时用不同 priority）来避免双 Standby。
    EXPECT_EQ(static_cast<int>(DecideRole(
                  RedundRole::Master, RedundRole::Master, true, 0, 5, 100, 100)),
              static_cast<int>(RedundRole::Standby));
}

TEST(RedundancyRoleTest, DualMasterEqualPriorityTieBreakerLocalWinsKeepsMaster) {
    // 等 priority + tie-breaker 让本机赢 → 保持 Master
    EXPECT_EQ(static_cast<int>(DecideRole(
                  RedundRole::Master, RedundRole::Master, true, 0, 5, 100, 100,
                  /*localTie*/"a", /*peerTie*/"b")),
              static_cast<int>(RedundRole::Master));
}

TEST(RedundancyRoleTest, DualMasterEqualPriorityTieBreakerLocalLosesDemotes) {
    // 等 priority + tie-breaker 让本机输 → 降 Standby
    EXPECT_EQ(static_cast<int>(DecideRole(
                  RedundRole::Master, RedundRole::Master, true, 0, 5, 100, 100,
                  /*localTie*/"z", /*peerTie*/"a")),
              static_cast<int>(RedundRole::Standby));
}

TEST(RedundancyRoleTest, DoubleIdleEqualPriorityTieBreakerLocalWinsBecomesMaster) {
    // RD-1 核心场景：两个 Idle 等 priority，旧代码双方都 Standby → 永不 failover。
    // 新逻辑 tie-breaker 决胜 → 一升 Master 一 Standby。
    EXPECT_EQ(static_cast<int>(DecideRole(
                  RedundRole::Idle, RedundRole::Idle, true, 0, 5, 100, 100,
                  "box_a", "box_b")),
              static_cast<int>(RedundRole::Master));
    EXPECT_EQ(static_cast<int>(DecideRole(
                  RedundRole::Idle, RedundRole::Idle, true, 0, 5, 100, 100,
                  "box_b", "box_a")),
              static_cast<int>(RedundRole::Standby));
}

// ---- 稳定状态（本机 Standby，peer 是 Master）----

TEST(RedundancyRoleTest, StandbyWithMasterAliveStays) {
    EXPECT_EQ(static_cast<int>(DecideRole(
                  RedundRole::Standby, RedundRole::Master, true, 0, 5, 100, 200)),
              static_cast<int>(RedundRole::Standby));
}

// ---- 边界：peerAlive 但 missedHeartbeats 非零 ----
// peerAlive 优先，missed 计数在下一次 CheckFailover 之前会被清零，函数本身
// 应该忽略之。
TEST(RedundancyRoleTest, PeerAliveIgnoresMissedCounter) {
    EXPECT_EQ(static_cast<int>(DecideRole(
                  RedundRole::Standby, RedundRole::Master, /*peerAlive*/true,
                  /*missed*/99, 5, 100, 200)),
              static_cast<int>(RedundRole::Standby));
}

} // namespace
