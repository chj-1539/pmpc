#ifndef IEC101_SLAVE_H
#define IEC101_SLAVE_H

//=============================================================================
// iec101_slave.h — IEC 60870-5-101 从站模块
//
// 串口监听 IEC 101 主站请求
// 支持总召唤 GI、遥控、时钟同步
// DI 变化自动上送
//=============================================================================

#include "comm_io.h"
#include "pmpc.h"
#include "iec104_master.h"  // 复用 IecType, IecCOT
#include "event_bus.h"
#include "module_manager.h"
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <atomic>
#include <thread>
#include <mutex>
#include <cstdint>

struct Slave101DIMapping { uint32_t ioa; uint16_t ch, dev, point; };
struct Slave101AIMapping { uint32_t ioa; uint8_t type = IecType::M_ME_NC_1; uint16_t ch, dev, point; double scale = 1.0, offset = 0.0; };
struct Slave101DOMapping { uint32_t ioa; int val; uint16_t ch, dev, point; };
struct Slave101DeviceConfig { uint16_t linkAddr; uint16_t coa; std::string desc;
    std::map<uint32_t, Slave101DIMapping> diMap; std::map<uint32_t, Slave101AIMapping> aiMap;
    std::map<uint32_t, std::vector<Slave101DOMapping>> doMap; };
struct Slave101Config {
    std::string portName = "COM1"; int baud = 9600; std::string parity = "even";
    int dataBits = 8; int stopBits = 1; int verbose = 1;
    std::vector<Slave101DeviceConfig> devices;
};

class Iec101Slave {
public:
    Iec101Slave(); ~Iec101Slave();
    bool LoadConfig(const std::string& path); bool Start(); void Stop();
    bool IsRunning() const { return running_; }
private:
    void PortThread();
    void HandleFrame(CommIO& io, const uint8_t* buf, size_t len);
    void SendGIRsp(CommIO& io, uint16_t linkAddr, uint16_t coa);
    void SendACK(CommIO& io, uint16_t linkAddr);
    bool FindAndExecDO(const uint8_t* asdu, size_t len);

    static uint8_t CalcCS(const uint8_t* data, size_t len);
    static int SafeStoi(const std::string& s, int def = 0);

    Slave101Config config_; std::atomic<bool> running_{false}; std::thread portThr_;
    size_t tokenDI_ = 0;
};

class Iec101SlaveModule : public AppModule {
public:
    Iec101SlaveModule(); ~Iec101SlaveModule() override;
    const char* Name() const override { return "iec101_slave"; }
    bool LoadConfig(const std::string& cfgPath) override;
    bool ValidateConfig(const std::string& cfgPath, std::vector<std::string>& errors) override;
    bool Start() override; void Stop() override; bool IsRunning() const override;
private:
    struct Impl; std::unique_ptr<Impl> impl_;
};

#endif
