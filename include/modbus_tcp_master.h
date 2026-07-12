#ifndef MODBUS_TCP_MASTER_H
#define MODBUS_TCP_MASTER_H

//=============================================================================
// modbus_tcp_master.h — Modbus TCP 主站采集框架
//
// 层次：global → Channel → Template → Device → 映射条目
// 模板继承：设备引用模板，自动合并模板条目 + 设备自定义条目
// 组批优化：同func+连续地址自动合并为一个 Modbus 请求
//=============================================================================

#include "socket.h"
#include "pmpc.h"
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <atomic>
#include <thread>
#include <cstdint>
#include <functional>
#include <mutex>

// ─── 枚举 ──────────────────────────────────────────────────────────────────

/// 数据类型
enum class MDataType : uint8_t {
    Unknown,
    Int16, UInt16,
    Int32, UInt32,
    Int64, UInt64,
    Float,
    Double,
};

/// 字节顺序
enum class MEndian : uint8_t {
    Unknown,
    // 2字节
    AB,      // [0][1] 大端(Modbus默认)
    BA,      // [1][0] 小端
    // 4字节 (int32/uint32/float)
    ABCD,    // [0][1][2][3] 大端
    CDAB,    // [2][3][0][1] 低字在前(Modicon默认)
    BADC,    // [1][0][3][2] 字内字节交换
    DCBA,    // [3][2][1][0] 全小端
    // 8字节 (int64/uint64/double)
    ABCDEFGH,   // 大端
    GHEFCDAB,   // 交换32位字
    HGFEDCBA,   // 全小端
};

/// 通讯中断行为
enum class MCommLoss : uint8_t {
    Keep,       // 保持上次值
    Zero,       // 归零
    SetMax,     // 置最大值
    SetMin,     // 置最小值
    SetCustom,  // 自定义值
};

/// Modbus 功能码（仅使用本模块涉及的）
namespace MFunc {
    constexpr uint8_t READ_COILS           = 0x01;
    constexpr uint8_t READ_DISCRETE_INPUTS = 0x02;
    constexpr uint8_t READ_HOLDING_REGS    = 0x03;
    constexpr uint8_t READ_INPUT_REGS      = 0x04;
    constexpr uint8_t WRITE_SINGLE_COIL    = 0x05;
    constexpr uint8_t WRITE_SINGLE_REG     = 0x06;
    constexpr uint8_t WRITE_MULTI_COILS    = 0x0F;
    constexpr uint8_t WRITE_MULTI_REGS     = 0x10;
}

// ─── 映射条目 ──────────────────────────────────────────────────────────────

/// 遥信 (DI) 映射条目
struct DIMapping {
    uint16_t addr = 0;          // Modbus 地址
    uint8_t  func = 1;          // 功能码 01/02/03/04
    uint16_t qty = 0;           // FC01/02: 线圈数量(0=不适用)
    int      bit = -1;          // FC03/04: 位序号(-1=不适用)
    uint16_t point = 1;         // 本地 DI 起始点号
    bool     invert = false;    // 是否取反
    MCommLoss commLoss = MCommLoss::Keep;

    // 运行时辅助
    int regCount() const {
        if (func == 1 || func == 2) return 0; // 位操作，非寄存器
        return 1; // 1个寄存器含16位
    }
};

/// 遥测 (AI) 映射条目
struct AIMapping {
    uint16_t addr = 0;          // Modbus 起始地址
    uint8_t  func = 3;          // 功能码 03/04
    uint16_t point = 1;         // 本地 AI 点号
    MDataType dtype = MDataType::UInt16;
    MEndian  endian = MEndian::AB;
    double   scale = 1.0;
    double   offset = 0.0;
    MCommLoss commLoss = MCommLoss::Keep;
    double   commLossVal = 0.0;

    /// 该条目占用的寄存器数量
    int regCount() const;
};

/// 遥控 (DO) 映射条目
struct DOMapping {
    uint16_t addr = 0;          // Modbus 地址
    uint8_t  func = 5;          // 功能码 05/15
    uint16_t point = 1;         // 本地 DO 点号
    bool     invert = false;
    int      pulseMs = 0;       // 脉冲宽度(ms)，0=不发送复位报文
};

/// 遥调 (AO) 映射条目
struct AOMapping {
    uint16_t addr = 0;          // Modbus 起始地址
    uint8_t  func = 6;          // 功能码 06/16
    uint16_t point = 1;         // 本地 AO 点号
    MDataType dtype = MDataType::UInt16;
    MEndian  endian = MEndian::AB;
    double   scale = 1.0;
    double   offset = 0.0;
    int      pulseMs = 0;       // 脉冲宽度(ms)，0=不发送复位报文
};

// ─── 设备配置（由模板+设备自身合并而成） ─────────────────────────────────

struct DeviceConfig {
    uint8_t  channel = 0;
    uint8_t  stationId = 0;
    std::string name;
    std::string templateName;

    std::vector<DIMapping> diList;
    std::vector<AIMapping> aiList;
    std::vector<DOMapping> doList;
    std::vector<AOMapping> aoList;
};

