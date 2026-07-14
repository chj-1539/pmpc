#ifndef IEC101_MASTER_H
#define IEC101_MASTER_H

//=============================================================================
// iec101_master.h — IEC 60870-5-101 主站采集模块
//
// 层次：global → Channel → Template → Device
// 每通道独立线程，RS-485 串口 / TCP 串口服务器
// ASDU 层与 IEC 104 相同（复用 IecType / IecCOT）
// 链路层：可变帧 68 LEN LEN 68 CTRL ADDR ASDU CS 16
//          固定帧 10 CMD ADDR CS 16
//=============================================================================

#include "comm_io.h"
#include "pmpc.h"
#include "iec104_master.h"  // 复用 IecType, IecCOT, IEC104AIMapping 等
#include "module_manager.h"
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <atomic>
#include <thread>
#include <mutex>
#include <cstdint>

// ─── 链路层常量 ───────────────────────────────────────────────────────────
constexpr uint8_t IEC101_START_VAR = 0x68;  // 可变帧长
constexpr uint8_t IEC101_START_FIX = 0x10;  // 固定帧长
constexpr uint8_t IEC101_END       = 0x16;

// ─── 链路层命令 ───────────────────────────────────────────────────────────
namespace Iec101Cmd {
    constexpr uint8_t REQ_1D = 0x5B;  // 请求1级数据
    constexpr uint8_t REQ_2D = 0x7B;  // 请求2级数据
    constexpr uint8_t RESP_1D = 0x0B; // 1级响应
    constexpr uint8_t RESP_2D = 0x0B; // 2级响应
    constexpr uint8_t C_RES = 0x00;   // 复位
    constexpr uint8_t C_IC  = 0x64;   // 总召唤（ASDU 层）
}

// ─── 101 映射条目 ─────────────────────────────────────────────────────────
struct Iec101AIMapping { uint32_t ioa = 0; uint8_t type = IecType::M_ME_NC_1; uint16_t point = 0; double scale = 1.0, offset = 0.0; };
struct Iec101DIMapping { uint32_t ioa = 0; uint8_t type = IecType::M_SP_NA_1; uint16_t point = 0; };
struct Iec101DOMapping { uint32_t ioa = 0; int val = 0; uint8_t type = IecType::C_SC_NA_1; uint16_t point = 0; };
struct Iec101EnergyMapping { uint32_t ioa = 0; uint16_t point = 0; double scale = 1.0, offset = 0.0; };

struct Iec101DeviceConfig {
    uint16_t linkAddr = 0;  // 链路地址
    uint16_t coa = 0;       // 公共地址
    std::string name;
    std::vector<Iec101DIMapping> diList;
    std::vector<Iec101AIMapping> aiList;
    std::vector<Iec101EnergyMapping> energyList;
};

struct Iec101ChannelConfig {
    std::string portName = "COM1"; int baud = 9600; std::string parity = "even";
    int dataBits = 8; int stopBits = 1; int timeoutMs = 1000; int giCycleS = 300;
    int retryCount = 2; int retrySleepMs = 5000; int verbose = 0;
    std::vector<Iec101DeviceConfig> devices;
};

struct Iec101MasterConfig { int timeoutMs = 1000; int verbose = 0; std::vector<Iec101ChannelConfig> channels; };

class Iec101Master {
public:
    Iec101Master(); ~Iec101Master();
    bool LoadConfig(const std::string& path); bool Start(); void Stop();
    bool IsRunning() const { return running_; }

    // 测试挂钩：允许 tests/test_iec101_master_multichannel.cxx 触及
    // config_ 与 HandleGIResponse，验证 M11 修复。
    friend class Iec101MasterTestAccess;

private:
    void ChannelThread(int chIdx);
    void PollDevice(CommIO& io, const Iec101DeviceConfig& dev, int chIdx, int devIdx, int timeoutMs);
    bool SendRecvVarFrame(CommIO& io, const uint8_t* asdu, size_t asduLen, uint16_t linkAddr,
                          uint8_t* respAsdu, size_t& respLen, int timeoutMs);
    bool SendRecvFixedFrame(CommIO& io, uint8_t cmd, uint16_t linkAddr, int timeoutMs);
    void HandleGIResponse(const uint8_t* asdu, size_t len, uint16_t coa, int chIdx);
    void HandleSpontaneous(const uint8_t* asdu, size_t len, uint16_t coa);

    static uint8_t CalcCS(const uint8_t* data, size_t len);
    static int SafeStoi(const std::string& s, int def = 0);
    static double SafeStod(const std::string& s, double def = 0.0);

    Iec101MasterConfig config_;
    std::atomic<bool> running_{false};
    std::vector<std::thread> threads_;
    // M18（第二轮）修复：FCB（帧计数位）应每次发送后翻转，平衡传输规约要求。
    // 当前只有单通道/单串口场景，此处用普通成员即可（多通道串行访问时最差
    // 情况是丢一次翻转，不会数据损坏）。
    uint8_t fcbToggle_ = 0;
};

class Iec101MasterModule : public AppModule {
public:
    Iec101MasterModule(); ~Iec101MasterModule() override;
    const char* Name() const override { return "iec101_master"; }
    bool LoadConfig(const std::string& cfgPath) override;
    bool ValidateConfig(const std::string& cfgPath, std::vector<std::string>& errors) override;
    bool Start() override; void Stop() override; bool IsRunning() const override;
private:
    struct Impl; std::unique_ptr<Impl> impl_;
};

#endif
