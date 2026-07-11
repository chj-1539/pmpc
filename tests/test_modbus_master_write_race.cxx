//=============================================================================
// test_modbus_master_write_race.cxx
//
// 回归 CLAUDE.md bug #5：ModbusTcpMaster::WriteDOChanges / WriteAOChanges
// 曾用 DoPoint::lastMaster / AoPoint::lastVal 判断"是否需要写回"，但
// CheckAllPointChange（200ms）在 master 500ms 采集循环之间同步了这两个
// 字段，导致下一轮遥控被误判为"无变化"，写不出。
//
// 修复：模块级独立追踪 doSent_ / aoSent_。本测试直接驱动 WriteDOChanges
// (经 ModbusTcpMasterTestAccess friend 桥暴露)，用 FakeModbusSlave 观察
// 实际线上请求数量。
//=============================================================================

#include "mini_gtest.h"
#include "pmpc_test_fixture.h"
#include "modbus_tcp_master.h"
#include "pmpc.h"
#include "harness/fake_modbus_slave.h"
#include <chrono>
#include <cstdio>
#include <fstream>
#include <thread>

// g_running 通常在 main.cxx 里定义并由 pemp_server/redundancy 等模块引用；
// 单测里我们不链接 main.cxx，这里给个静态定义占位。
std::atomic<bool> g_running{true};

using pmpc::testing::GlobalStateFixture;
using pmpc::testing::harness::FakeModbusSlave;

// friend 桥：暴露 WriteDOChanges/WriteAOChanges 和 doSent_/aoSent_
class ModbusTcpMasterTestAccess {
public:
    explicit ModbusTcpMasterTestAccess(ModbusTcpMaster& m) : m_(m) {}
    void WriteDo(socket& sock, uint16_t& transId, const DeviceConfig& dev) {
        m_.WriteDOChanges(sock, transId, dev);
    }
    void WriteAo(socket& sock, uint16_t& transId, const DeviceConfig& dev) {
        m_.WriteAOChanges(sock, transId, dev);
    }
    size_t DoSentSize() {
        std::lock_guard<std::mutex> lk(m_.sentMtx_);
        return m_.doSent_.size();
    }
private:
    ModbusTcpMaster& m_;
};

namespace {

class ModbusWriteRaceTest : public GlobalStateFixture {
protected:
    void SetUp() override {
        GlobalStateFixture::SetUp();
        // 一个通道，一个设备，一个 DO 点、一个 AO 点
        cfgPath_ = "test_modbus_write_race_cfg.ini";
        std::ofstream f(cfgPath_);
        f << "[Channel_1]\n" << "Dev_1=0,0,1,1\n";
        f.close();
        RemoteDataMgr::Instance().LoadConfig(cfgPath_);
    }
    void TearDown() override {
        std::remove(cfgPath_.c_str());
        GlobalStateFixture::TearDown();
    }

    // 手工构造一个映射：本地 DO pt=1 → Modbus FC05 addr=100
    DeviceConfig MakeDeviceConfig() {
        DeviceConfig dev;
        dev.channel   = 1;
        dev.stationId = 1;
        DOMapping do1;
        do1.point = 1;
        do1.func  = 5;
        do1.addr  = 100;
        do1.pulseMs = 0;     // 关闭 pulse 队列，避免污染其他测试
        dev.doList.push_back(do1);
        return dev;
    }

