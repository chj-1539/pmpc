#ifndef PMPC_H
#define PMPC_H

//=============================================================================
// pmpc.h — 四遥数据管理系统公共头文件
// 包含：类型定义、ConfigErrorReporter、RemoteDataMgr 声明
// 跨平台支持：使用标准 C++17，无平台特定 API
//=============================================================================

#include <cstdint>
#include <string>
#include <vector>
#include <deque>
#include <mutex>
#include <memory>
#include <atomic>
#include <chrono>
#include <thread>

// ==================== 四遥测点定义 ====================

/// 遥信 (Digital Input)
struct DiPoint
{
    uint16_t pointNo;
    bool     value;
    uint64_t tsMs;
    bool     lastVal;       // 缓存上一次值，用于变化检测
};

/// 遥测 (Analog Input)
struct AiPoint
{
    uint16_t pointNo;
    double   value;
    double   lastVal;       // 缓存上一次值
};

/// 遥控 (Digital Output) — 含主站下发值与从站反馈值
struct DoPoint
{
    uint16_t pointNo;
    bool     masterVal;
    bool     slaveVal;
    bool     lastMaster;
    bool     lastSlave;
};

/// 遥调 (Analog Output)
struct AoPoint
{
    uint16_t pointNo;
    double   value;
    double   lastVal;
};

/// 设备（每个设备持有独立互斥锁，互不干扰）
struct Device
{
    uint16_t                              devNo;
    std::vector<DiPoint> diList;
    std::vector<AiPoint> aiList;
    std::vector<DoPoint> doList;
    std::vector<AoPoint> aoList;
    mutable std::unique_ptr<std::mutex>   devMtx;   ///< 本设备数据锁，仅保护本设备点位数据

    Device() : devMtx(std::make_unique<std::mutex>()) {}
    Device(Device&&) = default;
    Device& operator=(Device&&) = default;
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;
    ~Device() = default;
};

/// 通道
struct Channel
{
    uint16_t            chId;
    std::vector<Device> devList;
};

// ==================== 配置错误报告器 ====================

/**
 * @brief 配置解析过程中的错误/警告收集器
 *        支持收集、打印、保存到文件
 */
class ConfigErrorReporter
{
public:
    explicit ConfigErrorReporter(const std::string& logDir = "logs");
    ~ConfigErrorReporter() = default;

    /// 报告一行错误（带行号）
    void Report(int lineNum, const std::string& level, const std::string& message);

    /// 报告全局错误（无行号）
    void Report(const std::string& level, const std::string& message);

    bool HasErrors() const;
    size_t ErrorCount() const { return errors_.size(); }
    void Clear();

    /// 将所有错误写入日志文件（自动创建目录）
    bool Save(const std::string& configPath = "");

    /// 打印摘要到 stderr
    void PrintSummary() const;

private:
    std::string logDir_;
    std::vector<std::string> errors_;

    std::string MakeTimestamp() const;
    static bool CreateDir(const std::string& path);
};

// ==================== 全局数据管理器（单例） ====================

/**
 * @brief 四遥数据全局共享表
 *        单例模式，内部所有读写接口互斥
 *
 * 锁顺序（防止死锁，所有代码必须遵守）：
 *   1. mysqlMtx_ (DataRecorder)
 *   2. structMtx_ (RemoteDataMgr 通道结构)
 *   3. pulseMtx_ / aoQueueMtx_ (RemoteDataMgr 脉冲/AO队列)
 *   4. devMtx (每设备独立锁)
 *   5. EventBus::Publish (从不持锁调用)
 */
class RemoteDataMgr
{
public:
    static RemoteDataMgr& Instance();
    RemoteDataMgr(const RemoteDataMgr&) = delete;
    RemoteDataMgr& operator=(const RemoteDataMgr&) = delete;

    // ---- 配置 ----
    bool LoadConfig(const std::string& iniPath,
                    ConfigErrorReporter* reporter = nullptr);
    void ClearAll();

