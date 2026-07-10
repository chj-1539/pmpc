#ifndef CDT_SLAVE_H
#define CDT_SLAVE_H

//=============================================================================
// cdt_slave.h — CDT 从站（RTU，主动发送）模块
//
// 主动循环上送 CDT 规约帧
// AI → YC(addr)  DI → YX(addr,bit)
//=============================================================================

#include "serial_port.h"
#include "pmpc.h"
#include "module_manager.h"
#include <string>
#include <vector>
#include <memory>
#include <atomic>
#include <thread>
#include <mutex>
#include <cstdint>
#include "comm_io.h"

struct CdtSlaveYC { uint16_t addr; uint16_t ch; uint16_t dev; uint16_t point; double scale; double offset; };
struct CdtSlaveYX { uint16_t addr; uint8_t bit; uint16_t ch; uint16_t dev; uint16_t point; };
struct CdtSlaveYM { uint16_t addr; uint16_t ch; uint16_t dev; uint16_t point; double scale; double offset; };
struct CdtSlaveDevice { uint16_t station; std::vector<CdtSlaveYC> ycList; std::vector<CdtSlaveYX> yxList; std::vector<CdtSlaveYM> ymList; };
struct CdtSlaveConfig {
    std::string portName = "COM1"; int baud = 1200; std::string parity = "even";
    int dataBits = 8; int stopBits = 1; int cycleMs = 1000; int verbose = 0;
    std::vector<CdtSlaveDevice> devices;
};

class CdtSlave {
public:
    CdtSlave(); ~CdtSlave();
    bool LoadConfig(const std::string& path); bool Start(); void Stop(); bool IsRunning() const { return running_; }
private:
    void PortThread();
    void BuildAndSend(CommIO& sp);
    static int SafeStoi(const std::string& s, int def = 0);
    static double SafeStod(const std::string& s, double def = 0.0);
    CdtSlaveConfig config_; std::atomic<bool> running_{false}; std::thread thread_;
};

class CdtSlaveModule : public AppModule {
public:
    CdtSlaveModule(); ~CdtSlaveModule() override;
    const char* Name() const override { return "cdt_slave"; }
    bool LoadConfig(const std::string& cfgPath) override;
    bool ValidateConfig(const std::string& cfgPath, std::vector<std::string>& errors) override;
    bool Start() override; void Stop() override; bool IsRunning() const override;
private:
    struct Impl; std::unique_ptr<Impl> impl_;
};

#endif
