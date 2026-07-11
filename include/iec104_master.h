#ifndef IEC104_MASTER_H
#define IEC104_MASTER_H

//=============================================================================
// iec104_master.h — IEC 60870-5-104 主站采集模块
//
// 层次：global → Channel → Template → Device
// 每通道独立线程，长连接 + 总召唤 + 主动上报
//=============================================================================

#include "socket.h"
#include "pmpc.h"
#include "module_manager.h"
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <atomic>
#include <thread>
#include <mutex>
#include <cstdint>
#include <functional>
#include <chrono>

// ═══════════════════════════════════════════════════════════════════════════
// IEC 104 常量定义
// ═══════════════════════════════════════════════════════════════════════════

// ─── 帧类型 ───
constexpr uint8_t FRAME_START = 0x68;

// ─── U-format 控制域 ───
constexpr uint32_t CTRL_U_STARTDT_ACT  = 0x07000000;
constexpr uint32_t CTRL_U_STARTDT_CON  = 0x0B000000;
constexpr uint32_t CTRL_U_STOPDT_ACT   = 0x13000000;
constexpr uint32_t CTRL_U_STOPDT_CON   = 0x23000000;
constexpr uint32_t CTRL_U_TESTFR_ACT   = 0x43000000;
constexpr uint32_t CTRL_U_TESTFR_CON   = 0x83000000;

// ─── ASDU 类型标识 ───
namespace IecType {
    // 遥信
    constexpr uint8_t M_SP_NA_1  = 0x01;  // 单点遥信
    constexpr uint8_t M_SP_TB_1  = 0x1E;  // 单点遥信（带时标 CP56Time2a）
    constexpr uint8_t M_DP_NA_1  = 0x03;  // 双点遥信
    constexpr uint8_t M_DP_TB_1  = 0x1F;  // 双点遥信（带时标）

    // 遥测
    constexpr uint8_t M_ME_NA_1  = 0x09;  // 归一化值
    constexpr uint8_t M_ME_NB_1  = 0x0B;  // 标度化值
    constexpr uint8_t M_ME_NC_1  = 0x0D;  // 短浮点数
    constexpr uint8_t M_ME_ND_1  = 0x0A;  // 无品质描述遥测
    constexpr uint8_t M_ME_TD_1  = 0x1A;  // 带时标的短浮点数

    // 电度
    constexpr uint8_t M_IT_NA_1  = 0x15;  // 电能脉冲计数

    // 遥控命令
    constexpr uint8_t C_SC_NA_1  = 0x2D;  // 单点遥控
    constexpr uint8_t C_DC_NA_1  = 0x2E;  // 双点遥控
    constexpr uint8_t C_SE_NC_1  = 0x32;  // 设定值（float）
    constexpr uint8_t C_SE_NA_1  = 0x30;  // 设定值（归一化）

    // 总召唤
    constexpr uint8_t C_IC_NA_1  = 0x64;  // 总召唤命令
    constexpr uint8_t C_IC_NA_1_ACT_CON = 0x64; // 总召唤激活确认/终止
}

// ─── 传输原因 (COT) ───
namespace IecCOT {
    constexpr uint8_t PERIODIC        = 0x01;
    constexpr uint8_t BACKGROUND      = 0x02;
    constexpr uint8_t SPONTANEOUS     = 0x03;
    constexpr uint8_t INITIALIZED     = 0x04;
    constexpr uint8_t REQUEST         = 0x05;
    constexpr uint8_t ACTIVATION      = 0x06;
    constexpr uint8_t ACTIVATION_CON  = 0x07;
    constexpr uint8_t DEACTIVATION    = 0x08;
    constexpr uint8_t DEACTIVATION_CON = 0x09;
    constexpr uint8_t ACTIVATION_TERM = 0x0A;
    constexpr uint8_t RETURN_INFO_REMOTE = 0x14;
}

// ═══════════════════════════════════════════════════════════════════════════
// 配置结构
// ═══════════════════════════════════════════════════════════════════════════

