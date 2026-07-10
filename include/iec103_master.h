#ifndef IEC103_MASTER_H
#define IEC103_MASTER_H

//=============================================================================
// iec103_master.h — IEC 60870-5-103 主站采集模块
//
// 层次：global → Channel → Template → Device
// 每通道独立线程，RS-485 串口 / TCP 串口服务器
// 数据通过 FUN(功能码) + INF(信息号) 标识
// 遥脉统一按 AI 存储
//=============================================================================

#include "comm_io.h"
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

// ─── IEC 103 常量 ─────────────────────────────────────────────────────────
constexpr uint8_t FRAME_START_103 = 0x68;

// ─── ASDU 类型 ────────────────────────────────────────────────────────────
namespace Iec103Type {
    constexpr uint8_t C_IC_NA_1  = 0x64;  // 总召唤
    constexpr uint8_t C_CI_NA_1  = 0x65;  // 查询命令
}

// ─── DI 映射 ──────────────────────────────────────────────────────────────
struct Iec103DIMapping { uint8_t fun, inf; uint16_t point; };
struct Iec103AIMapping { uint8_t fun, inf; uint16_t point; uint8_t type = 0; double scale = 1.0, offset = 0.0; }; // type: 0=normalized, 1=float
struct Iec103DOMapping { uint8_t fun, inf; uint16_t point; };
struct Iec103DeviceConfig {
    uint16_t station = 0;
    std::string name, templateName;
    std::vector<Iec103DIMapping> diList;
    std::vector<Iec103AIMapping> aiList;
    std::vector<Iec103DOMapping> doList;
};
struct Iec103ChannelConfig {
    std::string portName = "COM1"; int baud = 9600; std::string parity = "even";
    int dataBits = 8; int stopBits = 1; int scanMs = 3000; int timeoutMs = 1000;
    int retryCount = 2; int retrySleepMs = 5000; int verbose = 0;
    std::vector<Iec103DeviceConfig> devices;
};
struct Iec103MasterConfig { int timeoutMs = 1000; int verbose = 0; std::vector<Iec103ChannelConfig> channels; };

class Iec103Master {
public:
    Iec103Master(); ~Iec103Master();
    bool LoadConfig(const std::string& path); bool Start(); void Stop();
    bool IsRunning() const { return running_; }
private:
    void ChannelThread(int chIdx);
    bool PollDevice(CommIO& io, const Iec103DeviceConfig& dev, const Iec103ChannelConfig& ch, int chIdx, int devIdx);
    bool SendAndRecv(CommIO& io, const uint8_t* req, size_t reqLen, uint8_t* resp, size_t& respLen, int timeoutMs);
    static int SafeStoi(const std::string& s, int def = 0);
    static double SafeStod(const std::string& s, double def = 0.0);
    Iec103MasterConfig config_;
    std::map<std::string, Iec103DeviceConfig> templates_;
    std::atomic<bool> running_{false}; std::vector<std::thread> threads_;
};

class Iec103MasterModule : public AppModule {
public:
    Iec103MasterModule(); ~Iec103MasterModule() override;
    const char* Name() const override { return "iec103_master"; }
    bool LoadConfig(const std::string& cfgPath) override;
    bool ValidateConfig(const std::string& cfgPath, std::vector<std::string>& errors) override;
    bool Start() override; void Stop() override; bool IsRunning() const override;
private:
    struct Impl; std::unique_ptr<Impl> impl_;
};

#endif