// ─── 通道配置 ──────────────────────────────────────────────────────────────

struct ChannelConfig {
    std::string ip;
    uint16_t port = 502;
    int scanMs = 3000;
    int timeoutMs = 3000;
    int retryCount = 3;
    int retrySleepMs = 10000;
    int verbose = 0;
    int hexDump = 0;
    // 双连接冗余
    std::string standbyIp;
    int standbyPort = 0;
    int fallback = 1;
    // 最大读取长度（0=继承全局）
    int maxDiRead = 0;   // FC01/02 单包最大线圈数
    int maxAiRead = 0;   // FC03/04 单包最大寄存器数
    int keepAliveAddr = -1;  // 连接测试寄存器地址（-1=禁用），无 DI/AI 时自动读此地址保活

    std::vector<DeviceConfig> devices;
};

// ─── 全局配置 ──────────────────────────────────────────────────────────────

struct MasterConfig {
    int timeoutMs = 3000;
    int retryCount = 3;
    int retrySleepMs = 10000;
    int verbose = 0;
    int hexDump = 0;
    MCommLoss commLoss = MCommLoss::Keep;
    int maxDiRead = 2000;  // FC01/02 默认最多 2000 线圈
    int maxAiRead = 125;   // FC03/04 默认最多 125 寄存器

    std::vector<ChannelConfig> channels;
};

// ─── 数据转换工具 ──────────────────────────────────────────────────────────

class DataConvert {
public:
    /// 从字节数组解析值（根据数据类型和字节顺序）
    static double ParseValue(const uint8_t* data, size_t offset,
                             MDataType dtype, MEndian endian);

    /// 应用比例/偏移
    static double ApplyScale(double raw, double scale, double offset);

    /// 通讯中断时取默认值
    static double GetCommLossValue(MCommLoss loss, double customVal);
    static bool   GetCommLossBool(MCommLoss loss, bool customVal);

    /// 获取某类型占用的寄存器数
    static int GetRegCount(MDataType dtype);
};

// ─── 字节顺序重排工具 ──────────────────────────────────────────────────────

class EndianConvert {
public:
    /// 将 Modbus 大端寄存器数据按指定顺序重排为 uint64
    static uint64_t Rearrange(const uint8_t* regs, int regCount, MEndian endian);
};

// ─── 日志工具 ──────────────────────────────────────────────────────────────

class MLogger {
public:
    enum Level { ERROR, WARN, INFO, DEBUG };

    static void Print(Level lv, const char* tag, const std::string& msg);
    static void Printf(Level lv, const char* tag, const char* fmt, ...);
    static void HexDump(const char* tag, const uint8_t* data, size_t len);

    static std::atomic<int> verbosity;
    static int hexDump;
};

// ─── Modbus TCP 主站 ───────────────────────────────────────────────────────

class ModbusTcpMaster {
public:
    ModbusTcpMaster();
    ~ModbusTcpMaster();

    /// 加载配置文件，解析模板+设备
    bool LoadConfig(const std::string& path);

    /// 启动所有通道（每通道独立线程）
    bool Start();

    /// 停止所有通道
    void Stop();

    bool IsRunning() const { return running_; }

    /// 获取当前加载的配置（只读）
    const MasterConfig& GetConfig() const { return config_; }

    // 测试挂钩：供 tests/test_modbus_master_write_race.cxx 访问私有
    // WriteDOChanges/WriteAOChanges 与 doSent_/aoSent_，验证 bug #5 竞态修复。
    // 生产代码不使用。见 CLAUDE.md「已知陷阱 / 修复历史」bug #5。
    friend class ModbusTcpMasterTestAccess;

    // AO "值几乎相等" 判定 —— H9 修复用（Modbus AO 写回节流）。
    // 老代码用绝对容差 std::abs(a - b) < 0.001，对量程差异大的场景不友好：
    //   * 1e9 附近，浮点精度就 >0.001，两次采相同值也判为"变化"，一直重发
    //   * 1e-6 附近，两个明显不同的值仍会被判为"相同"，漏发
    // 用相对+绝对混合：|a-b| <= max(absTol, relTol * max(|a|,|b|))
    // 见 tests/test_modbus_ao_epsilon.cxx。默认参数与生产量程匹配。
    static inline bool AoAlmostEqual(double a, double b,
                                     double absTol = 1e-6,
                                     double relTol = 1e-4) {
        double diff = a > b ? a - b : b - a;
        double aa = a >= 0 ? a : -a;
        double bb = b >= 0 ? b : -b;
        double scale = aa > bb ? aa : bb;
        double bound = relTol * scale;
        if (bound < absTol) bound = absTol;
        return diff <= bound;
    }

