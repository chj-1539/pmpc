#ifndef IEC104_SLAVE_H
#define IEC104_SLAVE_H

//=============================================================================
// iec104_slave.h — IEC 60870-5-104 从站模块
//
// 功能：
//   监听 TCP 端口，接收 IEC 104 主站连接
//   - 总召唤响应 (C_IC_NA_1 → GI 全量回复)
//   - 遥控执行 (C_SC_NA_1 / C_DC_NA_1 → SetDoMaster)
//   - 遥调执行 (C_SE_NC_1 → SetAo)
//   - DI 变化上送 (EventBus → M_SP_TB_1)
//   - AI 循环/变化上送 (三模式：change/cycle/both)
//   - 时钟同步 (C_CS_NA_1)
//=============================================================================

#include "socket.h"
#include "pmpc.h"
#include "module_manager.h"
#include "event_bus.h"
#include "iec104_master.h"   // 复用 IecType, IecCOT, APCI 常量
#include <string>
#include <vector>
#include <map>
#include <set>
#include <memory>
#include <atomic>
#include <thread>
#include <mutex>
#include <cstdint>
#include <functional>
#include <chrono>

// ═══════════════════════════════════════════════════════════════════════════
// 配置结构
// ═══════════════════════════════════════════════════════════════════════════

/// DI 映射条目
struct SlaveDIMapping {
    uint32_t ioa = 0;
    uint16_t ch = 0;
    uint16_t dev = 0;
    uint16_t point = 0;
};

/// AI 映射条目
struct SlaveAIMapping {
    uint32_t ioa = 0;
    uint8_t  type = IecType::M_ME_NC_1;
    uint16_t ch = 0;
    uint16_t dev = 0;
    uint16_t point = 0;
    double   scale = 1.0;
    double   offset = 0.0;
};

/// 电度映射
struct SlaveEnergyMapping {
    uint32_t ioa = 0;
    uint16_t ch = 0;
    uint16_t dev = 0;
    uint16_t point = 0;
    double   scale = 1.0;
    double   offset = 0.0;
};

/// DO 映射条目（遥控）
struct SlaveDOMapping {
    uint32_t ioa = 0;
    int      val = 0;
    uint16_t ch = 0;
    uint16_t dev = 0;
    uint16_t point = 0;
};

/// AO 映射条目（遥调）
struct SlaveAOMapping {
    uint32_t ioa = 0;
    uint16_t ch = 0;
    uint16_t dev = 0;
    uint16_t point = 0;
    double   scale = 1.0;
    double   offset = 0.0;
};

/// 设备配置
struct SlaveDeviceConfig {
    uint16_t commonAddr = 0;
    std::string desc;
    std::map<uint32_t, SlaveDIMapping>    diMap;     // ioa → entry
    std::map<uint32_t, SlaveAIMapping>    aiMap;     // ioa → entry
    std::map<uint32_t, SlaveEnergyMapping> energyMap; // ioa → entry
    std::map<uint32_t, std::vector<SlaveDOMapping>> doMap;    // ioa → [ops]
    std::map<uint32_t, SlaveAOMapping>    aoMap;     // ioa → entry
};

/// 从站全局配置
struct SlaveConfig {
    int port = 2404;
    int maxClients = 10;
    int verbose = 1;
    int t0 = 30, t1 = 15, t2 = 10, t3 = 20, k = 12, w = 8;
    int spontaneousWithTimestamp = 1;
    std::string aiUploadMode = "change"; // change / cycle / both
    int aiCycleS = 60;
    int clockSyncEnable = 1;
    int adjustLocalClock = 0;
    std::vector<SlaveDeviceConfig> devices;
};

// ═══════════════════════════════════════════════════════════════════════════
// IEC 104 从站类
// ═══════════════════════════════════════════════════════════════════════════

class Iec104Slave {
public:
    Iec104Slave();
    ~Iec104Slave();

    bool LoadConfig(const std::string& path);
    bool Start();
    void Stop();
    bool IsRunning() const { return running_; }

    // C_SC_NA_1 遥控决策纯函数（H5 修复）。给定某个 IOA 上的 DO mapping
    // 列表和帧内 cmdVal，返回**应该**触发 SetDoMaster 的 (ch, dev, point)
    // 列表。若无匹配则返回空 vector —— 修复前老代码会 fallback 到
    // doMap[0]，把不匹配的请求当"随便找一个"来执行，属于安全隐患。
    //
    // 返回空表示 negative acknowledgment（COT.P/N=1），调用方不应写 DO。
    // 每个匹配 entry 的 doVal = (cmdVal != 0)。
    struct RemoteControlTarget {
        uint16_t ch = 0, dev = 0, point = 0;
        bool     doVal = false;
    };
    // inline 定义（见类外）避免测试文件链入 iec104_slave.cxx 的一整棵传递依赖图。
    static std::vector<RemoteControlTarget>
    DecideRemoteControlTargets(const std::vector<SlaveDOMapping>& mapping,
                               int cmdVal);

