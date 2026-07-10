//=============================================================================
// test_socket.cxx — Socket RAII 封装测试
// 策略: TCP loopback (127.0.0.1:0), 显式内联线程, 避免辅助结构体竞争
// 覆盖: wsa_guard, socket_addr, connect/accept, send/recv,
//       move 语义, shutdown, error_code
//
// 线程模式: 每个测试创建自己的监听 socket + 工作线程,
//           listen_sock 在主线程中存活直到线程 join 完成
//=============================================================================
#include "mini_gtest.h"
#include "socket.h"
#include <thread>
#include <vector>
#include <cstdint>
#include <chrono>

// ==================== wsa_guard ====================

TEST(WsaGuardTest, DefaultConstruction) {
    EXPECT_NO_THROW(wsa_guard wsa);
}

TEST(WsaGuardTest, DoubleGuard) {
    wsa_guard wsa1;
    EXPECT_NO_THROW(wsa_guard wsa2);
}

// ==================== socket_addr ====================

class SocketAddrWithWsa : public ::testing::Test {
protected:
    wsa_guard wsa_;
};

TEST_F(SocketAddrWithWsa, ResolveLocalhost) {
    socket_addr addr("127.0.0.1", 8080);
    EXPECT_TRUE(addr.valid());
    EXPECT_EQ(addr.host(), "127.0.0.1");
    EXPECT_EQ(addr.port(), 8080);
}

TEST_F(SocketAddrWithWsa, InvalidHostThrows) {
    EXPECT_THROW(socket_addr("nonexistent.invalid", 80), socket_error);
}

TEST_F(SocketAddrWithWsa, Equality) {
    socket_addr a("127.0.0.1", 502);
    socket_addr b("127.0.0.1", 502);
    EXPECT_EQ(a, b);
}

TEST_F(SocketAddrWithWsa, Inequality) {
    socket_addr a("127.0.0.1", 502);
    socket_addr b("127.0.0.1", 503);
    EXPECT_NE(a, b);
}

TEST_F(SocketAddrWithWsa, ToString) {
    socket_addr addr("127.0.0.1", 502);
    std::string s = addr.to_string();
    EXPECT_FALSE(s.empty());
}

TEST(SocketAddrTest, DefaultIsInvalid) {
    socket_addr addr;
    EXPECT_FALSE(addr.valid());
}

TEST_F(SocketAddrWithWsa, ResolveAll) {
    auto addrs = socket_addr::resolve_all("127.0.0.1", 80);
    EXPECT_FALSE(addrs.empty());
}

// ==================== TCP connect/accept/send/recv ====================

TEST(SocketTest, ConnectAndRecv) {
    wsa_guard wsa;
    socket listen_sock;
    listen_sock.bind(socket_addr("127.0.0.1", 0));
    listen_sock.listen();
    socket_addr svr_addr = listen_sock.local_addr();

    std::thread svr([&]() {
        wsa_guard wsa2;
        socket peer = listen_sock.accept();
        peer.send(std::string("pong"));
        peer.close();
    });

    socket client;
    client.connect(svr_addr);
    uint8_t buf[64]{};
    auto n = client.recv(buf, sizeof(buf));
    EXPECT_EQ(n, 4u);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(buf), n), "pong");
    client.close();
    svr.join();
}

TEST(SocketTest, EchoRoundtrip) {
    wsa_guard wsa;
    socket listen_sock;
    listen_sock.bind(socket_addr("127.0.0.1", 0));
    listen_sock.listen();
    socket_addr svr_addr = listen_sock.local_addr();

    std::thread svr([&]() {
        wsa_guard wsa2;
        socket peer = listen_sock.accept();
        uint8_t buf[1024];
        auto nr = peer.recv(buf, sizeof(buf));
        if (nr > 0) peer.send(buf, nr);
        peer.close();
    });

    socket client;
    client.connect(svr_addr);
    client.send(std::string("Hello!"));
    uint8_t buf[1024]{};
    auto n = client.recv(buf, sizeof(buf));
    EXPECT_EQ(n, 6u);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(buf), n), "Hello!");
    client.close();
    svr.join();
}

TEST(SocketTest, SendStringOverload) {
    wsa_guard wsa;
    socket listen_sock;
    listen_sock.bind(socket_addr("127.0.0.1", 0));
    listen_sock.listen();
    socket_addr svr_addr = listen_sock.local_addr();

    std::thread svr([&]() {
        wsa_guard wsa2;
        socket peer = listen_sock.accept();
        uint8_t buf[16];
        auto nr = peer.recv(buf, sizeof(buf));
        peer.send(buf, nr);
        peer.close();
    });

    socket client;
    client.connect(svr_addr);
    auto sent = client.send("ping");
    EXPECT_EQ(sent, 4u);
    uint8_t buf[16]{};
    auto n = client.recv(buf, sizeof(buf));
    EXPECT_EQ(n, 4u);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(buf), n), "ping");
    client.close();
    svr.join();
}

TEST(SocketTest, LargePayload) {
    wsa_guard wsa;
    socket listen_sock;
    listen_sock.bind(socket_addr("127.0.0.1", 0));
    listen_sock.listen();
    socket_addr svr_addr = listen_sock.local_addr();

    std::thread svr([&]() {
        wsa_guard wsa2;
        socket peer = listen_sock.accept();
        uint8_t buf[8192];
        auto n = peer.recv(buf, sizeof(buf));
        peer.send(buf, n);
        peer.close();
    });

    socket client;
    client.connect(svr_addr);

    std::vector<uint8_t> send_data(4096, 0xAB);
    auto sent = client.send(send_data.data(), send_data.size());
    EXPECT_EQ(sent, send_data.size());

    uint8_t recv_buf[4096]{};
    auto nrecv = client.recv(recv_buf, sizeof(recv_buf));
    EXPECT_EQ(nrecv, 4096u);
    for (size_t i = 0; i < nrecv; i++)
        EXPECT_EQ(recv_buf[i], 0xAB);

    client.close();
    svr.join();
}

