//=============================================================================
// test_pemp_server_auth.cxx
//
// 回归 M3：PempServer::handle_exec_remote_ctrl 曾直接 (void)pwdLen 忽略
// 帧内密码字段，任何客户端都能执行遥控。修复：新增 rcPassword_ 成员和
// setter，非空时逐字节比对帧内密码。
//
// 本测试直接驱动 handle_exec_remote_ctrl（经 PempServerTestAccess friend
// 桥），用 socket loopback pair 收集应答，检查返回 CtrlResult。
//=============================================================================

#include "mini_gtest.h"
#include "pmpc_test_fixture.h"
#include "pemp_server.h"
#include "pmpc.h"
#include "protocol.h"
#include "harness/port_alloc.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

// g_running 通常在 main.cxx 里；测试进程无 main.cxx，补占位。
std::atomic<bool> g_running{true};

using pmpc::testing::GlobalStateFixture;
using pmpc::testing::harness::MakeListener;

// friend 桥：暴露 handle_exec_remote_ctrl
class PempServerTestAccess {
public:
    explicit PempServerTestAccess(PempServer& s) : s_(s) {}
    void ExecRemoteCtrl(const std::vector<uint8_t>& frame, socket& client) {
        s_.handle_exec_remote_ctrl(frame, client);
    }
private:
    PempServer& s_;
};

namespace {

// 建立一对已 connect/accept 的 loopback socket，返回 (server 端, client 端)
struct SocketPair {
    socket serverSide;
    socket clientSide;
};

SocketPair MakeSocketPair() {
    auto listener = MakeListener();
    socket clientSide;
    std::thread th([&]() { clientSide.connect("127.0.0.1", listener.port); });
    socket_addr peer;
    socket serverSide = listener.sock.accept(&peer);
    th.join();
    return { std::move(serverSide), std::move(clientSide) };
}

// 用 FrameBuilder 构造 06H 帧：ch, dev, pt, pwdLen, [pwd bytes...]
std::vector<uint8_t> BuildExecCtrlFrame(uint8_t ch, uint8_t dev, uint16_t pt,
                                        const std::string& pwd) {
    FrameBuilder fb;
    fb.Begin(FunCode::ExecRemoteCtrl);
    fb.Append(ch);
    fb.Append(dev);
    fb.AppendU16(pt);
    fb.Append(static_cast<uint8_t>(pwd.size()));
    for (char c : pwd) fb.Append(static_cast<uint8_t>(c));
    return fb.End();
}

// 从 socket 读取一个完整的 pemp 帧（简易实现：读到 FRAME_END 0x7D 为止）
std::vector<uint8_t> ReadFrame(socket& s, int timeoutMs = 500) {
    s.set_recv_timeout(std::chrono::milliseconds(timeoutMs));
    std::vector<uint8_t> buf;
    uint8_t byte;
    // 至少读完 START+FUN+LEN(2) = 4 字节，然后按 LEN 读 payload+END
    while (buf.size() < 4) {
        size_t n = s.recv(&byte, 1);
        if (n == 0) return {};
        buf.push_back(byte);
    }
    // FRAME_START=0x7B, FRAME_END=0x7D — payload len 在 buf[2..3] little-endian
    uint16_t payloadLen = static_cast<uint16_t>(
        buf[2] | (static_cast<unsigned>(buf[3]) << 8));
    size_t need = 4u + payloadLen + 1u;
    while (buf.size() < need) {
        size_t n = s.recv(&byte, 1);
        if (n == 0) return {};
        buf.push_back(byte);
    }
    return buf;
}

class PempAuthTest : public GlobalStateFixture {
protected:
    void SetUp() override {
        GlobalStateFixture::SetUp();
        // 需要一个存在 DI/DO 的点表，SetDoMaster 才能成功
        cfgPath_ = "test_pemp_auth_cfg.ini";
        std::ofstream f(cfgPath_);
        f << "[Channel_1]\n" << "Dev_1=0,0,1,0\n";   // 1 个 DO
        f.close();
        RemoteDataMgr::Instance().LoadConfig(cfgPath_);
        // 通讯状态位 pt=1 默认 false（离线）; 遥控前设成 true 表示在线
        RemoteDataMgr::Instance().SetDi(1, 1, 1, true, 1000, true);
    }
    void TearDown() override {
        std::remove(cfgPath_.c_str());
        GlobalStateFixture::TearDown();
    }

    // 从 06H 应答帧提取 CtrlResult (payload 里 offset=4 是结果字节)
    static uint8_t ExtractCtrlResult(const std::vector<uint8_t>& resp) {
        // payload 从 buf[4] 开始：ch(1)+dev(1)+pt(2)+result(1)
        return FrameParser::ReadByte(resp, 4);
    }

