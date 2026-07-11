//=============================================================================
// test_iec104_recvframe.cxx
//
// 回归 M12 (code review)：Iec104Master::RecvFrame 遇到 apduLen 非法
// (< 4 或 > 253) 时以前直接 return false，把后续 apduLen 个字节留在 socket
// 缓冲区里；下次 RecvFrame 又从新字节开始扫 FRAME_START，可能把 payload
// 当帧头 → 长期 desync。
//
// 修复：apduLen 无效时主动 shutdown + close，让上层循环退出重连。
//=============================================================================

#include "mini_gtest.h"
#include "iec104_master.h"
#include "harness/port_alloc.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

std::atomic<bool> g_running{true};

using pmpc::testing::harness::MakeListener;

// friend 桥：暴露 RecvFrame
class Iec104MasterTestAccess {
public:
    explicit Iec104MasterTestAccess(Iec104Master& m) : m_(m) {}
    bool Recv(socket& sock, int timeoutMs, uint8_t* buf, size_t& len) {
        return m_.RecvFrame(sock, timeoutMs, buf, len);
    }
private:
    Iec104Master& m_;
};

namespace {

struct SocketPair { socket serverSide, clientSide; };
SocketPair MakeSocketPair() {
    auto listener = MakeListener();
    socket clientSide;
    std::thread th([&]() { clientSide.connect("127.0.0.1", listener.port); });
    socket_addr peer;
    socket serverSide = listener.sock.accept(&peer);
    th.join();
    return { std::move(serverSide), std::move(clientSide) };
}

class Iec104RecvFrameTest : public ::testing::Test {
protected:
    wsa_guard wsa;
};

// 正常路径：有效 apduLen 应正常收到帧
TEST_F(Iec104RecvFrameTest, ValidFrameIsReceived) {
    Iec104Master m;
    Iec104MasterTestAccess acc(m);

    auto pair = MakeSocketPair();

    // 构造一个最短合法帧：0x68 apduLen=4 + 4 字节 payload
    std::thread producer([&]() {
        uint8_t frame[6] = {0x68, 0x04, 0x01, 0x02, 0x03, 0x04};
        pair.clientSide.send(frame, sizeof(frame));
    });

    uint8_t buf[256];
    size_t len = 0;
    bool ok = acc.Recv(pair.serverSide, /*timeoutMs=*/500, buf, len);
    EXPECT_TRUE(ok);
    EXPECT_EQ(len, static_cast<size_t>(6));
    EXPECT_EQ(buf[0], 0x68);
    EXPECT_EQ(buf[1], 0x04);

    producer.join();
    pair.serverSide.close();
    pair.clientSide.close();
}

// M12 关键回归：apduLen=0（<4）时应关闭 socket，下次 Recv 立即返回 false
TEST_F(Iec104RecvFrameTest, InvalidApduLenClosesSocket) {
    Iec104Master m;
    Iec104MasterTestAccess acc(m);

    auto pair = MakeSocketPair();
    std::thread producer([&]() {
        // 非法 apduLen=0，客户端接着又塞了 10 字节噪声
        uint8_t bad[12] = {0x68, 0x00, 'A', 'B', 'C', 'D',
                           'E', 'F', 'G', 'H', 'I', 'J'};
        pair.clientSide.send(bad, sizeof(bad));
    });

    uint8_t buf[256];
    size_t len = 0;
    bool ok = acc.Recv(pair.serverSide, 500, buf, len);
    EXPECT_FALSE(ok);
    // 修复后 socket 应已关闭；再次 Recv 立即返回 false（不会尝试读噪声）
    EXPECT_FALSE(pair.serverSide.is_open());

    producer.join();
    pair.clientSide.close();
}

// apduLen=254（>253）同理
TEST_F(Iec104RecvFrameTest, OversizeApduLenClosesSocket) {
    Iec104Master m;
    Iec104MasterTestAccess acc(m);

    auto pair = MakeSocketPair();
    std::thread producer([&]() {
        uint8_t bad[2] = {0x68, 0xFF};   // 255 > 253
        pair.clientSide.send(bad, sizeof(bad));
    });

    uint8_t buf[256];
    size_t len = 0;
    bool ok = acc.Recv(pair.serverSide, 500, buf, len);
    EXPECT_FALSE(ok);
    EXPECT_FALSE(pair.serverSide.is_open());

    producer.join();
    pair.clientSide.close();
}

// 首字节不是 FRAME_START：仍返回 false，但不关闭 socket（可能是启动阶段
// 收到了不完整的旧数据）
TEST_F(Iec104RecvFrameTest, BadStartByteReturnsFalseKeepsSocketOpen) {
    Iec104Master m;
    Iec104MasterTestAccess acc(m);

    auto pair = MakeSocketPair();
    std::thread producer([&]() {
        uint8_t junk[1] = {0xAA};
        pair.clientSide.send(junk, 1);
    });

    uint8_t buf[256];
    size_t len = 0;
    bool ok = acc.Recv(pair.serverSide, 500, buf, len);
    EXPECT_FALSE(ok);
    // 首字节错并不隐含 apduLen 帧同步破坏；socket 仍开，上层可决定重试或关闭
    EXPECT_TRUE(pair.serverSide.is_open());

    producer.join();
    pair.serverSide.close();
    pair.clientSide.close();
}

} // namespace
