//=============================================================================
// test_pemp_server_soe_upload.cxx
//
// 回归 H1 (code review)：do_upload_soe 曾无脑 PopAll → 构 frame → send，
// 若 send 失败（对端断开、超时、异常）events 就永久丢失。修复：send 失败
// 时用 SOEQueue::PushFrontBatch 把 events 回填到队列头，保持时序。
//
// 用 loopback socket 对，服务端调 do_upload_soe，客户端提前 close 触发
// 发送失败；断言 g_soeQueue 里的事件数量恢复。
//=============================================================================

#include "mini_gtest.h"
#include "pmpc_test_fixture.h"
#include "pemp_server.h"
#include "soe_queue.h"
#include "protocol.h"
#include "harness/port_alloc.h"
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

std::atomic<bool> g_running{true};

using pmpc::testing::GlobalStateFixture;
using pmpc::testing::harness::MakeListener;

class PempServerTestAccess {
public:
    explicit PempServerTestAccess(PempServer& s) : s_(s) {}
    bool UploadSoe(socket& client) { return s_.do_upload_soe(client); }
private:
    PempServer& s_;
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

void PushFakeSoe(uint16_t soeId, uint8_t status) {
    g_soeQueue.PushSimulated(1, 1, soeId, status);
}

class PempSoeUploadTest : public GlobalStateFixture {
protected:
    wsa_guard wsa_;
};

TEST_F(PempSoeUploadTest, SuccessDrainsQueue) {
    PushFakeSoe(1, 0x02);
    PushFakeSoe(2, 0x01);
    PushFakeSoe(3, 0x02);
    ASSERT_EQ(g_soeQueue.PendingCount(), static_cast<size_t>(3));

    PempServer srv(4096, 5000, 5000);
    PempServerTestAccess acc(srv);

    auto pair = MakeSocketPair();
    // 客户端保持开放，服务端上传应成功
    EXPECT_TRUE(acc.UploadSoe(pair.serverSide));
    EXPECT_EQ(g_soeQueue.PendingCount(), static_cast<size_t>(0));

    pair.serverSide.close();
    pair.clientSide.close();
}

// H1 关键回归：send 抛异常时 events 必须回填。
// 用 close(serverSide) 让 send 直接抛 socket_errc::not_open。
TEST_F(PempSoeUploadTest, SendFailureRequeuesEntries) {
    PushFakeSoe(1, 0x02);
    PushFakeSoe(2, 0x01);
    PushFakeSoe(3, 0x02);
    ASSERT_EQ(g_soeQueue.PendingCount(), static_cast<size_t>(3));

    PempServer srv(4096, 5000, 5000);
    PempServerTestAccess acc(srv);

    auto pair = MakeSocketPair();
    // 直接关闭 server side → send 会抛 socket_error(not_open)
    pair.serverSide.close();

    EXPECT_FALSE(acc.UploadSoe(pair.serverSide));

    // 事件全部回填，一条都不能少
    EXPECT_EQ(g_soeQueue.PendingCount(), static_cast<size_t>(3));

    pair.clientSide.close();
}

TEST_F(PempSoeUploadTest, RequeuedEventsPreserveOrder) {
    PushFakeSoe(10, 0x02);
    PushFakeSoe(20, 0x01);
    PushFakeSoe(30, 0x02);

    PempServer srv(4096, 5000, 5000);
    PempServerTestAccess acc(srv);

    auto pair = MakeSocketPair();
    pair.serverSide.close();

    EXPECT_FALSE(acc.UploadSoe(pair.serverSide));

    // 再取一次，验证顺序：10 → 20 → 30
    auto events = g_soeQueue.PopAll();
    ASSERT_EQ(events.size(), static_cast<size_t>(3));
    EXPECT_EQ(events[0].soeId, static_cast<uint16_t>(10));
    EXPECT_EQ(events[1].soeId, static_cast<uint16_t>(20));
    EXPECT_EQ(events[2].soeId, static_cast<uint16_t>(30));

    pair.clientSide.close();
}

TEST_F(PempSoeUploadTest, RequeuedEventsFrontOfNewOnes) {
    PushFakeSoe(1, 0x02);
    PushFakeSoe(2, 0x01);

    PempServer srv(4096, 5000, 5000);
    PempServerTestAccess acc(srv);

    auto pair = MakeSocketPair();
    pair.serverSide.close();

    EXPECT_FALSE(acc.UploadSoe(pair.serverSide));
    // 回填后再入一条新的
    PushFakeSoe(99, 0x02);

    auto events = g_soeQueue.PopAll();
    ASSERT_EQ(events.size(), static_cast<size_t>(3));
    EXPECT_EQ(events[0].soeId, static_cast<uint16_t>(1));   // 老的在前
    EXPECT_EQ(events[1].soeId, static_cast<uint16_t>(2));
    EXPECT_EQ(events[2].soeId, static_cast<uint16_t>(99));  // 新的在后

    pair.clientSide.close();
}

TEST_F(PempSoeUploadTest, EmptyQueueReturnsFalseNoOp) {
    PempServer srv(4096, 5000, 5000);
    PempServerTestAccess acc(srv);

    auto pair = MakeSocketPair();
    EXPECT_FALSE(acc.UploadSoe(pair.serverSide));   // 空队列
    EXPECT_EQ(g_soeQueue.PendingCount(), static_cast<size_t>(0));

    pair.serverSide.close();
    pair.clientSide.close();
}

} // namespace
