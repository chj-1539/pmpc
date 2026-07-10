#ifndef MODBUS_RTU_MASTER_H
#define MODBUS_RTU_MASTER_H

//=============================================================================
// modbus_rtu_master.h — Modbus RTU 主站采集框架
//
// 层次：global → Port → Template → Device → 映射条目
// 串口参数可配置，每端口独立线程
// 模板继承 + 地址合并优化（同 modbus_tcp_master）
//=============================================================================

#include "serial_port.h"
#include "pmpc.h"
#include "modbus_tcp_master.h"  // 复用 MDataType, MEndian, DIMapping, AIMapping, DOMapping, AOMapping
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

// ─── 串口通道配置 ─────────────────────────────────────────────────────────

struct RtuPortConfig {
    std::string portName = "COM1";
    int baud = 9600;
    std::string parity = "even";
    int dataBits = 8;
    int stopBits = 1;
    int scanMs = 3000;
    int timeoutMs = 1000;
    int retryCount = 2;
    int retrySleepMs = 5000;
    int verbose = 0;
    // 最大读取长度（0=继承全局）
    int maxDiRead = 0;   // FC01/02 单包最大线圈数
    int maxAiRead = 0;   // FC03/04 单包最大寄存器数
    int keepAliveAddr = -1;  // 连接测试寄存器地址（-1=禁用），无 DI/AI 时自动读此地址保活

    std::vector<DeviceConfig> devices;
};

// ─── 全局配置 ─────────────────────────────────────────────────────────────

struct RtuMasterConfig {
    int timeoutMs = 1000;
    int retryCount = 2;
    int retrySleepMs = 5000;
    int verbose = 0;
    int maxDiRead = 2000;  // FC01/02 默认最多 2000 线圈
    int maxAiRead = 125;   // FC03/04 默认最多 125 寄存器

    std::vector<RtuPortConfig> ports;
};

// ─── Modbus RTU 主站 ──────────────────────────────────────────────────────

class ModbusRtuMaster {
public:
    ModbusRtuMaster();
    ~ModbusRtuMaster();

    bool LoadConfig(const std::string& path);
    bool Start();
    void Stop();
    bool IsRunning() const { return running_; }

private:
    // ── 配置解析 ──
    bool ParseGlobal(const std::string& key, const std::string& val);
    bool ParsePort(const std::string& key, const std::string& val, RtuPortConfig& portCfg);
    bool ParseTemplateEntry(const std::string& prefix, const std::string& params, DeviceConfig& tmpl);
    bool ParseDeviceEntry(const std::string& prefix, const std::string& params, DeviceConfig& dev);

    // ── 端口线程 ──
    void PortThread(int portIdx);

    // ── 设备采集 ──
    bool PollDevice(CommIO& sp, const DeviceConfig& dev, int timeoutMs,
                    int maxDiRead, int maxAiRead, int keepAliveAddr = -1);
    bool SendAndReceive(CommIO& sp, uint8_t station, const uint8_t* pdu, size_t pduLen,
                        uint8_t* resp, size_t& respLen, int timeoutMs);

    // ── 帧收发 ──
    static uint16_t CRC16(const uint8_t* data, size_t len);
    bool BuildRtuFrame(uint8_t station, const uint8_t* pdu, size_t pduLen,
                       uint8_t* frame, size_t& frameLen);
    bool ParseRtuResponse(const uint8_t* frame, size_t frameLen,
                          uint8_t& func, uint8_t* data, size_t& dataLen);

    // ── DO/AO 回写 ──
    bool WriteDOChanges(CommIO& io, const DeviceConfig& dev, int timeoutMs);
    bool WriteAOChanges(CommIO& io, const DeviceConfig& dev, int timeoutMs);

    // ── 数据分发 ──
    bool DispatchFC01or02(uint8_t devFunc, const uint8_t* respData, size_t respLen,
                          const DeviceConfig& dev, uint16_t minAddr, uint16_t qty);
    bool DispatchFC03or04(uint8_t devFunc, const uint8_t* respData, size_t respLen,
                          const DeviceConfig& dev,
                          const std::vector<size_t>& diIdxs,
                          const std::vector<size_t>& aiIdxs,
                          uint16_t minAddr, uint16_t qty);

    RtuMasterConfig config_;
    std::map<std::string, DeviceConfig> templates_;
    std::atomic<bool> running_{false};
    std::vector<std::thread> threads_;

    // 通讯状态跟踪
    std::map<std::string, bool> commStatus_;
    mutable std::mutex commMtx_;

    // ── DO/AO 上次已发送值跟踪（独立于 CheckAllPointChange 的 lastMaster 同步） ──
    std::map<std::string, bool> doSent_;
    std::map<std::string, double> aoSent_;
    mutable std::mutex sentMtx_;

    // ── DO/AO 脉冲复位队列 ──
    struct PulseEntry {
        uint8_t  channel;
        uint8_t  stationId;
        uint8_t  func;
        uint16_t addr;
        uint8_t  valHi;
        uint8_t  valLo;
        uint64_t readyAtMs;
    };
    std::vector<PulseEntry> pulseQueue_;
    mutable std::mutex pulseMtx_;

    // ── 脉冲处理 ──
    void ProcessPulseQueue(CommIO& io, int timeoutMs);
};

// ==================== AppModule 包装 ====================

class ModbusRtuMasterModule : public AppModule {
public:
    ModbusRtuMasterModule();
    ~ModbusRtuMasterModule() override;
    const char* Name() const override { return "modbus_rtu_master"; }
    bool LoadConfig(const std::string& cfgPath) override;
    bool ValidateConfig(const std::string& cfgPath, std::vector<std::string>& errors) override;
    bool Start() override;
    void Stop() override;
    bool IsRunning() const override;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

#endif // MODBUS_RTU_MASTER_H
