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

    // CDT 帧同步头状态机（对外暴露供单元测试）。
    // 输入：当前累计 pos（已接受的同步头字节数，0..5 为同步头范围，6+ 为帧体）、
    //       新到达的一字节 byte。
    // 返回：处理该字节后的新 pos。约定：若返回值 > 输入 pos，调用方应把该字节
    //       写入 buf[输入pos]（即 buf 的"下一空位"）；否则调用方不写入。
    // inline 定义（见类外）避免测试文件链入 cdt_master.cxx 的一整棵传递依赖图。
    static size_t AdvanceSyncHeader(size_t pos, uint8_t byte);

private:
    void PortThread(int chIdx);
    void ParseFrame(const uint8_t* buf, size_t len, uint16_t station);
    static int SafeStoi(const std::string& s, int def = 0);
    static double SafeStod(const std::string& s, double def = 0.0);
    CdtMasterConfig config_; std::atomic<bool> running_{false}; std::vector<std::thread> threads_;
};

// static 成员函数的 inline 定义。
inline size_t CdtMaster::AdvanceSyncHeader(size_t pos, uint8_t byte) {
    // SYNC_BYTE = 0xEB, SYNC_BYTE2 = 0x90（CDT 规约同步头由 3 组 EB 90 组成）
    constexpr uint8_t SYNC_BYTE  = 0xEB;
    constexpr uint8_t SYNC_BYTE2 = 0x90;
    // pos 位应为 EB 的位置：0, 2, 4
    // pos 位应为 90 的位置：1, 3, 5
    // pos >= 6 表示同步头已完整，直接接收字节体。
    if (pos >= 6) return pos + 1;
    const bool expectEB = ((pos & 1u) == 0);
    const uint8_t expected = expectEB ? SYNC_BYTE : SYNC_BYTE2;
    if (byte == expected) return pos + 1;
    // 失配。若失配位是 90 位、字节恰好是 EB，则可作为新同步头起点，保持
    // pos=1；否则整体重置到 0。与原有 6 行 if 的语义一致。
    if (!expectEB && byte == SYNC_BYTE) return 1;
    return 0;
}

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
