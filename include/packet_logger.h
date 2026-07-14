#ifndef PACKET_LOGGER_H
#define PACKET_LOGGER_H

//=============================================================================
// packet_logger.h — 报文记录器 + 协议解析器框架
//
// 功能：
//   记录各通道/设备的收发报文到文件，支持按通道/设备过滤
//   通过 ProtocolParser 插件机制自动解析报文内容
//   新增协议：继承 ProtocolParser → 注册 → 自动适配
//   文件按设备每天滚动: logs/traffic/ch{N}_dev{N}_YYYYMMDD.log
//=============================================================================

#include "socket.h"
#include "module_manager.h"
#include <string>
#include <vector>
#include <map>
#include <set>
#include <memory>
#include <atomic>
#include <thread>
#include <mutex>
#include <cstdint>
#include <fstream>

/// 报文方向
enum class PktDir : uint8_t {
    TX,     // 发出
    RX,     // 收到
};

// ==================== 解析上下文 ====================

struct ParseContext {
    PktDir      dir;        // 方向
    uint16_t    ch;         // 通道号
    uint16_t    dev;        // 设备号
    uint8_t     station;    // 从站地址
    uint8_t     func;       // 功能码
    uint16_t    transId;    // 事务 ID
    const uint8_t* data;    // PDU 数据指针
    size_t      len;        // PDU 长度

    // 从 PDU 中读大端 uint16（下标从 0 开始）
    uint16_t ReadU16(size_t offset) const {
        if (offset + 1 >= len) return 0;
        return static_cast<uint16_t>(
            (static_cast<uint16_t>(data[offset]) << 8) | data[offset + 1]);
    }
};

// ==================== 协议解析器基类 ====================

/// 所有协议解析器继承此类，实现 CanParse + Parse
/// 新增协议解析器 → RegisterParser() → 自动生效
class ProtocolParser {
public:
    virtual ~ProtocolParser() = default;

    /// 协议名称（用于标识），如 "ModbusTCP" / "ModbusRTU"
    virtual const char* ProtocolName() const = 0;

    /// 判断是否能解析此报文
    virtual bool CanParse(const ParseContext& ctx) const = 0;

    /// 解析报文，返回可读描述（空字符串 = 解析失败/未知）
    /// 如 "FC03 读保持寄存器 addr=0 qty=10"
    ///    "FC05 写线圈 addr=5 val=ON"
    ///    "写寄存器响应"
    virtual std::string Parse(const ParseContext& ctx) const = 0;
};

// ==================== Modbus TCP 解析器 ====================

/// 内建的 Modbus TCP 协议解析器，自动注册
class ModbusTcpParser : public ProtocolParser {
public:
    const char* ProtocolName() const override { return "ModbusTCP"; }
    bool CanParse(const ParseContext& ctx) const override;
    std::string Parse(const ParseContext& ctx) const override;
};

// ==================== 报文记录器（全局单例） ====================

class PacketLogger {
public:
    static PacketLogger& Instance();

    bool Init(const std::string& logDir = "logs/traffic");

    // ── 控制 ──
    void SetEnabled(bool en) { enabled_ = en; }
    bool IsEnabled() const { return enabled_; }
    /// 关闭所有已打开的文件句柄（log stop 时释放文件，避免 .log 被锁定）
    void CloseAll();

    void SetParseEnabled(bool en) { parseEnabled_ = en; }
    bool IsParseEnabled() const { return parseEnabled_; }

    void EnableDevice(uint16_t ch, uint16_t dev);
    void DisableDevice(uint16_t ch, uint16_t dev);
    void EnableAll();
    bool IsDeviceEnabled(uint16_t ch, uint16_t dev) const;

    // ── 记录（自动解析） ──
    /// 记录一条报文，内部通过已注册的 ProtocolParser 自动解析
    void Log(PktDir dir, uint16_t ch, uint16_t dev, uint8_t station,
             uint8_t func, uint16_t transId,
             const uint8_t* data, size_t len);

    // ── 解析器注册 ──
    /// 注册协议解析器（在模块初始化时调用）
    void RegisterParser(std::unique_ptr<ProtocolParser> parser);

    // ── 状态 ──
    std::string GetStatus() const;
    size_t GetFileCount() const;
    void SetMaxDays(int days) { maxDays_ = days; }

    /// 清理超过 maxDays_ 的旧日志文件
    void CleanupOldLogs();

    friend class PacketLoggerModule;
private:
    PacketLogger() = default;
    ~PacketLogger();
    PacketLogger(const PacketLogger&) = delete;
    PacketLogger& operator=(const PacketLogger&) = delete;

    std::string LogFilePath(uint16_t ch, uint16_t dev) const;
    std::string Timestamp() const;
    std::string HexDump(const uint8_t* data, size_t len) const;

    std::string logDir_ = "logs/traffic";
    std::atomic<bool> enabled_{false};
    std::atomic<bool> parseEnabled_{true};

    std::set<std::pair<uint16_t, uint16_t>> filter_;
    mutable std::mutex filterMtx_;

    struct FileEntry {
        std::ofstream stream;
        std::string dateStr;
    };
    std::map<std::string, FileEntry> files_;
    mutable std::mutex fileMtx_;

    int maxDays_ = 30;

    // 已注册的协议解析器列表
    std::vector<std::unique_ptr<ProtocolParser>> parsers_;
    mutable std::mutex parserMtx_;
};

// ==================== AppModule 包装 ====================

class PacketLoggerModule : public AppModule {
public:
    PacketLoggerModule();
    ~PacketLoggerModule() override;
    const char* Name() const override { return "packet_logger"; }
    bool LoadConfig(const std::string& cfgPath) override;
    bool ValidateConfig(const std::string& cfgPath,
                        std::vector<std::string>& errors) override;
    bool Start() override;
    void Stop() override;
    bool IsRunning() const override;
    /// 返回全局单例的 PacketLogger，与所有协议模块（modbus_tcp_master、
    /// iec104_master 等）内部调用的 PacketLogger::Instance() 保持一致。
    /// 修复：老代码返回独立成员 logger_，log start 只启用了成员实例，
    /// 但协议模块全指向单例 → 报文永不落盘。
    PacketLogger& GetLogger() { return PacketLogger::Instance(); }

    static PacketLoggerModule* GetInstance();
private:
    // logger_ 已移除 —— 所有路径统一使用 PacketLogger::Instance() 单例
    std::string cfgPath_;
    bool loaded_ = false;
    bool running_ = false;
};

#endif // PACKET_LOGGER_H
