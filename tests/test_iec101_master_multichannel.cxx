//=============================================================================
// test_iec101_master_multichannel.cxx
//
// 回归 M11 (code review)：iec101_master 的 HandleGIResponse 曾遍历所有
// channels_ 并硬编码 mgr.SetDi(1, dev.coa, ...)，导致：
//   (a) 匹配任意通道下 coa 相同的设备（跨通道错配）
//   (b) 多通道场景下所有 DI 都记到 chId=1，互相覆盖
// 修复：HandleGIResponse 接收 chIdx 参数，只处理该通道，chId=(chIdx+1)。
//
// 注：不 include pmpc_test_fixture.h，因为它拖 protocol.h（PEMP FRAME_START
// = 0x7B）与 iec104_master.h 的 FRAME_START (0x68) 冲突。手动清 DataMgr。
//=============================================================================

#include "mini_gtest.h"
#include "iec101_master.h"
#include "pmpc.h"
#include "event_bus.h"
#include <atomic>
#include <cstdio>
#include <fstream>
#include <vector>

// g_running 用 —— 拖 packet_logger 时会需要
std::atomic<bool> g_running{true};

// friend 桥：暴露 HandleGIResponse + config_
class Iec101MasterTestAccess {
public:
    explicit Iec101MasterTestAccess(Iec101Master& m) : m_(m) {}

    // 直接把 config 塞进去（跳过 LoadConfig 的 ini 解析）
    void SetConfig(const Iec101MasterConfig& c) { m_.config_ = c; }

    void HandleGI(const uint8_t* asdu, size_t len, uint16_t coa, int chIdx) {
        m_.HandleGIResponse(asdu, len, coa, chIdx);
    }

private:
    Iec101Master& m_;
};

namespace {

class Iec101MultiChannelTest : public ::testing::Test {
protected:
    void SetUp() override {
        EventBus::Clear();
        RemoteDataMgr::Instance().ClearAll();
        cfgPath_ = "test_iec101_mc_cfg.ini";
        // 两个通道，每通道一个 devNo=10 的设备 —— iec101 里 SetDi 用 coa
        // 作为 dev 参数，所以点表里 devNo 必须与 coa 匹配。
        std::ofstream f(cfgPath_);
        f << "[Channel_1]\n" << "Dev_10=2,0,0,0\n";
        f << "[Channel_2]\n" << "Dev_10=2,0,0,0\n";
        f.close();
        RemoteDataMgr::Instance().LoadConfig(cfgPath_);
    }
    void TearDown() override {
        std::remove(cfgPath_.c_str());
        RemoteDataMgr::Instance().ClearAll();
        EventBus::Clear();
    }

    // 构造一个 GI 响应 ASDU：header 6 字节 + 一个 IO (IOA 3 + val 1)
    // header: type(1) VSQ(1) COT(1) 0 coa_lo coa_hi
    // IO:    ioa[3]  val(1)
    std::vector<uint8_t> BuildGIAsdu(uint16_t coa, uint32_t ioa, bool val) {
        std::vector<uint8_t> a{
            IecType::M_SP_NA_1, 0x01, IecCOT::ACTIVATION_CON, 0x00,
            static_cast<uint8_t>(coa & 0xFF),
            static_cast<uint8_t>((coa >> 8) & 0xFF),
            static_cast<uint8_t>(ioa & 0xFF),
            static_cast<uint8_t>((ioa >> 8) & 0xFF),
            static_cast<uint8_t>((ioa >> 16) & 0xFF),
            static_cast<uint8_t>(val ? 0x01 : 0x00),
        };
        return a;
    }

    // 建一个 2-通道 config，每通道有 1 设备（coa=10）、1 个 DI (ioa=100 pt=2)
    Iec101MasterConfig MakeTwoChannelConfig() {
        Iec101MasterConfig cfg;
        for (int i = 0; i < 2; ++i) {
            Iec101ChannelConfig ch;
            Iec101DeviceConfig dev;
            dev.linkAddr = static_cast<uint16_t>(i + 1);
            dev.coa      = 10;
            Iec101DIMapping di;
            di.ioa = 100;
            di.point = 2;   // pt=1 保留给通讯状态
            dev.diList.push_back(di);
            ch.devices.push_back(dev);
            cfg.channels.push_back(ch);
        }
        return cfg;
    }

    std::string cfgPath_;
};

// M11 关键回归：chIdx=0 → 只影响 chId=1
TEST_F(Iec101MultiChannelTest, GIResponseGoesToCorrectChannelIndex0) {
    Iec101Master m;
    Iec101MasterTestAccess acc(m);
    acc.SetConfig(MakeTwoChannelConfig());

    auto asdu = BuildGIAsdu(/*coa=*/10, /*ioa=*/100, /*val=*/true);
    acc.HandleGI(asdu.data(), asdu.size(), /*coa=*/10, /*chIdx=*/0);

    DiPoint p1, p2;
    ASSERT_TRUE(RemoteDataMgr::Instance().GetDi(1, 10, 2, p1));
    EXPECT_TRUE(p1.value);   // 通道 1 收到了
    ASSERT_TRUE(RemoteDataMgr::Instance().GetDi(2, 10, 2, p2));
    EXPECT_FALSE(p2.value);  // 通道 2 未受影响
}

// M11 关键回归：chIdx=1 → 只影响 chId=2
TEST_F(Iec101MultiChannelTest, GIResponseGoesToCorrectChannelIndex1) {
    Iec101Master m;
    Iec101MasterTestAccess acc(m);
    acc.SetConfig(MakeTwoChannelConfig());

    auto asdu = BuildGIAsdu(10, 100, true);
    acc.HandleGI(asdu.data(), asdu.size(), 10, /*chIdx=*/1);

    DiPoint p1, p2;
    ASSERT_TRUE(RemoteDataMgr::Instance().GetDi(1, 10, 2, p1));
    EXPECT_FALSE(p1.value);
    ASSERT_TRUE(RemoteDataMgr::Instance().GetDi(2, 10, 2, p2));
    EXPECT_TRUE(p2.value);
}

// 无效 chIdx：不应写任何数据
TEST_F(Iec101MultiChannelTest, InvalidChIdxIsNoOp) {
    Iec101Master m;
    Iec101MasterTestAccess acc(m);
    acc.SetConfig(MakeTwoChannelConfig());

    auto asdu = BuildGIAsdu(10, 100, true);
    acc.HandleGI(asdu.data(), asdu.size(), 10, /*chIdx=*/99);
    acc.HandleGI(asdu.data(), asdu.size(), 10, /*chIdx=*/-1);

    DiPoint p1, p2;
    RemoteDataMgr::Instance().GetDi(1, 10, 2, p1);
    RemoteDataMgr::Instance().GetDi(2, 10, 2, p2);
    EXPECT_FALSE(p1.value);
    EXPECT_FALSE(p2.value);
}

// 目标 coa 与本通道设备 coa 不匹配 → 不写
TEST_F(Iec101MultiChannelTest, MismatchedCoaIsIgnored) {
    Iec101Master m;
    Iec101MasterTestAccess acc(m);
    acc.SetConfig(MakeTwoChannelConfig());

    auto asdu = BuildGIAsdu(/*coa=*/99, 100, true);
    acc.HandleGI(asdu.data(), asdu.size(), 99, 0);

    DiPoint p1;
    RemoteDataMgr::Instance().GetDi(1, 10, 2, p1);
    EXPECT_FALSE(p1.value);
}

} // namespace
