//=============================================================================
// test_pemp_server_id_range.cxx
//
// 回归 H2 (code review)：PEMP 帧的 CH/DEV 字段只有 1 字节。RemoteDataMgr
// 允许 chId/devNo 到 65535，直接把它们 & 0xFF 截断会让不同通道打包成同一
// 字节，客户端无法区分。修复：do_upload_di / do_upload_ai 在 chId>255 或
// devNo>255 时 log 警告并跳过。
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

std::atomic<bool> g_running{true};

using pmpc::testing::GlobalStateFixture;
using pmpc::testing::harness::MakeListener;

// friend 桥：暴露 do_upload_di / do_upload_ai
class PempServerTestAccess {
public:
    explicit PempServerTestAccess(PempServer& s) : s_(s) {}
    bool UploadDi(socket& client) { return s_.do_upload_di(client); }
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

// 尝试读一个帧；返回 payload 里的第一字节（CH byte），空 vector 表示未收到
std::vector<uint8_t> TryReadOneFrame(socket& s, int timeoutMs = 200) {
    s.set_recv_timeout(std::chrono::milliseconds(timeoutMs));
    std::vector<uint8_t> buf;
    uint8_t byte;
    try {
        while (buf.size() < 4) {
            size_t n = s.recv(&byte, 1);
            if (n == 0) return {};
            buf.push_back(byte);
        }
    } catch (const socket_error&) { return {}; }
    uint16_t payloadLen = static_cast<uint16_t>(
        buf[2] | (static_cast<unsigned>(buf[3]) << 8));
    size_t need = 4u + payloadLen + 1u;
    try {
        while (buf.size() < need) {
            size_t n = s.recv(&byte, 1);
            if (n == 0) return {};
            buf.push_back(byte);
        }
    } catch (const socket_error&) { return {}; }
    return buf;
}

class PempIdRangeTest : public GlobalStateFixture {
protected:
    void SetUp() override {
        GlobalStateFixture::SetUp();
        cfgPath_ = "test_pemp_id_range_cfg.ini";
    }
    void TearDown() override {
        std::remove(cfgPath_.c_str());
        GlobalStateFixture::TearDown();
    }

    // 生成一个只含单个 [Channel_N] Dev_M 的配置
    void WriteConfig(uint16_t chId, uint16_t devNo,
                     int diCnt = 3, int aiCnt = 2, int doCnt = 0, int aoCnt = 0) {
        std::ofstream f(cfgPath_);
        f << "[Channel_" << chId << "]\n"
          << "Dev_" << devNo << "=" << diCnt << "," << aiCnt << ","
          << doCnt << "," << aoCnt << "\n";
        f.close();
        RemoteDataMgr::Instance().LoadConfig(cfgPath_);
    }

    std::string cfgPath_;
    wsa_guard   wsa_;
};

// 合法 chId/devNo (<=255) → 帧被下发
TEST_F(PempIdRangeTest, LegalIdsUploadFrame) {
    WriteConfig(1, 1);
    PempServer srv(4096, 5000, 5000);
    PempServerTestAccess acc(srv);

    auto pair = MakeSocketPair();
    EXPECT_TRUE(acc.UploadDi(pair.serverSide));

    auto frame = TryReadOneFrame(pair.clientSide);
    ASSERT_GE(frame.size(), static_cast<size_t>(5));
    // FrameParser::ReadByte(frame, N) 已加 FRAME_HEADER=4 偏移，
    // 所以 ReadByte(0) = payload[0] = CH，ReadByte(1) = payload[1] = DEV
    EXPECT_EQ(FrameParser::ReadByte(frame, 0), static_cast<uint8_t>(1));   // CH
    EXPECT_EQ(FrameParser::ReadByte(frame, 1), static_cast<uint8_t>(1));   // DEV

    pair.serverSide.close();
    pair.clientSide.close();
}

// H2 关键回归：chId=256 → 应被跳过，帧不下发（否则会静默变成 CH=0）
TEST_F(PempIdRangeTest, ChannelIdAbove255IsSkippedNotTruncated) {
    WriteConfig(/*chId=*/256, /*devNo=*/1);
    PempServer srv(4096, 5000, 5000);
    PempServerTestAccess acc(srv);

    auto pair = MakeSocketPair();
    EXPECT_TRUE(acc.UploadDi(pair.serverSide));

    auto frame = TryReadOneFrame(pair.clientSide);
    EXPECT_TRUE(frame.empty());   // 没有帧下发

    pair.serverSide.close();
    pair.clientSide.close();
}

// devNo=300 也应被跳过
TEST_F(PempIdRangeTest, DeviceIdAbove255IsSkipped) {
    WriteConfig(/*chId=*/1, /*devNo=*/300);
    PempServer srv(4096, 5000, 5000);
    PempServerTestAccess acc(srv);

    auto pair = MakeSocketPair();
    EXPECT_TRUE(acc.UploadDi(pair.serverSide));

    auto frame = TryReadOneFrame(pair.clientSide);
    EXPECT_TRUE(frame.empty());

    pair.serverSide.close();
    pair.clientSide.close();
}

// AI 上传同样有此保护
TEST_F(PempIdRangeTest, AiUploadAlsoSkipsOversizeIds) {
    WriteConfig(/*chId=*/256, /*devNo=*/1);
    PempServer srv(4096, 5000, 5000);
    PempServerTestAccess acc(srv);

    auto pair = MakeSocketPair();
    EXPECT_TRUE(acc.UploadAi(pair.serverSide));

    auto frame = TryReadOneFrame(pair.clientSide);
    EXPECT_TRUE(frame.empty());

    pair.serverSide.close();
    pair.clientSide.close();
}

// 边界：chId=255 恰好允许
TEST_F(PempIdRangeTest, Id255IsLegalBoundary) {
    WriteConfig(/*chId=*/255, /*devNo=*/255);
    PempServer srv(4096, 5000, 5000);
    PempServerTestAccess acc(srv);

    auto pair = MakeSocketPair();
    EXPECT_TRUE(acc.UploadDi(pair.serverSide));

    auto frame = TryReadOneFrame(pair.clientSide);
    ASSERT_GE(frame.size(), static_cast<size_t>(6));
    EXPECT_EQ(FrameParser::ReadByte(frame, 0), static_cast<uint8_t>(255));
    EXPECT_EQ(FrameParser::ReadByte(frame, 1), static_cast<uint8_t>(255));

    pair.serverSide.close();
    pair.clientSide.close();
}

} // namespace
