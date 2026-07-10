//=============================================================================
// test_data_mgr.cxx — RemoteDataMgr 四遥数据管理器单元测试
// 涵盖: LoadConfig / Get / Set / EventBus 事件触发 / 非存在点位
//=============================================================================

#include "mini_gtest.h"
#include "pmpc.h"
#include "event_bus.h"
#include <fstream>
#include <cstdio>
#include <atomic>

class DataMgrTest : public ::testing::Test {
protected:
    void SetUp() override {
        tmpPath_ = "test_point_cfg_tmp.ini";
        {
            std::ofstream f(tmpPath_);
            f << "[global]" << std::endl;
            f << "desc=test config" << std::endl;
            f << "[Channel_1]" << std::endl;
            // Dev_1: 3 DI (pts 1-4), 2 AI, 2 DO, 1 AO
            f << "Dev_1=3,2,2,1" << std::endl;
            // Dev_2: 1 DI (pts 1-2), 1 AI
            f << "Dev_2=1,1,0,0" << std::endl;
            f << "[Channel_2]" << std::endl;
            // Dev_1: 2 DI (pts 1-3), 3 AI, 1 DO, 2 AO
            f << "Dev_1=2,3,1,2" << std::endl;
        }

        auto& mgr = RemoteDataMgr::Instance();
        mgr.LoadConfig(tmpPath_);
    }

    void TearDown() override {
        RemoteDataMgr::Instance().ClearAll();
        std::remove(tmpPath_.c_str());
    }

    std::string tmpPath_;
};

// ==================== LoadConfig ====================

TEST_F(DataMgrTest, LoadConfigChannels) {
    auto channels = RemoteDataMgr::Instance().GetChannelIds();
    ASSERT_EQ(channels.size(), 2);
    EXPECT_EQ(channels[0], 1);
    EXPECT_EQ(channels[1], 2);
}

TEST_F(DataMgrTest, LoadConfigDevices) {
    auto devs1 = RemoteDataMgr::Instance().GetDeviceIds(1);
    ASSERT_EQ(devs1.size(), 2);
    EXPECT_EQ(devs1[0], 1);
    EXPECT_EQ(devs1[1], 2);

    auto devs2 = RemoteDataMgr::Instance().GetDeviceIds(2);
    ASSERT_EQ(devs2.size(), 1);
    EXPECT_EQ(devs2[0], 1);
}

TEST_F(DataMgrTest, LoadConfigNonExistentChannel) {
    auto devs = RemoteDataMgr::Instance().GetDeviceIds(99);
    EXPECT_TRUE(devs.empty());
}

// ==================== DI (遥信) ====================

TEST_F(DataMgrTest, SetAndGetDi) {
    auto& mgr = RemoteDataMgr::Instance();

    // pt 1 = comm status (always exists)
    DiPoint out;
    EXPECT_TRUE(mgr.GetDi(1, 1, 1, out));
    EXPECT_FALSE(out.value);

    // Set pt 2 (first business DI)
    EXPECT_TRUE(mgr.SetDi(1, 1, 2, true, 1000, true));

    EXPECT_TRUE(mgr.GetDi(1, 1, 2, out));
    EXPECT_TRUE(out.value);
    EXPECT_EQ(out.tsMs, 1000);
}

TEST_F(DataMgrTest, DiPoint1IsCommStatus) {
    // Point 1 is reserved for comm status; SetDi with pt=1 should NOT
    // publish an EventBus event (but still write the value)
    std::atomic<bool> eventFired{false};
    auto token = EventBus::Subscribe<DIChange>([&](const DIChange&) {
        eventFired = true;
    });

    ASSERT_TRUE(RemoteDataMgr::Instance().SetDi(1, 1, 1, true, 100, true));

    DiPoint out;
    ASSERT_TRUE(RemoteDataMgr::Instance().GetDi(1, 1, 1, out));
    EXPECT_TRUE(out.value);   // Value still written
    EXPECT_FALSE(eventFired.load());  // But no event

    EventBus::Unsubscribe<DIChange>(token);
}

TEST_F(DataMgrTest, DiFiresEventBusForNonCommPoints) {
    std::atomic<int> eventCount{0};
    DIChange received{};
    auto token = EventBus::Subscribe<DIChange>([&](const DIChange& e) {
        eventCount++;
        received = e;
    });

    ASSERT_TRUE(RemoteDataMgr::Instance().SetDi(1, 1, 2, true, 5000, true));
    EXPECT_EQ(eventCount.load(), 1);
    EXPECT_EQ(received.channel, 1);
    EXPECT_EQ(received.device, 1);
    EXPECT_EQ(received.point, 2);
    EXPECT_TRUE(received.value);
    EXPECT_EQ(received.tsMs, 5000);

    EventBus::Unsubscribe<DIChange>(token);
}