    // 从一组 (addr, qty) DI/AI 映射条目算出可以覆盖它们全部的连续 Modbus
    // 读请求范围 [minAddr, minAddr + qty - 1]。H6 相关：老代码在这里保留了
    // 一段"if (qty < maxQty) qty = maxQty" 的死代码 —— 数学上 qty = maxAddr
    // - minAddr + 1 已经 ≥ 任何单个条目的 qty，不需要 fallback。
    // 返回 (minAddr, qty)；输入空则返回 (0, 0)。
    // 见 tests/test_modbus_master_addr_span.cxx。
    struct AddrSpan { uint16_t minAddr = 0; uint16_t qty = 0; };
    template <typename ItemIter>
    static AddrSpan ComputeAddrSpan(ItemIter begin, ItemIter end) {
        if (begin == end) return {};
        uint16_t lo = 0xFFFF;
        uint16_t hi = 0;
        for (auto it = begin; it != end; ++it) {
            uint16_t addr = it->addr;
            uint16_t q    = it->qty;
            if (addr < lo) lo = addr;
            uint16_t last = static_cast<uint16_t>(addr + q - 1);
            if (last > hi) hi = last;
        }
        if (lo == 0xFFFF) return {};
        AddrSpan s;
        s.minAddr = lo;
        s.qty     = static_cast<uint16_t>(hi - lo + 1);
        return s;
    }

private:
    // ── 配置解析 ──
    bool ParseGlobal(const std::string& key, const std::string& val);
    bool ParseChannel(const std::string& key, const std::string& val,
                      ChannelConfig& ch);
    bool ParseTemplateEntry(const std::string& prefix, const std::string& params,
                            DeviceConfig& tmpl);
    bool ParseDeviceEntry(const std::string& prefix, const std::string& params,
                          DeviceConfig& dev);
    bool ParseDIMapping(const std::vector<std::pair<std::string,std::string>>& kv,
                        DIMapping& di);
    bool ParseAIMapping(const std::vector<std::pair<std::string,std::string>>& kv,
                        AIMapping& ai);
    bool ParseDOMapping(const std::vector<std::pair<std::string,std::string>>& kv,
                        DOMapping& do_);
    bool ParseAOMapping(const std::vector<std::pair<std::string,std::string>>& kv,
                        AOMapping& ao);

    // ── 模板合并 ──
    void MergeTemplate(DeviceConfig& dev, const DeviceConfig& tmpl);

    // ── 通道线程 ──
    void ChannelThread(int chIdx);

    // ── 设备采集 ──
    void PollDevice(socket& sock, const DeviceConfig& dev, int maxDiRead, int maxAiRead, int keepAliveAddr = -1);
    bool ReadAndDispatch(socket& sock, uint16_t transId,
                         const DeviceConfig& dev,
                         uint8_t devFunc,
                         const std::vector<size_t>& diIndices,
                         const std::vector<size_t>& aiIndices);
    /// FC03/04 带地址范围的批量读取
    bool ReadAndDispatch(socket& sock, uint16_t transId,
                         const DeviceConfig& dev,
                         uint8_t devFunc,
                         const std::vector<size_t>& diIndices,
                         const std::vector<size_t>& aiIndices,
                         uint16_t minAddr, uint16_t qty);

    // ── DO/AO 回写 ──
    void WriteDOChanges(socket& sock, uint16_t& transId,
                        const DeviceConfig& dev);
    void WriteAOChanges(socket& sock, uint16_t& transId,
                        const DeviceConfig& dev);

    // ── 帧收发 ──
    static bool BuildAndSend(socket& s, uint16_t transId, uint8_t station,
                             const uint8_t* pdu, size_t pduLen);
    static bool RecvFrame(socket& s, uint8_t* buf, size_t bufSize,
                          uint16_t expectedTransId, size_t& outLen);

    // ── 数据状态跟踪（通讯中断判断） ──
    // key = "ch_dev", value = true=在线 false=离线
    std::map<std::string, bool> commStatus_;
    mutable std::mutex commMtx_;    // 保护 commStatus_ 的并发访问

    // ── DO/AO 上次已发送值跟踪（独立于 CheckAllPointChange 的 lastMaster 同步） ──
    // key = "ch_dev_pt", value = 上次成功写入 Modbus 的值
    std::map<std::string, bool> doSent_;
    std::map<std::string, double> aoSent_;
    mutable std::mutex sentMtx_;

    // ── DO/AO 脉冲复位队列 ──
    struct PulseEntry {
        uint8_t  channel;
        uint8_t  stationId;
        uint8_t  func;       // 5=DO, 6=AO
        uint16_t addr;
        uint8_t  valHi;      // FC05: 0xFF/0x00, FC06: val>>8
        uint8_t  valLo;      // FC05: 0x00,       FC06: val&0xFF
        uint64_t readyAtMs;  // 到此刻发送复位
    };
    std::vector<PulseEntry> pulseQueue_;
    mutable std::mutex pulseMtx_;

    // ── 脉冲处理 ──
    void ProcessPulseQueue(socket& sock, uint16_t& transId);

    MasterConfig config_;
    std::map<std::string, DeviceConfig> templates_;  // name → template
    std::atomic<bool> running_{false};
    std::vector<std::thread> threads_;
};

#endif // MODBUS_TCP_MASTER_H