TEST(SocketTest, ConnectAndClose) {
    wsa_guard wsa;
    socket listen_sock;
    listen_sock.bind(socket_addr("127.0.0.1", 0));
    listen_sock.listen();
    socket_addr svr_addr = listen_sock.local_addr();

    std::thread svr([&]() {
        wsa_guard wsa2;
        socket peer = listen_sock.accept();
        // Client will close immediately, this recv returns 0 (EOF)
        uint8_t buf[8];
        peer.recv(buf, sizeof(buf)); // may return 0 after client closes
        peer.close();
    });

    socket client;
    client.connect(svr_addr);
    EXPECT_TRUE(client.is_open());
    client.close();
    EXPECT_FALSE(client.is_open());
    svr.join();
}

// ==================== Move semantics ====================

TEST(SocketTest, MoveConstruction) {
    wsa_guard wsa;
    socket listen_sock;
    listen_sock.bind(socket_addr("127.0.0.1", 0));
    listen_sock.listen();
    socket_addr svr_addr = listen_sock.local_addr();

    std::thread svr([&]() {
        wsa_guard wsa2;
        socket peer = listen_sock.accept();
        peer.send(std::string("moved"));
        peer.close();
    });

    socket client;
    client.connect(svr_addr);

    socket moved(std::move(client));
    EXPECT_TRUE(moved.is_open());
    EXPECT_FALSE(client.is_open()); // NOLINT: moved-from

    uint8_t buf[16]{};
    auto n = moved.recv(buf, sizeof(buf));
    EXPECT_EQ(n, 5u);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(buf), n), "moved");
    moved.close();
    svr.join();
}

TEST(SocketTest, MoveAssignment) {
    wsa_guard wsa;
    socket listen_sock;
    listen_sock.bind(socket_addr("127.0.0.1", 0));
    listen_sock.listen();
    socket_addr svr_addr = listen_sock.local_addr();

    std::thread svr([&]() {
        wsa_guard wsa2;
        socket peer = listen_sock.accept();
        peer.send(std::string("assign"));
        peer.close();
    });

    socket client;
    client.connect(svr_addr);

    socket dest;
    dest = std::move(client);
    EXPECT_TRUE(dest.is_open());
    EXPECT_FALSE(client.is_open()); // NOLINT: moved-from

    uint8_t buf[16]{};
    auto n = dest.recv(buf, sizeof(buf));
    EXPECT_EQ(n, 6u);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(buf), n), "assign");
    dest.close();
    svr.join();
}

// ==================== Close / shutdown ====================

TEST(SocketTest, DoubleCloseIsSafe) {
    wsa_guard wsa;
    socket s;
    s.close();
    EXPECT_NO_THROW(s.close());
}

TEST(SocketTest, ShutdownRead) {
    wsa_guard wsa;
    socket listen_sock;
    listen_sock.bind(socket_addr("127.0.0.1", 0));
    listen_sock.listen();
    socket_addr svr_addr = listen_sock.local_addr();

    std::thread svr([&]() {
        wsa_guard wsa2;
        socket peer = listen_sock.accept();
        peer.send(std::string("data"));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        peer.close();
    });

    socket client;
    client.connect(svr_addr);
    EXPECT_NO_THROW(client.shutdown(0)); // shutdown read
    client.close();
    svr.join();
}

TEST(SocketTest, ShutdownWrite) {
    wsa_guard wsa;
    socket listen_sock;
    listen_sock.bind(socket_addr("127.0.0.1", 0));
    listen_sock.listen();
    socket_addr svr_addr = listen_sock.local_addr();

    std::thread svr([&]() {
        wsa_guard wsa2;
        socket peer = listen_sock.accept();
        uint8_t buf[8];
        peer.recv(buf, sizeof(buf));
        peer.close();
    });

    socket client;
    client.connect(svr_addr);
    EXPECT_NO_THROW(client.shutdown(1)); // shutdown write
    client.close();
    svr.join();
}

// ==================== Error codes ====================

TEST(SocketErrorTest, ErrorCategoryName) {
    auto& cat = socket_category();
    std::string name = cat.name();
    EXPECT_FALSE(name.empty());
}

TEST(SocketErrorTest, MakeErrorCode) {
    auto ec = make_error_code(socket_errc::timeout);
    EXPECT_TRUE(ec);
    EXPECT_EQ(ec.value(), static_cast<int>(socket_errc::timeout));
}

TEST(SocketErrorTest, SocketErrorMessage) {
    socket_error err(socket_errc::connect_failed, "connection refused");
    std::string msg = err.what();
    EXPECT_NE(msg.find("connection refused"), std::string::npos);
}

// ==================== State ====================

TEST(SocketTest, DefaultConstructedIsNotOpen) {
    socket s;
    EXPECT_FALSE(s.is_open());
}

// ==================== Connect refused ====================

TEST(SocketTest, ConnectRefusedThrows) {
    wsa_guard wsa;
    socket client;
    EXPECT_THROW(client.connect("127.0.0.1", 1), socket_error);
}