    std::string cfgPath_;
    wsa_guard   wsa_;   // socket API 需要
};

// 关键场景：同一次遥控只落地一次线上 FC05。
// 具体流程：
//   1) SetDoMaster(1, 1, 1, true) —— 遥控合闸
//   2) WriteDOChanges → fake slave 应收到 1 次 FC05(coil=ON)
//   3) 立即再调 WriteDOChanges（模拟 master 下一轮采集循环）→ 0 次新请求
//      因为修复：write 完立即 masterVal ← false 且 doSent_[key]=false，两者一致。
TEST_F(ModbusWriteRaceTest, SingleRemoteControlSendsExactlyOneWrite) {
    FakeModbusSlave slave;
    slave.Start();

    ModbusTcpMaster master;
    ModbusTcpMasterTestAccess acc(master);

    // 连到 fake slave
    socket sock;
    sock.connect("127.0.0.1", slave.Port());
    sock.set_recv_timeout(std::chrono::milliseconds(500));

    auto dev = MakeDeviceConfig();
    uint16_t transId = 0;

    // 场景开始：SetDoMaster(true)
    ASSERT_TRUE(RemoteDataMgr::Instance().SetDoMaster(1, 1, 1, true));
    acc.WriteDo(sock, transId, dev);
    EXPECT_EQ(slave.RequestCount(), static_cast<size_t>(1));

    // 立即再调一次 —— 不应新增请求（modbus_tcp_master.cxx:1296-1301 已把
    // masterVal 复位、doSent_[key]=false，两者一致 → 跳过）
    acc.WriteDo(sock, transId, dev);
    EXPECT_EQ(slave.RequestCount(), static_cast<size_t>(1));

    sock.close();
    slave.Stop();
}

// bug #5 的关键：CheckAllPointChange 在采集循环之间跑一次，把
// DoPoint::lastMaster 同步为 masterVal 后，第二次 SetDoMaster(true) 仍
// 应能触发写入。修复前代码依赖 lastMaster 差异判断，会漏发；修复后依赖
// 模块级 doSent_，不受 lastMaster 影响。
TEST_F(ModbusWriteRaceTest, SecondRemoteControlAfterCheckAllPointChangeStillFires) {
    FakeModbusSlave slave;
    slave.Start();

    ModbusTcpMaster master;
    ModbusTcpMasterTestAccess acc(master);

    socket sock;
    sock.connect("127.0.0.1", slave.Port());
    sock.set_recv_timeout(std::chrono::milliseconds(500));

    auto dev = MakeDeviceConfig();
    uint16_t transId = 0;

    // 第一次遥控
    ASSERT_TRUE(RemoteDataMgr::Instance().SetDoMaster(1, 1, 1, true));
    acc.WriteDo(sock, transId, dev);
    ASSERT_EQ(slave.RequestCount(), static_cast<size_t>(1));

    // 抢跑 CheckAllPointChange —— 这是 bug #5 的触发条件：主循环会把
    // DoPoint 的 lastMaster 追平 masterVal。修复前 master 下一轮的差异
    // 检测就失效了。修复后 doSent_ 独立追踪，不受影响。
    RemoteDataMgr::Instance().CheckAllPointChange();

    // 第二次真变化
    ASSERT_TRUE(RemoteDataMgr::Instance().SetDoMaster(1, 1, 1, true));
    acc.WriteDo(sock, transId, dev);
    EXPECT_EQ(slave.RequestCount(), static_cast<size_t>(2));

    sock.close();
    slave.Stop();
}

// 保证 CheckAllPointChange 跑很多次也不会静默把 masterVal 撤回：
// 两次真实遥控之间跑多轮 CheckAllPointChange，仍应产生两次线上 FC05。
TEST_F(ModbusWriteRaceTest, MultipleCheckAllPointChangeBetweenControls) {
    FakeModbusSlave slave;
    slave.Start();

    ModbusTcpMaster master;
    ModbusTcpMasterTestAccess acc(master);

    socket sock;
    sock.connect("127.0.0.1", slave.Port());
    sock.set_recv_timeout(std::chrono::milliseconds(500));

    auto dev = MakeDeviceConfig();
    uint16_t transId = 0;

    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(RemoteDataMgr::Instance().SetDoMaster(1, 1, 1, true));
        acc.WriteDo(sock, transId, dev);
        // 每次遥控后多轮 CheckAllPointChange
        for (int j = 0; j < 3; ++j)
            RemoteDataMgr::Instance().CheckAllPointChange();
        acc.WriteDo(sock, transId, dev);
    }
    EXPECT_EQ(slave.RequestCount(), static_cast<size_t>(5));

    sock.close();
    slave.Stop();
}

} // namespace
