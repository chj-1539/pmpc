#ifndef CDT_MASTER_H
#define CDT_MASTER_H

//=============================================================================
// cdt_master.h — CDT 主站（被动接收）模块
//
// 接收 RTU 循环上送的 CDT 规约帧
// YC(addr) → AI  YX(addr,bit) → DI
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

struct CdtMasterYC { uint16_t addr; uint16_t point; double scale; double offset; };
struct CdtMasterYX { uint16_t addr; uint8_t bit; uint16_t point; };
struct CdtMasterYM { uint16_t addr; uint16_t point; double scale; double offset; };
struct CdtMasterDevice { uint16_t station; std::vector<CdtMasterYC> ycList; std::vector<CdtMasterYX> yxList; std::vector<CdtMasterYM> ymList; };
struct CdtMasterChannel {
    std::string portName = "COM1"; int baud = 1200; std::string parity = "even";
    int dataBits = 8; int stopBits = 1; int timeoutMs = 1000; int verbose = 0;
    std::vector<CdtMasterDevice> devices;
};
struct CdtMasterConfig { int timeoutMs = 1000; int verbose = 0; int frameTimeoutMs = 5000; std::vector<CdtMasterChannel> channels; };

class CdtMaster {
public:
    CdtMaster(); ~CdtMaster();
    bool LoadConfig(const std::string& path); bool Start(); void Stop(); bool IsRunning() const { return running_; }
private:
    void PortThread(int chIdx);
    void ParseFrame(const uint8_t* buf, size_t len, uint16_t station);
    static int SafeStoi(const std::string& s, int def = 0);
    static double SafeStod(const std::string& s, double def = 0.0);
    CdtMasterConfig config_; std::atomic<bool> running_{false}; std::vector<std::thread> threads_;
};

class CdtMasterModule : public AppModule {
public:
    CdtMasterModule(); ~CdtMasterModule() override;
    const char* Name() const override { return "cdt_master"; }
    bool LoadConfig(const std::string& cfgPath) override;
    bool ValidateConfig(const std::string& cfgPath, std::vector<std::string>& errors) override;
    bool Start() override; void Stop() override; bool IsRunning() const override;
private:
    struct Impl; std::unique_ptr<Impl> impl_;
};

#endif
