//=============================================================================
// test_socket_timeout.cxx
//
// 回归 CLAUDE.md bug #4：TCP socket 超时 vs 对端关闭的语义区分。
//
// 早期代码把 recv 抛异常和 recv 返回 0 混为一谈。修复约定：
//   * 超时 → 抛 socket_error(socket_errc::timeout)，socket 仍处于 is_open 状态；
//   * 对端 close → recv 返回 0；is_open 仍为 true（socket 由本地负责 close）。
// 用一个真实 loopback pair 直接测出这两条。
//=============================================================================

#include "mini_gtest.h"
#include "socket.h"
#include "harness/port_alloc.h"
#include <chrono>
#include <thread>

using pmpc::testing::harness::MakeListener;

namespace {

// 所有 socket 用例都需要 wsa_guard 覆盖生命周期
class SocketTimeoutTest : public ::testing::Test {
protected:
    wsa_guard wsa;
};

TEST_F(SocketTimeoutTest, RecvTimeoutThrowsTimeoutErrorSocketRemainsOpen) {
    auto listener = MakeListener();

    // 服务端 accept 但不写数据，模拟"链路慢/对端不响应"
    std::thread server([&]() {
        socket_addr peer;
        socket s = listener.sock.accept(&peer);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        s.close();
    });

    socket client;
    client.connect("127.0.0.1", listener.port);
    client.set_recv_timeout(std::chrono::milliseconds(100));

    uint8_t buf[16];
    bool gotTimeout = false;
    try {
        (void)client.recv(buf, sizeof(buf));
    } catch (const socket_error& e) {
        if (e.code() == socket_errc::timeout) gotTimeout = true;
    }
    EXPECT_TRUE(gotTimeout);
    EXPECT_TRUE(client.is_open());  // 关键：超时后 socket 仍可再次 recv

    client.close();
    listener.sock.close();
    server.join();
}

TEST_F(SocketTimeoutTest, PeerCloseReturnsZeroRecv) {
    auto listener = MakeListener();

    // 服务端 accept 后立即 close
    std::thread server([&]() {
        socket_addr peer;
        socket s = listener.sock.accept(&peer);
        s.close();
    });

    socket client;
    client.connect("127.0.0.1", listener.port);
    client.set_recv_timeout(std::chrono::milliseconds(1000));

    // 让服务端有机会 close
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    uint8_t buf[16];
    size_t n = client.recv(buf, sizeof(buf));
    EXPECT_EQ(n, static_cast<size_t>(0));  // EOF：对端正常关闭
    // 语义：对端 close 后 socket 本地状态仍是 open，需要调用者主动 close
    EXPECT_TRUE(client.is_open());

    client.close();
    listener.sock.close();
    server.join();
}

TEST_F(SocketTimeoutTest, RecvTimeoutSocketRemainsUsableAfterRetry) {
    // 超时不应"污染"socket：再等的话，若数据到了应正常收到
    auto listener = MakeListener();

    std::thread server([&]() {
        socket_addr peer;
        socket s = listener.sock.accept(&peer);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        const uint8_t msg[] = "HELLO";
        s.send(msg, 5);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        s.close();
    });

    socket client;
    client.connect("127.0.0.1", listener.port);
    client.set_recv_timeout(std::chrono::milliseconds(100));

    uint8_t buf[16];
    // 第一次：超时
    bool firstTimeout = false;
    try { (void)client.recv(buf, sizeof(buf)); }
    catch (const socket_error& e) { firstTimeout = (e.code() == socket_errc::timeout); }
    EXPECT_TRUE(firstTimeout);
    ASSERT_TRUE(client.is_open());

    // 再放宽超时，等数据
    client.set_recv_timeout(std::chrono::milliseconds(1000));
    size_t n = client.recv(buf, sizeof(buf));
    EXPECT_EQ(n, static_cast<size_t>(5));
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(buf), n), "HELLO");

    client.close();
    listener.sock.close();
    server.join();
}

} // namespace
