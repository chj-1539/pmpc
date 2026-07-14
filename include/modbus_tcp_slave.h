#ifndef MODBUS_TCP_SLAVE_H
#define MODBUS_TCP_SLAVE_H

//=============================================================================
// modbus_tcp_slave.h — Modbus TCP 从站模块
//
// 功能：
//   监听 TCP 端口，接收 Modbus TCP 请求
//   支持功能码：01 读线圈 / 02 读离散输入 / 03 读保持寄存器
//               04 读输入寄存器 / 05 写单个线圈
//   点对点映射：每个 Modbus 地址一一映射到本地四遥点
//   地址可以不连续，可跨通道、跨设备
//=============================================================================

#include "socket.h"
#include "pmpc.h"
#include "modbus_tcp_master.h"  // 复用 MDataType, MEndian, DataConvert
#include "module_manager.h"     // AppModule
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <atomic>
#include <thread>
#include <mutex>
#include <cstdint>
#include <set>

// ==================== 点对点映射条目 ====================

/// DI 映射条目（FC01/FC02 共用，通过 func 区分独立的 Modbus 地址空间）
struct SlaveDIEntry {
    uint16_t addr = 0;      // Modbus 地址
    uint8_t  func = 1;      // 1=FC01(读线圈), 2=FC02(读离散输入)
    uint16_t ch   = 0;      // 本地通道号
    uint16_t dev  = 0;      // 本地设备号
    uint16_t point = 0;     // 本地遥信点号
};

/// FC03/FC04 AI: 读保持/输入寄存器 → 本地遥测 AI
struct SlaveAIEntry {
    uint16_t addr   = 0;
    uint16_t ch     = 0;
    uint16_t dev    = 0;
    uint16_t point  = 0;
    MDataType dtype = MDataType::UInt16;
    MEndian  endian = MEndian::AB;
    double   scale  = 1.0;
    double   offset = 0.0;
};

/// FC03/FC04 按位 DI: 读寄存器某一位 → 本地遥信 DI
struct SlaveBitDIEntry {
    uint16_t addr  = 0;
    uint8_t  bit   = 0;     // 位号 0~15
    uint16_t ch    = 0;
    uint16_t dev   = 0;
    uint16_t point = 0;
};

/// FC05: 写单个线圈 → 本地遥控 DO
struct SlaveDOEntry {
    uint16_t addr   = 0;
    uint16_t ch     = 0;
    uint16_t dev    = 0;
    uint16_t point  = 0;
    bool     invert = false;
};

/// 复合键生成宏：将 (func<<16)|addr 作为 map key，隔离 FC01/FC02 地址空间
#define DI_KEY(func, addr)  ((static_cast<uint32_t>(func) << 16) | (addr))

// ==================== 从站设备配置 ====================

struct SlaveDeviceConfig {
    uint8_t  stationId = 0;
    std::string desc;

    // ═══ 注意 ═══
    // 仅保留 4 个 std::map 成员，避免 GCC 8.1 MinGW 未初始化 _M_node_count 的编译器 bug。
    // 之前版本有 7 个 map，最后 3 个的构造函数在作为 struct 深层成员时被跳过。
    // ═══════════

    /// FC01+FC02 遥信映射：key=DI_KEY(func, addr)
    std::map<uint32_t, SlaveDIEntry>                  diMap;
    /// FC03+FC04 遥测 AI 映射：addr → entry
    std::map<uint16_t, SlaveAIEntry>                  aiMap;
    /// FC03+FC04 按位遥信 DI 映射：addr → vector<bit entries>
    std::map<uint16_t, std::vector<SlaveBitDIEntry>>  diBitMap;
    /// FC05 遥控 DO 映射：addr → entry
    std::map<uint16_t, SlaveDOEntry>                  doMap;
};

// ==================== 从站全局配置 ====================

