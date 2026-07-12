//=============================================================================
// test_pemp_server_stop_lifecycle.cxx
//
// 回归 L6 (code review)：PempServer::stop 以前 detach 客户端线程，随后
// PempServerModule::Stop 调 `delete impl_->server`，被 detach 的线程仍持有
// this 引用，随时可能读到已析构的成员 → use-after-free。
//
// 修复：改为 join。client_handler 的 recv 超时 500ms + 循环检查 running_，
// 保证 stop 最坏 500ms 内所有线程退出，可安全 join。
//=============================================================================

#include "mini_gtest.h"
#include "pemp_server.h"
#include "harness/port_alloc.h"
#include <atomic>
#include <chrono>
#include <thread>

std::atomic<bool> g_running{true};

using pmpc::testing::harness::MakeListener;

namespace {

TEST(PempServerStopLifecycleTest, StopWithoutClientsReturnsPromptly) {
    wsa_guard wsa;
    // 用一个已分配的动态端口，避免 4096 冲突
    auto listener = MakeListener();
    uint16_t port = listener.port;
    listener.sock.close();
    // 等一会让端口释放
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    PempServer srv(port, 5000, 5000);
    PempBind b; b.port = port;
    srv.setBinds({b});
    ASSERT_TRUE(srv.start());
    EXPECT_TRUE(srv.is_running());

    auto t0 = std::chrono::steady_clock::now();
    srv.stop();
    auto dt = std::chrono::steady_clock::now() - t0;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(dt).count();
    EXPECT_LE(ms, static_cast<int64_t>(3000));
    EXPECT_FALSE(srv.is_running());
}

// L6 关键回归：有活跃 client 连接时，stop 也必须能 join 掉，不能 detach 后
// UAF。用 wall-clock 检查 stop 在合理超时内返回。
TEST(PempServerStopLifecycleTest, StopWithLiveClientJoinsBeforeReturn) {
    wsa_guard wsa;
    auto listener = MakeListener();
    uint16_t port = listener.port;
    listener.sock.close();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    PempServer srv(port, 5000, 5000);
    PempBind b; b.port = port;
    srv.setBinds({b});
    ASSERT_TRUE(srv.start());

    // 起一个 client 连上；不发数据，纯粹保持连接
    socket client;
    client.connect("127.0.0.1", port);

    // 让 server 端 accept 完成 + client_handler 进入 recv 循环
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    auto t0 = std::chrono::steady_clock::now();
    srv.stop();
    auto dt = std::chrono::steady_clock::now() - t0;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(dt).count();

    // client_handler recv 超时 500ms，加一些余量；join 应在此时间内完成
    EXPECT_LE(ms, static_cast<int64_t>(3000));
    EXPECT_FALSE(srv.is_running());

    client.close();
}

// 多客户端 stop：仍应及时 join 全部
TEST(PempServerStopLifecycleTest, StopWithMultipleClientsAllJoined) {
    wsa_guard wsa;
    auto listener = MakeListener();
    uint16_t port = listener.port;
    listener.sock.close();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    PempServer srv(port, 5000, 5000);
    PempBind b; b.port = port;
    srv.setBinds({b});
    ASSERT_TRUE(srv.start());

    std::vector<socket> clients(3);
    for (auto& c : clients)
        c.connect("127.0.0.1", port);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    auto t0 = std::chrono::steady_clock::now();
    srv.stop();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    EXPECT_LE(ms, static_cast<int64_t>(3000));
    for (auto& c : clients) c.close();
}

} // namespace
