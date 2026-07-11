//=============================================================================
// test_pemp_server_ai_precision.cxx
//
// 回归 H3 (code review)：PEMP 遥测帧的 AI 是 int32；RemoteDataMgr 存 double。
// 早期代码直接 static_cast<int32_t>(v) 会：
//   1) 静默丢 fractional part（3.14 → 3）
//   2) 对 |v| > 2^31 - 1 undefined behavior（在 g++/MSVC 上表现为回绕）
// 修复：新增 QuantizeAiToInt32Warn helper 做 clamp + 首次警告，帧仍按规约
// 用 int32 编码但至少不再是 UB，运维能在日志里看到需要 scale/offset 的点。
//=============================================================================

#include "mini_gtest.h"
#include "pmpc_test_fixture.h"
#include "pemp_server.h"
#include "pmpc.h"
#include "protocol.h"
#include "harness/port_alloc.h"
#include <atomic>
#include <chrono>
#include <climits>
#include <cstdio>
#include <fstream>
#include <thread>
#include <vector>

std::atomic<bool> g_running{true};

using pmpc::testing::GlobalStateFixture;
using pmpc::testing::harness::MakeListener;

class PempServerTestAccess {
public:
    explicit PempServerTestAccess(PempServer& s) : s_(s) {}
    bool UploadAi(socket& client) { return s_.do_upload_ai(client); }
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

std::vector<uint8_t> ReadFrame(socket& s, int timeoutMs = 500) {
    s.set_recv_timeout(std::chrono::milliseconds(timeoutMs));
    std::vector<uint8_t> buf;
    uint8_t byte;
    try {
        while (buf.size() < 4) {
            size_t n = s.recv(&byte, 1); if (n == 0) return {};
            buf.push_back(byte);
        }
    } catch (const socket_error&) { return {}; }
    uint16_t payloadLen = static_cast<uint16_t>(
        buf[2] | (static_cast<unsigned>(buf[3]) << 8));
    size_t need = 4u + payloadLen + 1u;
    try {
        while (buf.size() < need) {
            size_t n = s.recv(&byte, 1); if (n == 0) return {};
            buf.push_back(byte);
        }
    } catch (const socket_error&) { return {}; }
    return buf;
}

// UploadTelemetry (53H) payload：CH(1) DEV(1) TotalPoints(2) values[TotalPoints × 4 bytes int32 LE]
// 从帧中取出第 i 个 AI 值
int32_t ExtractAiValue(const std::vector<uint8_t>& frame, size_t i) {
    // FrameParser::ReadByte(frame, N) 加 FRAME_HEADER=4 偏移
    size_t off = 4;  // 相对偏移：CH(1)+DEV(1)+TotalPoints(2)
    return FrameParser::ReadI32(frame, off + i * 4);
}

class PempAiPrecisionTest : public GlobalStateFixture {
protected:
    void SetUp() override {
        GlobalStateFixture::SetUp();
        cfgPath_ = "test_pemp_ai_precision_cfg.ini";
        // 一个通道一个设备 4 个 AI 点
        std::ofstream f(cfgPath_);
        f << "[Channel_1]\n" << "Dev_1=0,4,0,0\n";
        f.close();
        RemoteDataMgr::Instance().LoadConfig(cfgPath_);
    }
    void TearDown() override {
        std::remove(cfgPath_.c_str());
        GlobalStateFixture::TearDown();
    }