// ==================== AI (遥测) ====================

TEST_F(DataMgrTest, SetAndGetAi) {
    auto& mgr = RemoteDataMgr::Instance();

    AiPoint out;
    EXPECT_TRUE(mgr.GetAi(1, 1, 1, out));
    EXPECT_DOUBLE_EQ(out.value, 0.0);

    EXPECT_TRUE(mgr.SetAi(1, 1, 1, 42.5));

    EXPECT_TRUE(mgr.GetAi(1, 1, 1, out));
    EXPECT_DOUBLE_EQ(out.value, 42.5);
    // lastVal is only updated by CheckAllPointChange, not by GetAi
    EXPECT_DOUBLE_EQ(out.lastVal, 0.0);
}

TEST_F(DataMgrTest, AiFiresEventBus) {
    std::atomic<bool> eventFired{false};
    auto token = EventBus::Subscribe<AIChange>([&](const AIChange& e) {
        eventFired = true;
        EXPECT_EQ(e.channel, 1);
        EXPECT_EQ(e.device, 1);
        EXPECT_EQ(e.point, 1);
        EXPECT_DOUBLE_EQ(e.value, 99.9);
    });

    RemoteDataMgr::Instance().SetAi(1, 1, 1, 99.9);
    EXPECT_TRUE(eventFired.load());

    EventBus::Unsubscribe<AIChange>(token);
}

// ==================== DO (遥控) ====================

TEST_F(DataMgrTest, SetAndGetDo) {
    auto& mgr = RemoteDataMgr::Instance();

    DoPoint out;
    EXPECT_TRUE(mgr.GetDo(1, 1, 1, out));
    EXPECT_FALSE(out.masterVal);
    EXPECT_FALSE(out.slaveVal);

    EXPECT_TRUE(mgr.SetDoMaster(1, 1, 1, true));
    EXPECT_TRUE(mgr.SetDoSlave(1, 1, 1, true));

    EXPECT_TRUE(mgr.GetDo(1, 1, 1, out));
    EXPECT_TRUE(out.masterVal);
    EXPECT_TRUE(out.slaveVal);
}

TEST_F(DataMgrTest, DoFiresEventBus) {
    std::atomic<int> eventCount{0};
    auto token = EventBus::Subscribe<DOChange>([&](const DOChange& e) {
        eventCount++;
        EXPECT_EQ(e.channel, 1);
        EXPECT_EQ(e.device, 1);
        EXPECT_EQ(e.point, 1);
    });

    RemoteDataMgr::Instance().SetDoMaster(1, 1, 1, true);
    EXPECT_EQ(eventCount.load(), 1);

    EventBus::Unsubscribe<DOChange>(token);
}

// ==================== AO (遥调) ====================

TEST_F(DataMgrTest, SetAndGetAo) {
    auto& mgr = RemoteDataMgr::Instance();

    AoPoint out;
    EXPECT_TRUE(mgr.GetAo(1, 1, 1, out));
    EXPECT_DOUBLE_EQ(out.value, 0.0);

    EXPECT_TRUE(mgr.SetAo(1, 1, 1, 75.0));

    EXPECT_TRUE(mgr.GetAo(1, 1, 1, out));
    EXPECT_DOUBLE_EQ(out.value, 75.0);
}

TEST_F(DataMgrTest, AoFiresEventBus) {
    std::atomic<bool> eventFired{false};
    auto token = EventBus::Subscribe<AOChange>([&](const AOChange& e) {
        eventFired = true;
        EXPECT_EQ(e.channel, 1);
        EXPECT_EQ(e.device, 1);
        EXPECT_EQ(e.point, 1);
        EXPECT_DOUBLE_EQ(e.value, 80.0);
    });

    RemoteDataMgr::Instance().SetAo(1, 1, 1, 80.0);
    EXPECT_TRUE(eventFired.load());

    EventBus::Unsubscribe<AOChange>(token);
}

// ==================== 非存在点位 ====================

TEST_F(DataMgrTest, GetNonExistentPoint) {
    auto& mgr = RemoteDataMgr::Instance();

    DiPoint diOut;
    EXPECT_FALSE(mgr.GetDi(1, 1, 99, diOut));   // pt 99 doesn't exist

    AiPoint aiOut;
    EXPECT_FALSE(mgr.GetAi(1, 1, 99, aiOut));
}

TEST_F(DataMgrTest, SetNonExistentPoint) {
    auto& mgr = RemoteDataMgr::Instance();
    EXPECT_FALSE(mgr.SetDi(1, 1, 99, true, 0, true));
    EXPECT_FALSE(mgr.SetAi(1, 1, 99, 1.0));
    EXPECT_FALSE(mgr.SetDoMaster(1, 1, 99, true));
    EXPECT_FALSE(mgr.SetDoSlave(1, 1, 99, true));
    EXPECT_FALSE(mgr.SetAo(1, 1, 99, 1.0));
}

