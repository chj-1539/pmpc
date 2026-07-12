//=============================================================================
// test_modbus_ao_epsilon.cxx
//
// 回归 H9 (code review)：Modbus master WriteAOChanges 用绝对容差 0.001
// 判定 "值几乎相等"，对量程差异大的场景不友好：
//   * ~1e9 的量：0.001 远小于浮点精度，几乎每次 poll 都判"变化"，一直写
//   * ~1e-6 的量：两个明显不同的值也判为"相同"，写不出
// 修复：ModbusTcpMaster::AoAlmostEqual(a, b) 用相对+绝对容差混合：
//     |a - b| <= max(absTol=1e-6, relTol=1e-4 * max(|a|, |b|))
//=============================================================================

#include "mini_gtest.h"
#include "modbus_tcp_master.h"

namespace {

// 便捷别名
constexpr auto Eq = &ModbusTcpMaster::AoAlmostEqual;

TEST(AoAlmostEqualTest, IdenticalValuesEqual) {
    EXPECT_TRUE(Eq(0.0, 0.0, 1e-6, 1e-4));
    EXPECT_TRUE(Eq(42.0, 42.0, 1e-6, 1e-4));
    EXPECT_TRUE(Eq(-1.5, -1.5, 1e-6, 1e-4));
}

TEST(AoAlmostEqualTest, SmallScaleUsesAbsTolerance) {
    // 数量级 ~1e-6：应主要按 absTol=1e-6 判
    EXPECT_TRUE (Eq(0.0000001, 0.0000005, 1e-6, 1e-4)); // 差 4e-7 < abs 1e-6
    EXPECT_FALSE(Eq(0.0,        0.001,     1e-6, 1e-4)); // 差 1e-3 > abs 1e-6
}

TEST(AoAlmostEqualTest, LargeScaleUsesRelativeTolerance) {
    // 数量级 ~1e9：绝对差 100 但相对差 1e-7 < relTol=1e-4，应算相等
    EXPECT_TRUE(Eq(1e9,        1e9 + 100.0, 1e-6, 1e-4));
    // 数量级 ~1e9：绝对差 1e6 相对差 1e-3 > relTol，算变化
    EXPECT_FALSE(Eq(1e9,       1e9 + 1e6,   1e-6, 1e-4));
}

TEST(AoAlmostEqualTest, BoundaryAtAbsToleranceExact) {
    // 差恰好等于 absTol → 相等
    EXPECT_TRUE(Eq(0.0, 1e-6, 1e-6, 1e-4));
    EXPECT_FALSE(Eq(0.0, 1e-6 * 1.001, 1e-6, 1e-4));
}

TEST(AoAlmostEqualTest, BoundaryAtRelativeToleranceExact) {
    // scale = 1000, relTol=1e-4 → 阈值 0.1
    EXPECT_TRUE(Eq(1000.0, 1000.1, 1e-6, 1e-4));
    EXPECT_FALSE(Eq(1000.0, 1000.2, 1e-6, 1e-4));
}

TEST(AoAlmostEqualTest, NegativeValuesHandled) {
    // 相同量级正负两侧
    EXPECT_TRUE (Eq(-100.0, -100.0000001, 1e-6, 1e-4));
    EXPECT_FALSE(Eq(-100.0, -101.0,       1e-6, 1e-4));
}

TEST(AoAlmostEqualTest, MixedSignsUseLargerMagnitudeScale) {
    // a=-1000, b=1000, scale = 1000, relTol=1e-4 → 阈值 0.1
    // 差 2000 >> 0.1 → 不等
    EXPECT_FALSE(Eq(-1000.0, 1000.0, 1e-6, 1e-4));
}

TEST(AoAlmostEqualTest, DefaultToleranceIsSensible) {
    // 用默认参数：absTol=1e-6, relTol=1e-4
    EXPECT_TRUE(ModbusTcpMaster::AoAlmostEqual(3.14, 3.14));
    EXPECT_TRUE(ModbusTcpMaster::AoAlmostEqual(1e6, 1e6 + 0.05));   // 相对 5e-8
    EXPECT_FALSE(ModbusTcpMaster::AoAlmostEqual(3.14, 3.15));       // 差 0.01
}

// 老代码病症再现：|1e9 - 1e9| 因浮点噪声 >0.001 → 判"变化"
// 修复后：这两个应算相等（相对 <1e-4）
TEST(AoAlmostEqualTest, LargeMagnitudeFloatingPointNoiseTreatedEqual) {
    // 模拟 1e9 附近的浮点漂移
    double a = 1e9;
    double b = 1e9 + 50.0;   // 相对差 5e-8，小于 1e-4
    EXPECT_TRUE(ModbusTcpMaster::AoAlmostEqual(a, b));
    // 老代码 std::abs(a-b) = 50 > 0.001 → 判变化 → 无谓重发
}

} // namespace
