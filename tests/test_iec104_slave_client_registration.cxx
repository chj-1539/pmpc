//=============================================================================
// test_iec104_slave_client_registration.cxx
//
// 回归 C1 (code review)：Iec104Slave 有个 clients_ 容器供
// SendDIActiveUpload / SendAIActiveUpload / TimerThread 遍历，但 ClientThread
// 从未插入过任何 ClientInfo — 上传路径永远空转，spontaneous DI / cycle AI
// 帧根本不出去。
//
// 修复：ClientThread 用 shared_ptr<ClientInfo> 把自己注册进 clients_，退出
// 前 erase。测试策略：起真 Iec104Slave.Start，用 TCP client connect，检查
// ClientCount() 由 0 变 1；断开后回到 0。
//
// 注意：iec104_slave.h → iec104_master.h → FRAME_START=0x68 与 protocol.h
// (PEMP FRAME_START=0x7B) 冲突，所以本测试不 include pmpc_test_fixture.h。
//=============================================================================

#include "mini_gtest.h"
#include "iec104_slave.h"
#include "harness/port_alloc.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <thread>

std::atomic<bool> g_running{true};

using pmpc::testing::harness::MakeListener;

namespace {

// 建一个最小可用的 iec104_slave.ini
class Iec104SlaveClientTest : public ::testing::Test {
protected:
    void SetUp() override {
        // 端口用系统分配的（bind 0 拿完释放，避免撞车）
        auto probe = MakeListener();
        port_ = probe.port;
        probe.sock.close();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        cfgPath_ = "test_iec104_slave_c1_cfg.ini";
        std::ofstream f(cfgPath_);
        f << "[global]\nport=" << port_ << "\n"
          << "spontaneous_with_timestamp=1\n"
          << "verbose=0\n"
          << "[device_1]\n"
          << "common_addr=1\n"
          << "desc=test\n";
        f.close();
    }

    void TearDown() override {
        std::remove(cfgPath_.c_str());
    }

    uint16_t port_ = 0;
    std::string cfgPath_;
    wsa_guard wsa_;
};

// C1 关键回归：新 client connect 后应出现在 clients_ 里
TEST_F(Iec104SlaveClientTest, ClientRegistersOnConnect) {
    Iec104Slave slv;
    ASSERT_TRUE(slv.LoadConfig(cfgPath_));
    ASSERT_TRUE(slv.Start());

    // 初始状态：无 client
    EXPECT_EQ(slv.ClientCount(), static_cast<size_t>(0));

    // 用 TCP client 连上
    socket client;
    client.connect("127.0.0.1", port_);

    // 给 slave 一点时间 accept + ClientThread 启动 + push_back
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    EXPECT_EQ(slv.ClientCount(), static_cast<size_t>(1));

    // 断开：ClientThread 应在 recv 失败时退出并 erase 自己
    client.close();
    // 等待 ClientThread 处理断开（500ms recv_timeout + erase）
    for (int i = 0; i < 30 && slv.ClientCount() > 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

    EXPECT_EQ(slv.ClientCount(), static_cast<size_t>(0));

    slv.Stop();
}

TEST_F(Iec104SlaveClientTest, MultipleClientsAllRegistered) {
    Iec104Slave slv;
    ASSERT_TRUE(slv.LoadConfig(cfgPath_));
    ASSERT_TRUE(slv.Start());
    EXPECT_EQ(slv.ClientCount(), static_cast<size_t>(0));

    std::vector<socket> clients(3);
    for (auto& c : clients) c.connect("127.0.0.1", port_);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    EXPECT_EQ(slv.ClientCount(), static_cast<size_t>(3));

    // 关一个：应回到 2
    clients[0].close();
    for (int i = 0; i < 30 && slv.ClientCount() > 2; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(slv.ClientCount(), static_cast<size_t>(2));

    for (auto& c : clients) { if (c.is_open()) c.close(); }
    slv.Stop();
}

// Stop() 应清空 clients_
TEST_F(Iec104SlaveClientTest, StopClearsClientList) {
    Iec104Slave slv;
    ASSERT_TRUE(slv.LoadConfig(cfgPath_));
    ASSERT_TRUE(slv.Start());

    socket client;
    client.connect("127.0.0.1", port_);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    ASSERT_EQ(slv.ClientCount(), static_cast<size_t>(1));

    slv.Stop();
    EXPECT_EQ(slv.ClientCount(), static_cast<size_t>(0));

    client.close();
}

} // namespace