/// AI 映射条目（包含 IEC 104 遥测映射）
struct IEC104AIMapping {
    uint32_t ioa = 0;           // 信息对象地址
    uint8_t  type = IecType::M_ME_NC_1;
    uint16_t ch = 0;            // 本地通道号
    uint16_t dev = 0;           // 本地设备号
    uint16_t point = 1;         // 本地 AI 点号
    double   scale = 1.0;
    double   offset = 0.0;
};

/// DI 映射条目
struct IEC104DIMapping {
    uint32_t ioa = 0;
    uint8_t  type = IecType::M_SP_NA_1;
    uint16_t ch = 0;
    uint16_t dev = 0;
    uint16_t point = 1;
};

/// DO 映射条目（遥控，含命令值）
struct IEC104DOMapping {
    uint32_t ioa = 0;
    int      val = 0;           // 命令值（0/1 或 1/2 取决于类型）
    uint8_t  type = IecType::C_SC_NA_1;
    uint16_t ch = 0;
    uint16_t dev = 0;
    uint16_t point = 1;
};

/// AO 映射条目（遥调设定值）
struct IEC104AOMapping {
    uint32_t ioa = 0;
    uint8_t  type = IecType::C_SE_NC_1;
    uint16_t ch = 0;
    uint16_t dev = 0;
    uint16_t point = 1;
    double   scale = 1.0;
    double   offset = 0.0;
};

/// 电度映射
struct IEC104EnergyMapping {
    uint32_t ioa = 0;
    uint8_t  type = IecType::M_IT_NA_1;
    uint16_t ch = 0;
    uint16_t dev = 0;
    uint16_t point = 1;
    double   scale = 1.0;
    double   offset = 0.0;
};

/// 设备配置
struct IEC104DeviceConfig {
    uint16_t commonAddr = 0;    // 公共地址
    std::string name;
    std::string templateName;
    std::vector<IEC104DIMapping> diList;
    std::vector<IEC104AIMapping> aiList;
    std::vector<IEC104DOMapping> doList;
    std::vector<IEC104AOMapping> aoList;
    std::vector<IEC104EnergyMapping> energyList;
};

/// 通道配置
struct IEC104ChannelConfig {
    std::string ip;
    uint16_t port = 2404;
    int timeoutMs = 5000;
    int retryCount = 3;
    int retrySleepMs = 10000;
    int verbose = 0;
    int giCycleS = 300;
    int clockSyncEnable = 1;
    int clockSyncIntervalS = 3600;
    int spontaneousEnable = 1;
    // IEC 104 连接参数
    int t0 = 30;
    int t1 = 15;
    int t2 = 10;
    int t3 = 20;
    int k  = 12;
    int w  = 8;
    // 双连接冗余
    std::string standbyIp;
    int standbyPort = 0;
    int fallback = 1;

    std::vector<IEC104DeviceConfig> devices;
};

/// 全局配置
struct IEC104MasterConfig {
    int timeoutMs = 5000;
    int retryCount = 3;
    int retrySleepMs = 10000;
    int verbose = 0;
    int giCycleS = 300;
    int clockSyncEnable = 1;
    int clockSyncIntervalS = 3600;
    int spontaneousEnable = 1;
    std::vector<IEC104ChannelConfig> channels;
};

// ═══════════════════════════════════════════════════════════════════════════
// IEC 104 主站类
// ═══════════════════════════════════════════════════════════════════════════

class Iec104Master {
public:
    Iec104Master();
    ~Iec104Master();

    // 测试挂钩：给 tests/test_iec104_recvframe.cxx 访问 RecvFrame 私有方法
    friend class Iec104MasterTestAccess;

    bool LoadConfig(const std::string& path);
    bool Start();
    void Stop();
    bool IsRunning() const { return running_; }