// ==================== 非存在通道/设备 ====================

TEST_F(DataMgrTest, NonExistentChannel) {
    auto& mgr = RemoteDataMgr::Instance();
    DiPoint out;
    EXPECT_FALSE(mgr.GetDi(99, 1, 1, out));
    EXPECT_FALSE(mgr.SetDi(99, 1, 1, true, 0, true));
}

TEST_F(DataMgrTest, NonExistentDevice) {
    auto& mgr = RemoteDataMgr::Instance();
    DiPoint out;
    EXPECT_FALSE(mgr.GetDi(1, 99, 1, out));
    EXPECT_FALSE(mgr.SetDi(1, 99, 1, true, 0, true));
}

// ==================== 批量读取 ====================

TEST_F(DataMgrTest, GetDiList) {
    std::vector<DiPoint> list;
    EXPECT_TRUE(RemoteDataMgr::Instance().GetDiList(1, 1, list));
    // 3 business DI + 1 comm status = 4
    ASSERT_EQ(list.size(), 4);
    EXPECT_EQ(list[0].pointNo, 1);  // pt 1 = comm status

    // Device 2 has 1 DI (pt 1 only, since 1 DI means 2 total pts)
    EXPECT_TRUE(RemoteDataMgr::Instance().GetDiList(1, 2, list));
    ASSERT_EQ(list.size(), 2);
}

TEST_F(DataMgrTest, GetAiList) {
    std::vector<AiPoint> list;
    EXPECT_TRUE(RemoteDataMgr::Instance().GetAiList(1, 1, list));
    ASSERT_EQ(list.size(), 2);
    EXPECT_EQ(list[0].pointNo, 1);
    EXPECT_EQ(list[1].pointNo, 2);
}

TEST_F(DataMgrTest, GetDoList) {
    std::vector<DoPoint> list;
    EXPECT_TRUE(RemoteDataMgr::Instance().GetDoList(1, 1, list));
    ASSERT_EQ(list.size(), 2);
}

TEST_F(DataMgrTest, GetAoList) {
    std::vector<AoPoint> list;
    EXPECT_TRUE(RemoteDataMgr::Instance().GetAoList(1, 1, list));
    ASSERT_EQ(list.size(), 1);
}

TEST_F(DataMgrTest, GetListNonExistentDevice) {
    std::vector<DiPoint> list;
    EXPECT_FALSE(RemoteDataMgr::Instance().GetDiList(99, 1, list));
    EXPECT_FALSE(RemoteDataMgr::Instance().GetDiList(1, 99, list));
}

// ==================== 综合读写 ====================

TEST_F(DataMgrTest, ReadWriteAllTypes) {
    auto& mgr = RemoteDataMgr::Instance();

    // Write
    EXPECT_TRUE(mgr.SetDi(1, 1, 2, true, 100, true));
    EXPECT_TRUE(mgr.SetAi(1, 1, 1, 36.5));
    EXPECT_TRUE(mgr.SetDoMaster(1, 1, 1, true));
    EXPECT_TRUE(mgr.SetDoSlave(1, 1, 1, true));
    EXPECT_TRUE(mgr.SetAo(1, 1, 1, 80.0));

    // Read back
    DiPoint di; mgr.GetDi(1, 1, 2, di);
    EXPECT_TRUE(di.value);

    AiPoint ai; mgr.GetAi(1, 1, 1, ai);
    EXPECT_DOUBLE_EQ(ai.value, 36.5);

    DoPoint d; mgr.GetDo(1, 1, 1, d);
    EXPECT_TRUE(d.masterVal);
    EXPECT_TRUE(d.slaveVal);

    AoPoint ao; mgr.GetAo(1, 1, 1, ao);
    EXPECT_DOUBLE_EQ(ao.value, 80.0);
}

// ==================== 多通道隔离 ====================

TEST_F(DataMgrTest, MultiChannelIsolation) {
    auto& mgr = RemoteDataMgr::Instance();

    // Channel 1, Device 2 has 1 AI
    EXPECT_TRUE(mgr.SetAi(1, 2, 1, 10.0));

    // Channel 2, Device 1 has 3 AI
    EXPECT_TRUE(mgr.SetAi(2, 1, 1, 20.0));

    // Verify independence
    AiPoint a1, a2;
    mgr.GetAi(1, 2, 1, a1);
    mgr.GetAi(2, 1, 1, a2);
    EXPECT_DOUBLE_EQ(a1.value, 10.0);
    EXPECT_DOUBLE_EQ(a2.value, 20.0);
}
