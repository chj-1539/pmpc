#ifndef DLT645_MASTER_H
#define DLT645_MASTER_H

//=============================================================================
// dlt645_master.h — DLT 645 电表采集模块
//
// 层次：global → Channel → Template → Device
// 每通道独立线程，RS-485 串口通信
// 1997/2007 两种协议版本通过模板区分
// 所有数据统一存储为 AI（含电能量）
// 设备级波特率自动切换
//=============================================================================

#include "serial_port.h"
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
#include "comm_io.h"

// ─── 协议版本 ─────────────────────────────────────────────────────────────
enum class DltVersion : uint8_t {
    V1997,
    V2007,
};

// ─── AI 映射条目 ──────────────────────────────────────────────────────────
struct DltAIMapping {
    uint32_t dataId = 0;    // 数据标识（1997=2字节, 2007=4字节）
    uint16_t point = 0;     // 本地 AI 点号
    uint8_t  dataLen = 4;   // 数据域字节数（2/3/4）
    double   scale = 1.0;
    double   offset = 0.0;
};

// ─── 设备配置 ─────────────────────────────────────────────────────────────
struct DltDeviceConfig {
    uint64_t address = 0;           // 电表地址（BCD编码）
    std::string addressStr;         // 原始字符串
    std::string name;
    std::string templateName;
    int baud = 2400;                // 本设备波特率
    std::vector<DltAIMapping> aiList;
};

// ─── 通道配置 ─────────────────────────────────────────────────────────────
struct DltChannelConfig {
    std::string portName = "COM1";
    int baud = 2400;
    std::string parity = "even";
    int dataBits = 8;
    int stopBits = 1;
    int scanMs = 5000;
    int timeoutMs = 1000;
    int retryCount = 2;
    int retrySleepMs = 5000;
    int verbose = 0;

    std::vector<DltDeviceConfig> devices;
};

// ─── 全局配置 ─────────────────────────────────────────────────────────────
struct DltMasterConfig {
    int timeoutMs = 1000;
    int retryCount = 2;
    int retrySleepMs = 5000;
    int verbose = 0;
    int defaultBaud = 2400;
    std::vector<DltChannelConfig> channels;
};

/// DLT 645 主站类
class Dlt645Master {
public:
    Dlt645Master();
    ~Dlt645Master();

    bool LoadConfig(const std::string& path);
    bool Start();
    void Stop();
    bool IsRunning() const { return running_; }

private:
    // ── 配置解析 ──
    bool ParseGlobal(const std::string& key, const std::string& val);
    bool ParseChannel(const std::string& key, const std::string& val,
                      DltChannelConfig& ch);
    bool ParseTemplateEntry(const std::string& params, DltAIMapping& ai, bool is2007);
    bool ParseDeviceEntry(const std::string& key, const std::string& val,
                          DltDeviceConfig& dev);
    void MergeTemplate(DltDeviceConfig& dev, const std::vector<DltAIMapping>& tmpl);

    // ── 通道线程 ──
    void ChannelThread(int chIdx);

    // ── 设备轮询 ──
    bool PollDevice(CommIO& sp, const DltDeviceConfig& dev, DltVersion ver,
                    int timeoutMs, int chIdx, int devIdx,
                    const DltChannelConfig& ch);

    // ── DLT 645 协议 ──
    static bool BuildReadFrame(uint8_t* frame, size_t& frameLen,
                                uint64_t addr, uint32_t dataId,
                                DltVersion ver);
    static uint8_t CheckSum(const uint8_t* data, size_t len);
    static uint64_t AddrFromString(const std::string& s);
    static double   BCDToDouble(const uint8_t* data, size_t len);

    // ── 辅助 ──
    static int SafeStoi(const std::string& s, int def = 0);
    static double SafeStod(const std::string& s, double def = 0.0);

    DltMasterConfig config_;
    // 模板存储: 共 2 个预定义模板 (1997, 2007)
    std::vector<DltAIMapping> template1997_;
    std::vector<DltAIMapping> template2007_;
    std::atomic<bool> running_{false};
    std::vector<std::thread> threads_;
};

// ==================== AppModule 包装 ====================

class Dlt645MasterModule : public AppModule {
public:
    Dlt645MasterModule();
    ~Dlt645MasterModule() override;
    const char* Name() const override { return "dlt645_master"; }
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

#endif // DLT645_MASTER_H