    const IEC104MasterConfig& GetConfig() const { return config_; }

private:
    // ── 配置解析 ──
    bool ParseGlobal(const std::string& key, const std::string& val);
    bool ParseChannel(const std::string& key, const std::string& val,
                      IEC104ChannelConfig& ch);
    bool ParseTemplateEntry(const std::string& prefix, const std::string& params,
                            IEC104DeviceConfig& tmpl);
    bool ParseDeviceEntry(const std::string& prefix, const std::string& params,
                          IEC104DeviceConfig& dev);
    bool ParseDIEntry(const std::vector<std::pair<std::string,std::string>>& kv,
                      IEC104DIMapping& di);
    bool ParseAIEntry(const std::vector<std::pair<std::string,std::string>>& kv,
                      IEC104AIMapping& ai);
    bool ParseDOEntry(const std::vector<std::pair<std::string,std::string>>& kv,
                      IEC104DOMapping& do_);
    bool ParseAOEntry(const std::vector<std::pair<std::string,std::string>>& kv,
                      IEC104AOMapping& ao);
    bool ParseEnergyEntry(const std::vector<std::pair<std::string,std::string>>& kv,
                          IEC104EnergyMapping& energy);

    void MergeTemplate(IEC104DeviceConfig& dev, const IEC104DeviceConfig& tmpl);

    // ── 通道线程 ──
    void ChannelThread(int chIdx);

    // ── IEC 104 协议 ──
    bool SendUFrame(socket& sock, uint32_t ctrl);
    bool SendSFrame(socket& sock);
    bool SendIFrame(socket& sock, const uint8_t* asdu, size_t asduLen);
    bool RecvFrame(socket& sock, int timeoutMs, uint8_t* buf, size_t& len);
    bool HandleUFrame(uint32_t ctrl);
    bool HandleSFrame(uint32_t ctrl);
    bool HandleIFrame(socket& sock, const uint8_t* asdu, size_t asduLen,
                      int chIdx, const IEC104DeviceConfig* dev);

    // ── ASDU 处理 ──
    void HandleTotalInterrogation(socket& sock, const uint8_t* asdu, size_t asduLen,
                                   int chIdx, const IEC104DeviceConfig* dev);
    void HandleSpontaneousDI(const uint8_t* asdu, size_t asduLen,
                              int chIdx, const IEC104DeviceConfig* dev);
    void HandleSpontaneousAI(const uint8_t* asdu, size_t asduLen,
                              int chIdx, const IEC104DeviceConfig* dev);
    void HandleGIResponseDI(const uint8_t* asdu, size_t asduLen,
                             int chIdx, const IEC104DeviceConfig* dev);
    void HandleGIResponseAI(const uint8_t* asdu, size_t asduLen,
                             int chIdx, const IEC104DeviceConfig* dev);
    void HandleGIResponseEnergy(const uint8_t* asdu, size_t asduLen,
                                  int chIdx, const IEC104DeviceConfig* dev);

    // ── 时钟同步 ──
    void SendClockSync(socket& sock);

    // ── 总召唤 ──
    void SendTotalInterrogation(socket& sock);

    // ── 辅助 ──
    uint64_t NowMs() const;
    IEC104DeviceConfig* FindDevice(int chIdx, uint16_t commonAddr);
    static uint64_t ParseCP56Time2a(const uint8_t* data);
    static std::string MonthSuffix();

    static int SafeStoi(const std::string& s, int def = 0);
    static double SafeStod(const std::string& s, double def = 0.0);

    // ── ASDU 类型解析 ──
    static int GetASDULength(uint8_t type);

    IEC104MasterConfig config_;
    std::map<std::string, IEC104DeviceConfig> templates_;
    std::atomic<bool> running_{false};
    std::vector<std::thread> threads_;
};

// ==================== AppModule 包装 ====================

class Iec104MasterModule : public AppModule {
public:
    Iec104MasterModule();
    ~Iec104MasterModule() override;
    const char* Name() const override { return "iec104_master"; }
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

#endif // IEC104_MASTER_H
