#ifndef DATA_RECORDER_H
#define DATA_RECORDER_H

//=============================================================================
// data_recorder.h — MySQL 数据存储模块
//
// 功能：
//   将四遥数据存入 MySQL，支持多 PMPC 实例共享同一数据库
//   - rt_status 实时值表（带 source+tag 区分多源）
//   - di_log_YYYYMM DI 变化记录表（按月分表，事件驱动）
//   - ai_log_YYYYMM AI 定时存盘表（按月分表，可配间隔+时间对齐）
//   - 点位可配置，支持自定义标签名
//   - Schema 版本管理
//=============================================================================

#include "module_manager.h"
#include "event_bus.h"
#include "data_recorder_helpers.h"
#include <mysql.h>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <memory>
#include <atomic>
#include <thread>
#include <mutex>
#include <cstdint>
#include <functional>

// ==================== 点位配置 ====================

struct DataPointKey {
    uint16_t ch, dev, pt;
    bool operator<(const DataPointKey& o) const {
        if (ch != o.ch) return ch < o.ch;
        if (dev != o.dev) return dev < o.dev;
        return pt < o.pt;
    }
};

struct AIPointConfig {
    std::string tag;
    int intervalMs = 5000;   // 存盘间隔
    int64_t lastSaveMs = 0;  // 上次存盘时间
};

struct DIPointConfig {
    std::string tag;
};

struct DOPointConfig {
    std::string tag;
};

struct AOPointConfig {
    std::string tag;
    int intervalMs = 300000;
    int64_t lastSaveMs = 0;
};

// ==================== 数据记录器（全局单例） ====================

class DataRecorder {
public:
    static DataRecorder& Instance();

    // ── TimerThread 配置健壮化辅助（CR-2 修复配套；实现见
    //    data_recorder_helpers.h，让测试可零依赖调用）──
    static inline int ClampAiIntervalMs(int cfg) {
        return pmpc::data_recorder::ClampAiIntervalMs(cfg);
    }
    static inline int ComputeCleanupPeriodTicks(int effectiveIntervalMs) {
        return pmpc::data_recorder::ComputeCleanupPeriodTicks(effectiveIntervalMs);
    }

    bool Init(const std::string& iniPath);
    void Stop();
    bool IsConnected() const { return connected_; }

    // ── EventBus 回调 ──
    void OnDIChange(const DIChange& ev);
    void OnAIChange(const AIChange& ev);
    void OnDOChange(const DOChange& ev);
    void OnAOChange(const AOChange& ev);

    // ── 状态查询 ──
    std::string GetStatus() const;

    friend class DataRecorderModule;

private:
    DataRecorder() = default;
    ~DataRecorder();
    DataRecorder(const DataRecorder&) = delete;
    DataRecorder& operator=(const DataRecorder&) = delete;

    // ── MySQL 连接 ──
    bool ConnectMySQL();
    void DisconnectMySQL();
    bool ExecSQL(const std::string& sql);
    bool ExecSQLPrintLocked(const std::string& sql, const char* errTag); // 调用方需已持 mysqlMtx_

    // ── 建表 ──
    bool CreateTables();
    bool CreateRTStatus();
    bool CreateDILogLocked(const std::string& tableName); // 调用方需已持 mysqlMtx_
    bool CreateAILog(const std::string& tableName);
    bool CreateInstanceTable();
    bool CheckSchemaVersion();

    // ── 写入 ──
    void WriteDI(const DIChange& ev, const std::string& tag,
                 const std::string& oldVal = "0");
    void WriteAI();             // 定时器：批量写入 AI 日志
    void WriteRT(const std::string& type, const std::string& tag,
                  uint16_t ch, uint16_t dev, uint16_t pt,
                  const std::string& val, uint64_t ts_ms);

    // ── 时间对齐 ──
    static int64_t AlignTimestamp(int64_t ts_ms, int intervalMs);

    // ── 辅助 ──
    static std::string MonthSuffix();
    std::string CurrentMonthTable(const char* prefix);
    std::string Escape(const std::string& s) const;

    // ── 配置解析 ──
    bool ParseConfig(const std::string& path);
    static bool ParsePointKey(const std::string& params, uint16_t& ch,
                               uint16_t& dev, uint16_t& pt);

    // ── 定时器线程 ──
    void TimerThread();

    // ── 删除过期月度表 ──
    void DropOldTables();

    // ── 配置 ──
    std::string source_;        // 数据源标识
    std::string host_ = "127.0.0.1";
    int port_ = 3306;
    std::string dbName_ = "pmpc_data";
    std::string user_ = "root";
    std::string password_;
    std::atomic<bool> enabled_{false};
    int aiIntervalMs_ = 5000;
    int retentionMonths_ = 12;

    // 点位过滤
    std::map<DataPointKey, DIPointConfig> diPoints_;
    std::map<DataPointKey, AIPointConfig> aiPoints_;
    std::map<DataPointKey, DOPointConfig> doPoints_;
    std::map<DataPointKey, AOPointConfig> aoPoints_;

    // MySQL
    // DR-1（第二轮）：mysql_ 改为 atomic 指针 —— 快速路径 On*Change 里的
    // `if (!mysql_) return` 与 TimerThread 里的 swap 并发发生时，普通指针
    // 读写在 C++ memory model 下是数据竞争 UB。用 atomic 让 load(acquire)
    // 与 store(release) 建立 happens-before。SQL 调用点需先 load 到局部
    // 变量再传给 mysql_query。
    //
    // 用法：
    //   快速路径判活：if (!MysqlIsUp()) return;         // 无 memory barrier 之下的粗筛
    //   要用 handle：  MYSQL* h = mysql_.load(order);   // 传给 mysql_query 等
    std::atomic<MYSQL*> mysql_{nullptr};
    inline bool MysqlIsUp() const {
        return mysql_.load(std::memory_order_acquire) != nullptr;
    }
    std::atomic<bool> connected_{false};
    mutable std::mutex mysqlMtx_;

    // 定时器
    std::thread timerThr_;
    std::atomic<bool> running_{false};
};

// ==================== AppModule 包装 ====================

class DataRecorderModule : public AppModule {
public:
    DataRecorderModule();
    ~DataRecorderModule() override;
    const char* Name() const override { return "data_recorder"; }
    bool LoadConfig(const std::string& cfgPath) override;
    bool ValidateConfig(const std::string& cfgPath,
                        std::vector<std::string>& errors) override;
    bool Start() override;
    void Stop() override;
    bool IsRunning() const override;

    static DataRecorderModule* GetInstance();
    DataRecorder& GetRecorder() { return recorder_; }

private:
    DataRecorder recorder_;
    std::string cfgPath_;
    bool loaded_ = false;
    bool running_ = false;
    size_t tokenDI_ = 0;
    size_t tokenAI_ = 0;
    size_t tokenDO_ = 0;
    size_t tokenAO_ = 0;
};

#endif // DATA_RECORDER_H