    std::string cfgPath_;
    wsa_guard   wsa_;
};

TEST_F(PempAuthTest, NoPasswordSetAllowsControl) {
    // 修复前的老行为：密码未配置 → 通过（保留 default 兼容）
    PempServer srv(4096, 5000, 5000);
    // 不调用 setRemoteCtrlPassword，或明确 setRemoteCtrlPassword("")
    ASSERT_TRUE(srv.remoteCtrlPassword().empty());

    PempServerTestAccess acc(srv);
    auto pair = MakeSocketPair();
    auto frame = BuildExecCtrlFrame(1, 1, 1, "");

    acc.ExecRemoteCtrl(frame, pair.serverSide);
    auto resp = ReadFrame(pair.clientSide);
    ASSERT_GE(resp.size(), static_cast<size_t>(9));
    EXPECT_EQ(ExtractCtrlResult(resp), static_cast<uint8_t>(CtrlResult::Success));

    pair.serverSide.close();
    pair.clientSide.close();
}

TEST_F(PempAuthTest, CorrectPasswordAllowsControl) {
    PempServer srv(4096, 5000, 5000);
    srv.setRemoteCtrlPassword("secret");

    PempServerTestAccess acc(srv);
    auto pair = MakeSocketPair();
    auto frame = BuildExecCtrlFrame(1, 1, 1, "secret");

    acc.ExecRemoteCtrl(frame, pair.serverSide);
    auto resp = ReadFrame(pair.clientSide);
    ASSERT_GE(resp.size(), static_cast<size_t>(9));
    EXPECT_EQ(ExtractCtrlResult(resp), static_cast<uint8_t>(CtrlResult::Success));

    pair.serverSide.close();
    pair.clientSide.close();
}

TEST_F(PempAuthTest, WrongPasswordRejectsControl) {
    // M3 关键回归：错误密码必须被拒绝
    PempServer srv(4096, 5000, 5000);
    srv.setRemoteCtrlPassword("secret");

    PempServerTestAccess acc(srv);
    auto pair = MakeSocketPair();
    auto frame = BuildExecCtrlFrame(1, 1, 1, "wrong!");

    acc.ExecRemoteCtrl(frame, pair.serverSide);
    auto resp = ReadFrame(pair.clientSide);
    ASSERT_GE(resp.size(), static_cast<size_t>(9));
    EXPECT_EQ(ExtractCtrlResult(resp), static_cast<uint8_t>(CtrlResult::Failed));

    // 且 DO 未被写：masterVal 应仍为 false
    DoPoint doPt;
    ASSERT_TRUE(RemoteDataMgr::Instance().GetDo(1, 1, 1, doPt));
    EXPECT_FALSE(doPt.masterVal);

    pair.serverSide.close();
    pair.clientSide.close();
}

TEST_F(PempAuthTest, EmptyPasswordWhenServerRequiresRejects) {
    // 服务端要求密码，客户端发送空密码 → 拒绝
    PempServer srv(4096, 5000, 5000);
    srv.setRemoteCtrlPassword("secret");

    PempServerTestAccess acc(srv);
    auto pair = MakeSocketPair();
    auto frame = BuildExecCtrlFrame(1, 1, 1, "");

    acc.ExecRemoteCtrl(frame, pair.serverSide);
    auto resp = ReadFrame(pair.clientSide);
    ASSERT_GE(resp.size(), static_cast<size_t>(9));
    EXPECT_EQ(ExtractCtrlResult(resp), static_cast<uint8_t>(CtrlResult::Failed));

    pair.serverSide.close();
    pair.clientSide.close();
}

TEST_F(PempAuthTest, ShortPasswordRejects) {
    // 帧内 pwdLen 短于服务端配置 → 拒绝（避免"前缀匹配"漏洞）
    PempServer srv(4096, 5000, 5000);
    srv.setRemoteCtrlPassword("secret");

    PempServerTestAccess acc(srv);
    auto pair = MakeSocketPair();
    auto frame = BuildExecCtrlFrame(1, 1, 1, "sec");   // 3 字节，非 6

    acc.ExecRemoteCtrl(frame, pair.serverSide);
    auto resp = ReadFrame(pair.clientSide);
    ASSERT_GE(resp.size(), static_cast<size_t>(9));
    EXPECT_EQ(ExtractCtrlResult(resp), static_cast<uint8_t>(CtrlResult::Failed));

    pair.serverSide.close();
    pair.clientSide.close();
}

} // namespace