struct SlaveConfig {
    int port = 502;
    int maxClients = 10;
    int verbose = 1;
    std::vector<SlaveDeviceConfig> devices;
};

/// 监听绑定（IP 访问控制）
struct SlaveBind {
    std::string allowedIP;  // "" = 允许所有
    uint16_t port;
};

// ==================== Modbus TCP 从站 ====================

class ModbusTcpSlave {
public:
    ModbusTcpSlave();
    ~ModbusTcpSlave();

    bool LoadConfig(const std::string& path);
    bool Start();
    void Stop();
    bool IsRunning() const { return running_; }

private:
    // ── TCP 服务 ──
    void AcceptLoop(const SlaveBind& bind, socket& listenSock);
    void ClientThread(socket clientSock, std::shared_ptr<std::atomic<bool>> done);

    // ── 功能码处理 ──
    bool HandleFC01(const SlaveDeviceConfig& dev, const uint8_t* pdu,
                    size_t pduLen, std::vector<uint8_t>& resp);
    bool HandleFC02(const SlaveDeviceConfig& dev, const uint8_t* pdu,
                    size_t pduLen, std::vector<uint8_t>& resp);
    bool HandleFC03(const SlaveDeviceConfig& dev, const uint8_t* pdu,
                    size_t pduLen, std::vector<uint8_t>& resp);
    bool HandleFC04(const SlaveDeviceConfig& dev, const uint8_t* pdu,
                    size_t pduLen, std::vector<uint8_t>& resp);
    bool HandleFC05(const SlaveDeviceConfig& dev, const uint8_t* pdu,
                    size_t pduLen, std::vector<uint8_t>& resp);

    // FC03/04 共用读寄存器实现
    bool HandleFCReadRegs(const SlaveDeviceConfig& dev,
                           const uint8_t* pdu, size_t pduLen,
                           std::vector<uint8_t>& resp, uint8_t func,
                           const std::map<uint16_t, SlaveAIEntry>& aiMap,
                           const std::map<uint16_t, std::vector<SlaveBitDIEntry>>& diBitMap);

    // ── 帧收发 ──
    bool SendFrame(socket& sock, uint16_t transId, uint8_t stationId,
                   const uint8_t* pdu, size_t pduLen);
    bool SendException(socket& sock, uint16_t transId, uint8_t stationId,
                       uint8_t func, uint8_t errCode);

    // ── 数据转换工具 ──
public:   // H14（第二轮）: modbus_rtu_slave 也复用这些纯函数，需 public
    static uint64_t DoubleToRawValue(double value, MDataType dtype);
    static void RawToWireBytes(uint64_t rawVal, int byteCount, MEndian endian,
                                uint8_t* out);

    // ── 配置解析 ──
    static MDataType ParseDataType(const std::string& s);
    static MEndian  ParseEndian(const std::string& s);
    bool ParseDIEntry(const std::string& params, SlaveDIEntry& entry);
    bool ParseAIEntry(const std::string& params, SlaveAIEntry& entry);
    bool ParseBitDIEntry(const std::string& params, SlaveBitDIEntry& entry);
    bool ParseDOEntry(const std::string& params, SlaveDOEntry& entry);

    SlaveConfig config_;
    std::vector<SlaveBind> binds_;
    std::atomic<bool> running_{false};
    std::vector<socket> listenSocks_;
    std::vector<std::thread> acceptThreads_;
    std::vector<std::thread> clientThreads_;
    std::vector<std::shared_ptr<std::atomic<bool>>> clientDoneFlags_;
    std::mutex clientMtx_;
    int cleanupCnt_ = 0;
};

// ==================== AppModule 包装 ====================

class ModbusTcpSlaveModule : public AppModule {
public:
    ModbusTcpSlaveModule();
    ~ModbusTcpSlaveModule() override;
    const char* Name() const override { return "modbus_tcp_slave"; }
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

#endif // MODBUS_TCP_SLAVE_H
