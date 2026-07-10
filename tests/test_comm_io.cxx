//=============================================================================
// test_comm_io.cxx — CommIO 统一 IO 封装测试
// 策略:
//   1. IsTcpAddr 纯函数测试 (无 I/O)
//   2. TCP 模式: 每个测试独立创建 listen_sock + 线程 (避免 fixture 竞争)
//   3. 串口模式: 仅类型检测 (无硬件)
//=============================================================================
#include "mini_gtest.h"
#include "comm_io.h"
#include <thread>

// ==================== IsTcpAddr ====================

TEST(CommIO_IsTcpAddrTest, IpPortReturnsTrue) {
    EXPECT_TRUE(IsTcpAddr("192.168.1.1:502"));
    EXPECT_TRUE(IsTcpAddr("127.0.0.1:4096"));
}

TEST(CommIO_IsTcpAddrTest, HostnamePortReturnsTrue) {
    EXPECT_TRUE(IsTcpAddr("localhost:2404"));
    EXPECT_TRUE(IsTcpAddr("my-server:7503"));
}

TEST(CommIO_IsTcpAddrTest, ComPortReturnsFalse) {
    EXPECT_FALSE(IsTcpAddr("COM1"));
    EXPECT_FALSE(IsTcpAddr("COM2"));
    EXPECT_FALSE(IsTcpAddr("COM256"));
}

TEST(CommIO_IsTcpAddrTest, DevicePathReturnsFalse) {
    EXPECT_FALSE(IsTcpAddr("\\\\.\\COM1"));
    EXPECT_FALSE(IsTcpAddr("/dev/ttyS0"));
    EXPECT_FALSE(IsTcpAddr("/dev/ttyUSB0"));
}

TEST(CommIO_IsTcpAddrTest, EmptyStringReturnsFalse) {
    EXPECT_FALSE(IsTcpAddr(""));
}

// ==================== CommIO TCP 模式 ====================
// 每个测试独立创建 listen_sock + 线程; 保障线程不会在 TearDown 中卡死

TEST(CommIOTcpTest, OpenAndClose) {
    wsa_guard wsa;
    socket ls;
    ls.bind(socket_addr("127.0.0.1", 0));
    ls.listen();
    std::string addr = ls.local_addr().host() + ":" + std::to_string(ls.local_addr().port());

    std::thread svr([&]() { wsa_guard w2; socket peer = ls.accept(); peer.close(); });

    CommIO io;
    bool ok = io.open(addr, 9600, "none", 8, 1, 1000);
    EXPECT_TRUE(ok);
    EXPECT_EQ(io.type, CommIO::TCP);
    EXPECT_TRUE(io.isOpen());
    io.close();
    EXPECT_FALSE(io.isOpen());

    ls.close();
    svr.join();
}

TEST(CommIOTcpTest, SendAndRecv) {
    wsa_guard wsa;
    socket ls;
    ls.bind(socket_addr("127.0.0.1", 0));
    ls.listen();
    std::string addr = ls.local_addr().host() + ":" + std::to_string(ls.local_addr().port());

    std::thread svr([&]() {
        wsa_guard w2;
        socket peer = ls.accept();
        uint8_t buf[32];
        auto n = peer.recv(buf, sizeof(buf));
        if (n > 0) peer.send(buf, n);
        peer.close();
    });

    CommIO io;
    ASSERT_TRUE(io.open(addr, 9600, "none", 8, 1, 5000));
    uint8_t msg[] = "HELLO";
    io.write(msg, 5);
    uint8_t buf[32]{};
    size_t n = io.read(buf, sizeof(buf));
    EXPECT_EQ(n, 5u);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(buf), n), "HELLO");
    io.close();

    ls.close();
    svr.join();
}

TEST(CommIOTcpTest, IsOpenAfterClose) {
    wsa_guard wsa;
    socket ls;
    ls.bind(socket_addr("127.0.0.1", 0));
    ls.listen();
    std::string addr = ls.local_addr().host() + ":" + std::to_string(ls.local_addr().port());

    std::thread svr([&]() { wsa_guard w2; socket peer = ls.accept(); peer.close(); });

    CommIO io;
    ASSERT_TRUE(io.open(addr, 9600, "none", 8, 1, 5000));
    EXPECT_TRUE(io.isOpen());
    io.close();
    EXPECT_FALSE(io.isOpen());

    ls.close();
    svr.join();
}

TEST(CommIOTcpTest, SetReadTimeout) {
    wsa_guard wsa;
    socket ls;
    ls.bind(socket_addr("127.0.0.1", 0));
    ls.listen();
    std::string addr = ls.local_addr().host() + ":" + std::to_string(ls.local_addr().port());

    std::thread svr([&]() { wsa_guard w2; socket peer = ls.accept(); peer.close(); });

    CommIO io;
    ASSERT_TRUE(io.open(addr, 9600, "none", 8, 1, 5000));
    EXPECT_NO_THROW(io.setReadTimeout(100));
    io.close();

    ls.close();
    svr.join();
}

TEST(CommIOTcpTest, DefaultNotOpen) {
    CommIO io;
    EXPECT_FALSE(io.isOpen());
}

// ==================== CommIO 串口模式 ====================
// 不使用 open() 以防止依赖物理串口; 直接构造并检查类型

TEST(CommIOSerialTest, SerialTypeDetection) {
    CommIO io;
    io.type = CommIO::SERIAL;
    EXPECT_FALSE(io.isOpen());
    EXPECT_NO_THROW(io.close());
}

TEST(CommIOSerialTest, SerialSetBaudWithoutOpen) {
    CommIO io;
    io.type = CommIO::SERIAL;
    EXPECT_FALSE(io.setBaud(9600));
}

TEST(CommIOSerialTest, SerialCurrentBaudReturnsZero) {
    CommIO io;
    io.type = CommIO::SERIAL;
    EXPECT_EQ(io.currentBaud(), 0);
}

TEST(CommIOSerialTest, TcpCurrentBaudReturnsZero) {
    CommIO io;
    io.type = CommIO::TCP;
    EXPECT_EQ(io.currentBaud(), 0);
}

TEST(CommIOSerialTest, TcpSetBaudReturnsFalse) {
    CommIO io;
    io.type = CommIO::TCP;
    EXPECT_FALSE(io.setBaud(9600));
}
