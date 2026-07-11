//=============================================================================
// test_event_bus_isolated_include.cxx
//
// 回归测试 C6：event_bus.h 之前用了 std::remove_if 但未包含 <algorithm>，
// 只是通过其他头文件的传递包含才能在部分 TU 中编译过。这个测试文件除
// event_bus.h 外不包含任何其他 STL 算法头，若 event_bus.h 未自足，此文件
// 就无法编译。
//
// 用法：build_tests.bat 会尝试编译本文件；编译失败即回归。运行时只做一次
// Subscribe/Publish/Unsubscribe 的 smoke check。
//=============================================================================

#include "event_bus.h"
#include "mini_gtest.h"

namespace {

struct HelloEvent { int n; };

TEST(EventBusIsolatedIncludeTest, CompilesWithoutTransitiveAlgorithm) {
    int seen = 0;
    auto tok = EventBus::Subscribe<HelloEvent>([&](const HelloEvent& e) {
        seen += e.n;
    });
    EventBus::Publish(HelloEvent{7});
    EventBus::Publish(HelloEvent{3});
    EXPECT_EQ(seen, 10);

    // Unsubscribe 内部用 std::remove_if — 这个调用点是 C6 的实际触发路径
    EventBus::Unsubscribe<HelloEvent>(tok);
    EventBus::Publish(HelloEvent{100});
    EXPECT_EQ(seen, 10);  // Unsubscribe 生效后不再累加
}

} // namespace
