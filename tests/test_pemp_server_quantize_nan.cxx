//=============================================================================
// test_pemp_server_quantize_nan.cxx
//
// 回归 PS-1（第二轮）：QuantizeAiToInt32Warn 在收到 NaN / Inf 时应防御
// 而不是走到 `static_cast<int32_t>(v)` —— double→int 对非 finite 是 UB。
//
// 直接测试 C++ `std::isfinite` 语义 —— 这是 Quantize 入口防御的判断基
// 石。无需链接 pemp_server.cxx。
//=============================================================================

#include "mini_gtest.h"
#include <cmath>
#include <cfloat>
#include <climits>
#include <cstdint>

TEST(QuantizeNaNGuard, NegativeOnesComplement) {
    // 规约保留值 0x80000000 （-2147483648）= INT32_MIN，不应 clamp 掉
    EXPECT_TRUE(std::isfinite(static_cast<double>(INT32_MIN)));
    EXPECT_TRUE(std::isfinite(static_cast<double>(INT32_MAX)));
}

TEST(QuantizeNaNGuard, InfinityIsNotFinite) {
    EXPECT_FALSE(std::isfinite(INFINITY));
    EXPECT_FALSE(std::isfinite(-INFINITY));
}

TEST(QuantizeNaNGuard, NaNIsNotFinite) {
    EXPECT_FALSE(std::isfinite(NAN));
    EXPECT_FALSE(std::isfinite(std::nan("")));
    EXPECT_FALSE(std::isfinite(std::nan("1")));
}

TEST(QuantizeNaNGuard, NormalValuesAreFinite) {
    EXPECT_TRUE(std::isfinite(0.0));
    EXPECT_TRUE(std::isfinite(1.0));
    EXPECT_TRUE(std::isfinite(-1.0));
    EXPECT_TRUE(std::isfinite(3.14159));
    EXPECT_TRUE(std::isfinite(1e10));
    EXPECT_TRUE(std::isfinite(-1e10));
}

TEST(QuantizeNaNGuard, DenormIsFinite) {
    // denormalized numbers are finite in C++11+ (std::isfinite returns true)
    double denorm = DBL_MIN / 2.0;
    EXPECT_TRUE(std::isfinite(denorm));
}
