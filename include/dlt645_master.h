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

    // ── DLT-2（第二轮）：CS 范围公开助手（供测试） ──────────────────────────
    // 规约要求 CS = 从第一个 0x68（起始符）开始，到 CS 前一字节，所有字节
    // 按字节累加 mod 256。老代码写作 `CheckSum(frame + 1, pos - 1)` —— 起点
    // 少算 1 字节（漏掉了起始符 0x68）。inline static 便于测试零依赖调用。
    static inline uint8_t CalcCS(const uint8_t* data, size_t len) {
        uint8_t cs = 0;
        for (size_t i = 0; i < len; i++) cs = static_cast<uint8_t>(cs + data[i]);
        return cs;
    }

    // ── DLT-1（第二轮）：±0x33 数据加/解码 ─────────────────────────────────
    // 规约 §5.1：帧结构 68 A0..A5 68 C L DATA CS 16 —— DATA 字段（DI + Value）
    // 发送前每字节 +0x33，接收后每字节 -0x33。之前的实现完全未做该变换 →
    // 任何真实电表都会拒绝或返回被误解的 BCD。
    // 就地修改；DATA 长度 = L 字段。
    static inline void EncodeData(uint8_t* data, size_t len) {
        for (size_t i = 0; i < len; i++)
            data[i] = static_cast<uint8_t>(data[i] + 0x33);
    }
    static inline void DecodeData(uint8_t* data, size_t len) {
        for (size_t i = 0; i < len; i++)
            data[i] = static_cast<uint8_t>(data[i] - 0x33);
    }

    // ── DLT-3（第二轮）：应答地址回比公开助手（供测试） ──────────────────────
    // resp[1..6] 应该等于请求发出的 addr（低字节在前）。若不一致，多半是
    // 同一 485 总线上的别的表迟延应答。
    static inline bool AddrEquals(const uint8_t* respAddr6, uint64_t addr) {
        for (size_t i = 0; i < 6; i++) {
            uint8_t byte = static_cast<uint8_t>((addr >> (i * 8)) & 0xFF);
            if (respAddr6[i] != byte) return false;
        }
        return true;
    }

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
