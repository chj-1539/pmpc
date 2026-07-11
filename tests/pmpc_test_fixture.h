//=============================================================================
// pmpc_test_fixture.h — 测试全局状态隔离基类
//
// 用途：pmpc 有多个进程级单例/全局对象（EventBus 静态 handler map、
//       RemoteDataMgr::Instance()、g_soeQueue、Logger 全局级别），
//       单元测试之间必须显式清理，否则一旦启用 --gtest_shuffle 就会大规模 flake。
//
// 用法：
//   #include "pmpc_test_fixture.h"
//   class MyTest : public pmpc::testing::GlobalStateFixture {
//     protected:
//       void SetUp() override {
//           GlobalStateFixture::SetUp();
//           // ...自己的初始化...
//       }
//       // 或者不覆盖 SetUp/TearDown，用继承即可。
//   };
//
//   TEST_F(MyTest, Foo) { ... }
//
// 也可以直接 TEST_F(pmpc::testing::GlobalStateFixture, Foo) — 不需要子类。
//=============================================================================

#ifndef PMPC_TEST_FIXTURE_H
#define PMPC_TEST_FIXTURE_H

#include "mini_gtest.h"
#include "event_bus.h"
#include "pmpc.h"
#include "soe_queue.h"
#include "logger.h"

namespace pmpc {
namespace testing {

class GlobalStateFixture : public ::testing::Test {
public:
    void SetUp() override {
        // 前置清扫：进入本测试前若上一个测试遗留了订阅/数据，先清。
        ResetAll();
    }

    void TearDown() override {
        // 后置清扫：本测试自身可能忘记 Unsubscribe / ClearAll，
        // 兜底一次以免污染下一个测试。
        ResetAll();
    }

    /// 手动重置。适用于测试内部需要在中途重置的场景（少见）。
    static void ResetAll() {
        // 事件总线：清空所有 handler 表。生产不应调用。
        EventBus::Clear();

        // 四遥数据管理器：清空所有通道/设备。见 pmpc_data_mgr.cxx:88。
        RemoteDataMgr::Instance().ClearAll();

        // SOE 队列：全局实例，多个采集/上送模块共享。
        g_soeQueue.Clear();

        // Logger 全局级别：Phase-A 的很多测试会改 Logger::SetLevel；
        // 恢复到 INFO 默认，避免污染。
        Logger::SetLevel(Logger::INFO);
    }
};

} // namespace testing
} // namespace pmpc

#endif // PMPC_TEST_FIXTURE_H
