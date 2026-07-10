#ifndef MODBUS_RTU_SLAVE_H
#define MODBUS_RTU_SLAVE_H

//=============================================================================
// modbus_rtu_slave.h — Modbus RTU 从站模块
//
// 串口监听 Modbus RTU 请求（FC01/02/03/04/05）
// 点对点映射：每个 Modbus 地址一一映射到本地四遥点
// 地址可以不连续，可跨通道、跨设备
// 支持物理串口和 TCP 串口服务器
//=============================================================================

#include "pmpc.h"
#include "comm_io.h"
#include "modbus_tcp_master.h"  // 复用 MDataType, MEndian, DataConvert
#include "module_manager.h"
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <atomic>
#include <thread>
#include <mutex>
#include <cstdint>

// ─── 映射条目（同 modbus_tcp_slave） ──────────────────────────────────────
struct RtuSlaveFC01Entry { uint16_t addr, ch, dev, point; };
struct RtuSlaveFC02Entry { uint16_t addr, ch, dev, point; };
struct RtuSlaveFC34AIEntry { uint16_t addr, ch, dev, point; MDataType dtype = MDataType::UInt16; MEndian endian = MEndian::AB; double scale = 1.0, offset = 0.0; };
struct RtuSlaveFC34DIEntry { uint16_t addr, ch, dev, point; uint8_t bit; };
struct RtuSlaveFC05Entry { uint16_t addr, ch, dev, point; bool invert = false; };

struct RtuSlaveDeviceConfig {
    uint8_t stationId = 0;
    std::string desc;
    std::map<uint16_t, RtuSlaveFC01Entry>   fc01Map;
    std::map<uint16_t, RtuSlaveFC02Entry>   fc02Map;
    std::map<uint16_t, RtuSlaveFC34AIEntry> fc03AIMap;
    std::multimap<uint16_t, RtuSlaveFC34DIEntry> fc03DIMap;
    std::map<uint16_t, RtuSlaveFC34AIEntry> fc04AIMap;
    std::multimap<uint16_t, RtuSlaveFC34DIEntry> fc04DIMap;
    std::map<uint16_t, RtuSlaveFC05Entry>   fc05Map;
};

struct RtuSlaveConfig {
    std::string portName = "COM1";
    int baud = 9600; std::string parity = "even";
    int dataBits = 8; int stopBits = 1;
    int verbose = 1;
    std::vector<RtuSlaveDeviceConfig> devices;
};

class ModbusRtuSlave {
public:
    ModbusRtuSlave(); ~ModbusRtuSlave();
    bool LoadConfig(const std::string& path);
    bool Start(); void Stop();
    bool IsRunning() const { return running_; }
private:
    void PortThread();
    void HandleRequest(CommIO& io, const uint8_t* pdu, size_t pduLen, uint8_t stationId);
    bool HandleFC01(const RtuSlaveDeviceConfig& dev, const uint8_t* pdu, size_t pduLen, std::vector<uint8_t>& resp);
    bool HandleFC02(const RtuSlaveDeviceConfig& dev, const uint8_t* pdu, size_t pduLen, std::vector<uint8_t>& resp);
    bool HandleFC03(const RtuSlaveDeviceConfig& dev, const uint8_t* pdu, size_t pduLen, std::vector<uint8_t>& resp);
    bool HandleFC04(const RtuSlaveDeviceConfig& dev, const uint8_t* pdu, size_t pduLen, std::vector<uint8_t>& resp);
    bool HandleFC05(const RtuSlaveDeviceConfig& dev, const uint8_t* pdu, size_t pduLen, std::vector<uint8_t>& resp);
    bool HandleFCReadRegs(const uint8_t* pdu, size_t pduLen, std::vector<uint8_t>& resp, uint8_t func,
                          const std::map<uint16_t, RtuSlaveFC34AIEntry>& aiMap,
                          const std::multimap<uint16_t, RtuSlaveFC34DIEntry>& diMap);

    static uint16_t CRC16(const uint8_t* data, size_t len);
    bool SendRtuFrame(CommIO& io, uint8_t stationId, const uint8_t* pdu, size_t pduLen);

    RtuSlaveConfig config_;
    std::atomic<bool> running_{false};
    std::thread portThr_;
};

class ModbusRtuSlaveModule : public AppModule {
public:
    ModbusRtuSlaveModule(); ~ModbusRtuSlaveModule() override;
    const char* Name() const override { return "modbus_rtu_slave"; }
    bool LoadConfig(const std::string& cfgPath) override;
    bool ValidateConfig(const std::string& cfgPath, std::vector<std::string>& errors) override;
    bool Start() override; void Stop() override; bool IsRunning() const override;
private:
    struct Impl; std::unique_ptr<Impl> impl_;
};

#endif
