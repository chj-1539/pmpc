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

TEST(RedundancyRoleTest, DualMasterEqualPriorityBothKeepMaster) {
    // 等 priority 的双主一方不会主动降级（要么由 priority 决胜、要么外部干预）
    // —— 记录当前行为，避免误改
    EXPECT_EQ(static_cast<int>(DecideRole(
                  RedundRole::Master, RedundRole::Master, true, 0, 5, 100, 100)),
              static_cast<int>(RedundRole::Master));
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