    // M4 修复：Iec104Slave 内部用来跟踪 client 线程；测试也需要直接构造。
    // 每个 entry 携带一个 shared_ptr<atomic<bool>> done 标志，工作线程
    // return 前置位；CleanupFinishedClientThreads 基于该标志回收已结束线程。
    struct ClientThreadEntry {
        std::thread                thr;
        std::shared_ptr<std::atomic<bool>> done;
    };
    static inline size_t CleanupFinishedClientThreads(std::vector<ClientThreadEntry>& threads) {
        size_t removed = 0;
        for (auto it = threads.begin(); it != threads.end(); ) {
            if (!it->done || !it->done->load()) { ++it; continue; }
            if (it->thr.joinable()) it->thr.join();
            it = threads.erase(it);
            ++removed;
        }
        return removed;
    }

private:
    struct SlaveBind {
        std::string allowedIP;
        uint16_t port;
    };
    // ── TCP 服务 ──
    void AcceptLoop(const SlaveBind& bind, socket& listenSock);
    void ClientThread(socket clientSock);

    // ── 帧收发 ──
    bool SendUFrame(socket& sock, uint32_t ctrl);
    bool SendSFrame(socket& sock, uint32_t rNr);
    bool SendIFrame(socket& sock, uint32_t& sNr, uint32_t rNr,
                    const uint8_t* asdu, size_t asduLen);
    bool RecvFrame(socket& sock, int timeoutMs, uint8_t* buf, size_t& len);

    // ── 帧处理 ──
    bool HandleFrame(socket& sock, const uint8_t* buf, size_t len,
                     uint32_t& sNr, uint32_t& rNr,
                     SlaveDeviceConfig*& currentDev);

    // ── ASDU 构建 ──
    void BuildGIResponseDI(socket& sock, uint32_t& sNr, uint32_t rNr,
                           const SlaveDeviceConfig& dev);
    void BuildGIResponseAI(socket& sock, uint32_t& sNr, uint32_t rNr,
                           const SlaveDeviceConfig& dev);
    void BuildGIResponseEnergy(socket& sock, uint32_t& sNr, uint32_t rNr,
                                const SlaveDeviceConfig& dev);
    void SendDIActiveUpload(const DIChange& ev);
    void SendAIActiveUpload(const AIChange& ev, uint32_t ioa,
                            const SlaveAIMapping& ai, const SlaveDeviceConfig& dev);

    // ── 定时器线程（AI 循环上送） ──
    void TimerThread();

    // ── 辅助 ──
    SlaveDeviceConfig* FindDevice(uint16_t commonAddr);
    uint64_t NowMs() const;
    static void PackFloatASDU(uint8_t* buf, size_t& pos, double value, uint32_t ioa);
    static void PackNormalizedASDU(uint8_t* buf, size_t& pos, double value, uint32_t ioa, double scale);
    static void PackCP56Time2a(uint8_t* buf, size_t& pos);
    static uint32_t ReadCtrl(const uint8_t* buf);
    static uint32_t GetSNr(uint32_t ctrl);
    static uint32_t GetRNr(uint32_t ctrl);
    static int SafeStoi(const std::string& s, int def = 0);
    static double SafeStod(const std::string& s, double def = 0.0);

    SlaveConfig config_;
    std::vector<SlaveBind> binds_;
    std::atomic<bool> running_{false};
    std::vector<socket> listenSocks_;
    std::vector<std::thread> acceptThreads_;

    // 客户端线程跟踪（结构与 helper 定义在 public 部分，见上）
    std::vector<ClientThreadEntry> clientThreads_;
    std::mutex clientMtx_;

    // EventBus 订阅令牌
    size_t tokenDI_ = 0;
    size_t tokenAI_ = 0;

    // 定时器线程（AI 循环上送）
    std::thread timerThr_;

    // 上行帧发送（所有连接的客户端共享）
    // 主动上送需遍历所有客户端发送
    struct ClientInfo {
        socket sock;
        uint32_t sNr = 0;
        uint32_t rNr = 0;
    };
    std::vector<ClientInfo> clients_;
    std::mutex clientsMtx_;
};

// H5：inline 定义在类外，测试文件仅 include 头文件即可直接调用
inline std::vector<Iec104Slave::RemoteControlTarget>
Iec104Slave::DecideRemoteControlTargets(const std::vector<SlaveDOMapping>& mapping,
                                        int cmdVal)
{
    std::vector<RemoteControlTarget> targets;
    const bool doVal = (cmdVal != 0);
    for (const auto& m : mapping) {
        if (m.val == cmdVal) {
            RemoteControlTarget t;
            t.ch    = m.ch;
            t.dev   = m.dev;
            t.point = m.point;
            t.doVal = doVal;
            targets.push_back(t);
        }
    }
    return targets;
}

// ==================== AppModule 包装 ====================

class Iec104SlaveModule : public AppModule {
public:
    Iec104SlaveModule();
    ~Iec104SlaveModule() override;
    const char* Name() const override { return "iec104_slave"; }
    bool LoadConfig(const std::string& cfgPath) override;
    bool ValidateConfig(const std::string& cfgPath,
                        std::vector<std::string>& errors) override;
    bool Start() override;
    void Stop() override;
    bool IsRunning() const override;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

#endif // IEC104_SLAVE_H
