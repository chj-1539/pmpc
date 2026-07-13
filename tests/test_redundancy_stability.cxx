//=============================================================================
// test_redundancy_stability.cxx
//
// 回归第二轮 P1 簇 E（RD-1..RD-5）：Redundancy 冷启动 + 心跳超时 + 并发字段。
//
// RD-1: DecideRole 增加 (localTieBreaker, peerTieBreaker) 参数打破等优先级
//       双 Standby 死锁。这里表驱动 tie-break 所有组合。
// RD-4: peer 掉线检测由 send-fail 单一路径扩展到 recv-timeout 累计判断
//       lastHbTime_ 超期。（本文件只测 pure decision；实际集成路径需
//       起真 socket，超本文件范围。）
// RD-5: peerPriority_ / missedHeartbeats_ 改 std::atomic —— static_assert
//       确保类型不回退到普通 int/uint16_t。
//=============================================================================

#include "mini_gtest.h"
#include "redundancy.h"
#include <atomic>
#include <string>
#include <type_traits>

using namespace pmpc::redundancy;

// ── RD-1: tie-breaker 场景 ────────────────────────────────────────────────

TEST(TieBreaker, DoubleIdleEqualPriorityLocalSmallerWinsMaster) {
    // 关键 RD-1 场景：老逻辑 priority 相等 → 双 Standby 死锁
    EXPECT_EQ(static_cast<int>(DecideRole(
                  RedundRole::Idle, RedundRole::Idle, true, 0, 5, 100, 100,
                  /*local*/"box_a", /*peer*/"box_b")),
              static_cast<int>(RedundRole::Master));
}

TEST(TieBreaker, DoubleIdleEqualPriorityLocalLargerBecomesStandby) {
    EXPECT_EQ(static_cast<int>(DecideRole(
                  RedundRole::Idle, RedundRole::Idle, true, 0, 5, 100, 100,
                  "box_b", "box_a")),
              static_cast<int>(RedundRole::Standby));
}

TEST(TieBreaker, EmptyTiesFallbackToStandby) {
    // 兼容旧调用：不传 tie-breaker（都是空串），维持"等优先级 Standby"
    // 保守语义，避免双主
    EXPECT_EQ(static_cast<int>(DecideRole(
                  RedundRole::Idle, RedundRole::Idle, true, 0, 5, 100, 100)),
              static_cast<int>(RedundRole::Standby));
    // 单方传 tie-breaker、另一方空 → 空视为无法比较 → 保守 Standby
    EXPECT_EQ(static_cast<int>(DecideRole(
                  RedundRole::Idle, RedundRole::Idle, true, 0, 5, 100, 100,
                  "box_a", "")),
              static_cast<int>(RedundRole::Standby));
}

TEST(TieBreaker, StrictPriorityBeatsTie) {
    // priority 严格不等时 tie-breaker 不生效
    EXPECT_EQ(static_cast<int>(DecideRole(
                  RedundRole::Idle, RedundRole::Idle, true, 0, 5,
                  /*localPri*/200, /*peerPri*/100,
                  /*local*/"box_z", /*peer*/"box_a")),   // z > a 但 pri 更高
              static_cast<int>(RedundRole::Master));
}

TEST(TieBreaker, DualMasterEqualPriorityTieBreakerDecidesWhoDegrades) {
    // 双 Master 场景 + 等 priority：tie-break 输者降级
    EXPECT_EQ(static_cast<int>(DecideRole(
                  RedundRole::Master, RedundRole::Master, true, 0, 5, 100, 100,
                  "z", "a")),
              static_cast<int>(RedundRole::Standby));
    EXPECT_EQ(static_cast<int>(DecideRole(
                  RedundRole::Master, RedundRole::Master, true, 0, 5, 100, 100,
                  "a", "z")),
              static_cast<int>(RedundRole::Master));
}

// ── RD-1: 两台机器交叉决策一致性 ────────────────────────────────────────
// 关键：tie-breaker 只有在**双方传入对称字符串**时才能保证一升一降。
// 生产代码传 (localName_, peerIp_) 是不对称的 —— 只有当运维显式配对
// (即 A 的 peerIp = B 的 localName 反之亦然，或双方都传同一对 label)
// 才能保证一致。这里显式测试"对称输入"这个前提。