    // ---- 遍历查询（供 TCP Server 等模块使用，线程安全） ----
    std::vector<uint16_t> GetChannelIds();
    std::vector<uint16_t> GetDeviceIds(uint16_t ch);
    bool GetAiList(uint16_t ch, uint16_t dev, std::vector<AiPoint>& out);
    bool GetDiList(uint16_t ch, uint16_t dev, std::vector<DiPoint>& out);
    bool GetDoList(uint16_t ch, uint16_t dev, std::vector<DoPoint>& out);
    bool GetAoList(uint16_t ch, uint16_t dev, std::vector<AoPoint>& out);

    // ---- 读接口（子程序调用） ----
    bool GetDi(uint16_t ch, uint16_t dev, uint16_t pt, DiPoint& out);
    bool GetAi(uint16_t ch, uint16_t dev, uint16_t pt, AiPoint& out);
    bool GetDo(uint16_t ch, uint16_t dev, uint16_t pt, DoPoint& out);
    bool GetAo(uint16_t ch, uint16_t dev, uint16_t pt, AoPoint& out);

    // ---- 写接口（子程序调用） ----
    bool SetDi(uint16_t ch, uint16_t dev, uint16_t pt,
               bool val, uint64_t ts, bool change);
    bool SetAi(uint16_t ch, uint16_t dev, uint16_t pt, double val);
    bool SetDoMaster(uint16_t ch, uint16_t dev, uint16_t pt, bool val);
    bool SetDoSlave(uint16_t ch, uint16_t dev, uint16_t pt, bool val);
    bool SetAo(uint16_t ch, uint16_t dev, uint16_t pt, double val);

    // ---- 遍历检测变化 ----
    void CheckAllPointChange();

    // ---- 脉冲复位控制 ----
    /// DO 脉冲宽度（毫秒），0→1 后自动复位为 0
    void SetDoPulseMs(uint64_t ms) { doPulseMs_ = ms; }

private:
    RemoteDataMgr() = default;
    std::vector<Channel> channels_;
    mutable std::mutex structMtx_;   ///< 保护 channels_ 向量结构（查找通道时短暂持有）

    // 内部工具函数（Split 使用 str_util.h 的全局版本）
    uint16_t ParseChannelId(const std::string& sec);
    uint16_t ParseDevNo(const std::string& key);
    Channel* FindCh(uint16_t chId);
    /// 通过 ID 查找通道索引（调用者须持有 structMtx_ 锁或无并发修改 channels_）
    size_t   FindChIdx(uint16_t chId) const;
    Device*  FindDev(Channel* ch, uint16_t devNo);

    /// 安全锁定设备并执行操作（同时持有 structMtx_ 和 devMtx）
    /// 避免 FindDeviceForRead 的裸指针竞态条件
    template<typename F>
    bool WithDeviceLocked(uint16_t ch, uint16_t dev, F&& func) {
        std::lock_guard<std::mutex> lock(structMtx_);
        Channel* pCh = FindCh(ch);
        if (!pCh) return false;
        Device* pDev = FindDev(pCh, dev);
        if (!pDev) return false;
        std::lock_guard<std::mutex> devLock(*pDev->devMtx);
        return func(*pDev);
    }

    // ---- DO 脉冲复位 FIFO 队列 ----
    struct DoPulseEntry {
        uint16_t channel;
        uint16_t device;
        uint16_t point;
        uint64_t enqueueMs;   ///< 0→1 触发时刻（ms）
    };
    std::deque<DoPulseEntry> pulseQueue_;
    mutable std::mutex pulseMtx_;
    uint64_t doPulseMs_ = 300;   ///< 脉冲宽度，默认 300ms

    // ---- AO 变化追踪队列 ----
    struct AoChangeRecord {
        uint16_t channel;
        uint16_t device;
        uint16_t point;
        double   value;
        uint64_t tsMs;
    };
    std::deque<AoChangeRecord> aoChangeQueue_;
    mutable std::mutex aoQueueMtx_;

    void CheckPulseQueue();
};

// ==================== 全局退出标志 ====================

/// 主程序与子线程共享：设为 false 时所有循环退出
extern std::atomic<bool> g_running;

#endif // PMPC_H