    std::string cfgPath_;
    wsa_guard   wsa_;
};

// 合法整数 double 应无损转 int32
TEST_F(PempAiPrecisionTest, WholeNumbersEncodedExactly) {
    RemoteDataMgr::Instance().SetAi(1, 1, 1,    42.0);
    RemoteDataMgr::Instance().SetAi(1, 1, 2,  -100.0);
    RemoteDataMgr::Instance().SetAi(1, 1, 3,     0.0);
    RemoteDataMgr::Instance().SetAi(1, 1, 4, 65535.0);

    PempServer srv(4096, 5000, 5000);
    PempServerTestAccess acc(srv);
    auto pair = MakeSocketPair();
    ASSERT_TRUE(acc.UploadAi(pair.serverSide));

    auto frame = ReadFrame(pair.clientSide);
    ASSERT_GE(frame.size(), static_cast<size_t>(4 + 4 + 4 * 4 + 1));
    EXPECT_EQ(ExtractAiValue(frame, 0), 42);
    EXPECT_EQ(ExtractAiValue(frame, 1), -100);
    EXPECT_EQ(ExtractAiValue(frame, 2), 0);
    EXPECT_EQ(ExtractAiValue(frame, 3), 65535);

    pair.serverSide.close();
    pair.clientSide.close();
}

// H3 关键回归：超过 int32 上限 → clamp 到 INT32_MAX，不 UB
TEST_F(PempAiPrecisionTest, ValueAboveInt32MaxClampsToMax) {
    RemoteDataMgr::Instance().SetAi(1, 1, 1, 3e9);   // > 2^31 - 1
    RemoteDataMgr::Instance().SetAi(1, 1, 2, 100.0); // normal

    PempServer srv(4096, 5000, 5000);
    PempServerTestAccess acc(srv);
    auto pair = MakeSocketPair();
    ASSERT_TRUE(acc.UploadAi(pair.serverSide));

    auto frame = ReadFrame(pair.clientSide);
    ASSERT_GE(frame.size(), static_cast<size_t>(4 + 4 + 4 * 4 + 1));
    EXPECT_EQ(ExtractAiValue(frame, 0), INT32_MAX);   // 修复前是 UB / 回绕
    EXPECT_EQ(ExtractAiValue(frame, 1), 100);

    pair.serverSide.close();
    pair.clientSide.close();
}

// 下限侧同样 clamp 到 INT32_MIN
TEST_F(PempAiPrecisionTest, ValueBelowInt32MinClampsToMin) {
    RemoteDataMgr::Instance().SetAi(1, 1, 1, -3e9);
    RemoteDataMgr::Instance().SetAi(1, 1, 2, 0.0);

    PempServer srv(4096, 5000, 5000);
    PempServerTestAccess acc(srv);
    auto pair = MakeSocketPair();
    ASSERT_TRUE(acc.UploadAi(pair.serverSide));

    auto frame = ReadFrame(pair.clientSide);
    ASSERT_GE(frame.size(), static_cast<size_t>(4 + 4 + 4 * 4 + 1));
    EXPECT_EQ(ExtractAiValue(frame, 0), INT32_MIN);

    pair.serverSide.close();
    pair.clientSide.close();
}

// fractional part 被截断（这是规约限制，行为不变，测试锁定当前语义）
TEST_F(PempAiPrecisionTest, FractionalPartTruncated) {
    RemoteDataMgr::Instance().SetAi(1, 1, 1, 3.14);
    RemoteDataMgr::Instance().SetAi(1, 1, 2, -2.7);

    PempServer srv(4096, 5000, 5000);
    PempServerTestAccess acc(srv);
    auto pair = MakeSocketPair();
    ASSERT_TRUE(acc.UploadAi(pair.serverSide));

    auto frame = ReadFrame(pair.clientSide);
    ASSERT_GE(frame.size(), static_cast<size_t>(4 + 4 + 4 * 4 + 1));
    EXPECT_EQ(ExtractAiValue(frame, 0), 3);
    EXPECT_EQ(ExtractAiValue(frame, 1), -2);

    pair.serverSide.close();
    pair.clientSide.close();
}

// 边界：正好 INT32_MAX 应通过（不 clamp）
TEST_F(PempAiPrecisionTest, ExactInt32MaxPassesThrough) {
    RemoteDataMgr::Instance().SetAi(1, 1, 1, static_cast<double>(INT32_MAX));

    PempServer srv(4096, 5000, 5000);
    PempServerTestAccess acc(srv);
    auto pair = MakeSocketPair();
    ASSERT_TRUE(acc.UploadAi(pair.serverSide));

    auto frame = ReadFrame(pair.clientSide);
    ASSERT_GE(frame.size(), static_cast<size_t>(4 + 4 + 4 + 1));
    EXPECT_EQ(ExtractAiValue(frame, 0), INT32_MAX);

    pair.serverSide.close();
    pair.clientSide.close();
}

} // namespace