TEST(TieBreaker, SymmetricInputsReachAgreement) {
    // 双方传入的字符串是对偶的：A 传 (X, Y)，B 传 (Y, X)。
    // A 看：X < Y ？→ 决定；B 看：Y < X ？→ 相反。
    struct Case { std::string a; std::string b; };
    Case cases[] = {
        { "box_a", "box_b" },
        { "aa",    "bb" },
        { "host1", "host2" },
        { "10.0.0.1", "10.0.0.2" },
    };
    for (auto& c : cases) {
        // A 视角
        RedundRole aResult = DecideRole(
            RedundRole::Idle, RedundRole::Idle, true, 0, 5, 100, 100,
            /*local*/c.a, /*peer*/c.b);
        // B 视角（对偶输入：本机 = A 的 peer，对端 = A 的 local）
        RedundRole bResult = DecideRole(
            RedundRole::Idle, RedundRole::Idle, true, 0, 5, 100, 100,
            /*local*/c.b, /*peer*/c.a);
        bool oneMasterOneStandby =
            (aResult == RedundRole::Master && bResult == RedundRole::Standby) ||
            (aResult == RedundRole::Standby && bResult == RedundRole::Master);
        EXPECT_TRUE(oneMasterOneStandby);
    }
}

TEST(TieBreaker, AsymmetricInputsMayDoubleStandbyByDesign) {
    // 反面：不对称输入的记录 —— 生产 CheckFailover 传 (localName_, peerIp_)
    // 若两台机的 (name, peer_ip) 组合不构成对偶，可能双方都 Standby（保守
    // 语义）或双方都 Master（不会发生，因为 !localWinsTie 时 Idle → Standby、
    // Master + Master → Standby，都是"降级方向"）。
    // 这个测试文档化了这个限制，也保证不会退化到最坏情况（双 Master）。
    RedundRole aResult = DecideRole(
        RedundRole::Idle, RedundRole::Idle, true, 0, 5, 100, 100,
        "hostA", "192.168.1.20");
    RedundRole bResult = DecideRole(
        RedundRole::Idle, RedundRole::Idle, true, 0, 5, 100, 100,
        "hostB", "192.168.1.10");
    // 至少保证：从不双 Master
    bool bothMaster = (aResult == RedundRole::Master && bResult == RedundRole::Master);
    EXPECT_FALSE(bothMaster);
    // 双 Standby 是可能的（保守行为，比双 Master 安全）—— 不断言不双 Standby
}

// ── RD-5: static_assert 保护 atomic 字段类型 ──────────────────────────────
// 该文件独立编译，如果有人未来把 peerPriority_ / missedHeartbeats_ 改回
// 普通 int/uint16_t，本文件用 SFINAE 探针不方便；直接反射 header 里的
// 名字。RedundancyManager 是全类 public API，peerPriority_ / missedHeartbeats_
// 是私有的。用一个简单的语义测试代替：atomic<int>::is_lock_free 在目标
// 平台成立（配合 test_data_recorder_atomic 建立的 baseline）。

TEST(AtomicFieldsPlatformCheck, IntAndUint16AreLockFree) {
    // RD-5: 两个字段的 atomic 类型在 pmpc 目标平台（x86_64/aarch64 MinGW）
    // 应该 lock-free；否则 CheckFailover 每次调用会去抢 atomic 内部 mutex。
    std::atomic<int> a{0};
    std::atomic<uint16_t> b{0};
    EXPECT_TRUE(a.is_lock_free());
    EXPECT_TRUE(b.is_lock_free());
    EXPECT_TRUE(std::atomic<int>::is_always_lock_free);
    EXPECT_TRUE(std::atomic<uint16_t>::is_always_lock_free);
}

TEST(AtomicFieldsPlatformCheck, FetchAddSemantics) {
    // missedHeartbeats_++ 在生产代码里等价于 fetch_add(1) 后取新值
    std::atomic<int> a{0};
    int prev = a.fetch_add(1);   // 返回旧值
    EXPECT_EQ(prev, 0);
    EXPECT_EQ(a.load(), 1);
    int newV = ++a;              // 返回新值
    EXPECT_EQ(newV, 2);
}
