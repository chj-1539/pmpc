//=============================================================================
// test_data_recorder_helpers.cxx
//
// 回归 CR-2（第二轮代码审查）：DataRecorder::TimerThread 在 aiIntervalMs_
// 配置为 0/负数时进入 sleep_for(0ms) 忙等 100% CPU，且随后
// `tickCount % (120000 / aiIntervalMs_)` 除以零 → SIGFPE。
//
// 修复：把 clamp 逻辑抽到 data_recorder_helpers.h 里的两个纯函数：
//   * ClampAiIntervalMs(cfg) — < 100 → 100
//   * ComputeCleanupPeriodTicks(effective) — 若 120000/effective < 1 → 1
//
// 这两个 helper 无 mysql.h 依赖，可直接零依赖单元测试。TimerThread 里
// 双双使用它们。
//=============================================================================

#include "mini_gtest.h"
#include "data_recorder_helpers.h"

using pmpc::data_recorder::ClampAiIntervalMs;
using pmpc::data_recorder::ComputeCleanupPeriodTicks;

// ── ClampAiIntervalMs ──────────────────────────────────────────────────────

TEST(ClampAiIntervalMsTest, ZeroGetsClampedTo100) {
    EXPECT_EQ(ClampAiIntervalMs(0), 100);
}

TEST(ClampAiIntervalMsTest, NegativeGetsClampedTo100) {
    EXPECT_EQ(ClampAiIntervalMs(-1),    100);
    EXPECT_EQ(ClampAiIntervalMs(-9999), 100);
}

TEST(ClampAiIntervalMsTest, JustBelow100GetsClamped) {
    EXPECT_EQ(ClampAiIntervalMs(1),  100);
    EXPECT_EQ(ClampAiIntervalMs(99), 100);
}

TEST(ClampAiIntervalMsTest, AtOrAbove100Unchanged) {
    EXPECT_EQ(ClampAiIntervalMs(100),    100);
    EXPECT_EQ(ClampAiIntervalMs(200),    200);
    EXPECT_EQ(ClampAiIntervalMs(5000),   5000);
    EXPECT_EQ(ClampAiIntervalMs(300000), 300000);
}

// ── ComputeCleanupPeriodTicks ──────────────────────────────────────────────

TEST(ComputeCleanupPeriodTicksTest, TypicalIntervalYieldsExpectedTicks) {
    // 5000 ms 间隔 → 120000/5000 = 24 tick / 2 分钟
    EXPECT_EQ(ComputeCleanupPeriodTicks(5000), 24);
    // 1000 ms → 120 ticks
    EXPECT_EQ(ComputeCleanupPeriodTicks(1000), 120);
    // 100 ms（clamp 下限）→ 1200 ticks
    EXPECT_EQ(ComputeCleanupPeriodTicks(100),  1200);
}

TEST(ComputeCleanupPeriodTicksTest, IntervalLargerThan120000FloorsTo1) {
    // 关键：120000 / 300000 = 0，`% 0` UB；返回 1 保底
    EXPECT_EQ(ComputeCleanupPeriodTicks(300000), 1);
    EXPECT_EQ(ComputeCleanupPeriodTicks(120001), 1);
}

TEST(ComputeCleanupPeriodTicksTest, ExactlyDivisibleBoundary) {
    // 120000 / 120000 = 1，恰好边界
    EXPECT_EQ(ComputeCleanupPeriodTicks(120000), 1);
}

TEST(ComputeCleanupPeriodTicksTest, ZeroInputDoesNotDivideByZero) {
    // 上游 Clamp 后不应传入 0，但 helper 自己也要防御，返回 1
    EXPECT_EQ(ComputeCleanupPeriodTicks(0),  1);
    EXPECT_EQ(ComputeCleanupPeriodTicks(-1), 1);
}

// ── 组合语义：Clamp 后再算 tick 数一定安全 ────────────────────────────────

TEST(TimerConfigCombinedTest, ClampFirstThenComputeIsAlwaysSafe) {
    // 生产 TimerThread 的调用序列：先 Clamp，再 Compute
    const int badCfgs[] = { -1000, 0, 1, 99 };
    for (int cfg : badCfgs) {
        int eff = ClampAiIntervalMs(cfg);
        EXPECT_GE(eff, 100);
        int p = ComputeCleanupPeriodTicks(eff);
        EXPECT_GE(p, 1);
    }
    // 极大配置也不会崩
    int eff = ClampAiIntervalMs(999999);
    EXPECT_EQ(eff, 999999);
    int p = ComputeCleanupPeriodTicks(eff);
    EXPECT_EQ(p, 1);
}
