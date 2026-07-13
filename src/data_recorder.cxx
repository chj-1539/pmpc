//=============================================================================
// data_recorder.cxx — MySQL 数据存储模块实现
//
// 表结构：
//   schema_version   — 版本管理
//   rt_status        — 实时值（带 source+tag）
//   di_log_YYYYMM    — DI 变化记录（按月）
//   ai_log_YYYYMM    — AI 定时存盘（按月）
//   pmpc_instance    — 实例在线状态
//=============================================================================

#include "data_recorder.h"
#include "module_factory.h"
#include "ini_reader.h"
#include "str_util.h"
#include "packet_logger.h"
#include "pmpc.h"

#include <mysql.h>

#include <iostream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <ctime>
#include <cstring>

static constexpr int SCHEMA_VERSION = 1;

// ==================== 时间工具 ====================

// NowMs() 由 str_util.h 提供

std::string DataRecorder::MonthSuffix()
{
    auto now = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(now);
    struct tm t;
#ifdef _WIN32
    localtime_s(&t, &tt);
#else
    localtime_r(&tt, &t);
#endif
    char buf[16];
    std::strftime(buf, sizeof(buf), "%Y%m", &t);
    return buf;
}

std::string DataRecorder::CurrentMonthTable(const char* prefix)
{
    return std::string(prefix) + "_" + MonthSuffix();
}

int64_t DataRecorder::AlignTimestamp(int64_t ts_ms, int intervalMs)
{
    if (intervalMs <= 0) return ts_ms;
    return (ts_ms / static_cast<int64_t>(intervalMs)) * static_cast<int64_t>(intervalMs);
}

// ==================== 单例 ====================

DataRecorder& DataRecorder::Instance()
{
    static DataRecorder inst;
    return inst;
}

DataRecorder::~DataRecorder()
{
    Stop();
    DisconnectMySQL();
}

// ==================== MySQL 连接管理 ====================

bool DataRecorder::ConnectMySQL()
{
    if (mysql_) return true;

    mysql_ = mysql_init(nullptr);
    if (!mysql_) {
        std::cerr << "[DataRecorder] mysql_init 失败" << std::endl;
        return false;
    }

    if (!mysql_real_connect(mysql_, host_.c_str(), user_.c_str(),
                            password_.c_str(), dbName_.c_str(),
                            static_cast<unsigned int>(port_),
                            nullptr, 0))
    {
        std::cerr << "[DataRecorder] MySQL 连接失败: " << mysql_error(mysql_)
                  << " (" << host_ << ":" << port_ << ")" << std::endl;
        mysql_close(mysql_);
        mysql_ = nullptr;
        return false;
    }

    // 设置 UTF8MB4
    mysql_set_character_set(mysql_, "utf8mb4");

    connected_ = true;
    std::cout << "[DataRecorder] MySQL 已连接: " << host_ << ":" << port_
              << " db=" << dbName_ << " source=" << source_ << std::endl;
    return true;
}

void DataRecorder::DisconnectMySQL()
{
    if (mysql_) {
        mysql_close(mysql_);
        mysql_ = nullptr;
    }
    connected_ = false;
}

bool DataRecorder::ExecSQL(const std::string& sql)
{
    std::lock_guard<std::mutex> lock(mysqlMtx_);
    if (!mysql_) return false;
    if (mysql_query(mysql_, sql.c_str()) != 0) {
        std::cerr << "[DataRecorder] SQL 错误: " << mysql_error(mysql_)
                  << "\n  SQL: " << sql.substr(0, 200) << std::endl;
        return false;
    }
    return true;
}

bool DataRecorder::ExecSQLPrintLocked(const std::string& sql, const char* errTag)
{
    if (!mysql_) return false;
    if (mysql_query(mysql_, sql.c_str()) != 0) {
        std::cerr << "[DataRecorder] " << errTag << ": " << mysql_error(mysql_) << std::endl;
        return false;
    }
    return true;
}

bool DataRecorder::ExecSQLPrint(const std::string& sql, const char* errTag)
{
    std::lock_guard<std::mutex> lock(mysqlMtx_);
    return ExecSQLPrintLocked(sql, errTag);
}

std::string DataRecorder::Escape(const std::string& s) const
{
    if (!mysql_) return s;
    std::vector<char> buf(s.size() * 2 + 1, 0);
    mysql_real_escape_string(mysql_, buf.data(), s.c_str(), (unsigned long)s.size());
    return buf.data();
}

// ==================== 建表 ====================

bool DataRecorder::CreateRTStatus()
{
    const char* sql = R"(
        CREATE TABLE IF NOT EXISTS rt_status (
            source  VARCHAR(64) NOT NULL COMMENT '数据源',
            tag     VARCHAR(128) NOT NULL COMMENT '标签',
            ch      INT UNSIGNED NOT NULL,
            dev     INT UNSIGNED NOT NULL,
            pt      INT UNSIGNED NOT NULL,
            type    VARCHAR(4) NOT NULL COMMENT 'DI/AI/DO/AO',
            value   VARCHAR(64) NOT NULL,
            ts_ms   BIGINT NOT NULL,
            update_time TIMESTAMP DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
            PRIMARY KEY (source, ch, dev, pt, type),
            INDEX idx_tag (tag),
            INDEX idx_source_ts (source, ts_ms)
        ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin
    )";
    return ExecSQLPrintLocked(sql, "建表 rt_status");
}

bool DataRecorder::CreateDILogLocked(const std::string& tableName)
{
    std::string sql = R"(
        CREATE TABLE IF NOT EXISTS )" + tableName + R"( (
            id      BIGINT AUTO_INCREMENT PRIMARY KEY,
            source  VARCHAR(64) NOT NULL,
            tag     VARCHAR(128) NOT NULL,
            ch      INT UNSIGNED NOT NULL,
            dev     INT UNSIGNED NOT NULL,
            pt      INT UNSIGNED NOT NULL,
            old_val TINYINT NOT NULL,
            new_val TINYINT NOT NULL,
            ts_ms   BIGINT NOT NULL,
            INDEX idx_source_ts (source, ts_ms),
            INDEX idx_tag_ts (tag, ts_ms),
            INDEX idx_dev (source, ch, dev, pt, ts_ms)
        ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin
    )";
    return ExecSQLPrintLocked(sql, ("建表 " + tableName).c_str());
}

bool DataRecorder::CreateDILog(const std::string& tableName)
{
    std::lock_guard<std::mutex> lock(mysqlMtx_);
    return CreateDILogLocked(tableName);
}

bool DataRecorder::CreateAILog(const std::string& tableName)
{
    std::string sql = R"(
        CREATE TABLE IF NOT EXISTS )" + tableName + R"( (
            id      BIGINT AUTO_INCREMENT PRIMARY KEY,
            source  VARCHAR(64) NOT NULL,
            tag     VARCHAR(128) NOT NULL,
            ch      INT UNSIGNED NOT NULL,
            dev     INT UNSIGNED NOT NULL,
            pt      INT UNSIGNED NOT NULL,
            value   DOUBLE NOT NULL,
            ts_ms   BIGINT NOT NULL,
            INDEX idx_source_ts (source, ts_ms),
            INDEX idx_tag_ts (tag, ts_ms),
            INDEX idx_dev (source, ch, dev, pt, ts_ms)
        ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin
    )";
    return ExecSQLPrintLocked(sql, ("建表 " + tableName).c_str());
}

bool DataRecorder::CheckSchemaVersion()
{
    // 创建版本表
    {
        std::string sql = R"(
            CREATE TABLE IF NOT EXISTS schema_version (
                version INT PRIMARY KEY,
                ver_desc VARCHAR(256),
                applied TIMESTAMP DEFAULT CURRENT_TIMESTAMP
            ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4
        )";
        if (!ExecSQLPrintLocked(sql, "建表 schema_version")) return false;
    }

    // 查询当前版本
    MYSQL_RES* res = nullptr;
    if (!mysql_query(mysql_, "SELECT MAX(version) FROM schema_version")) {
        res = mysql_store_result(mysql_);
    }

    int dbVersion = 0;
    if (res) {
        MYSQL_ROW row = mysql_fetch_row(res);
        if (row && row[0]) dbVersion = SafeStoi(row[0]);
        mysql_free_result(res);
    }

    if (dbVersion > SCHEMA_VERSION) {
        std::cerr << "[DataRecorder] 警告: 数据库版本(" << dbVersion
                  << ") 比程序版本(" << SCHEMA_VERSION << ") 新"
                  << ", 降级运行" << std::endl;
    }
    else if (dbVersion < SCHEMA_VERSION) {
        std::cout << "[DataRecorder] 数据库版本 " << dbVersion
                  << " -> " << SCHEMA_VERSION << std::endl;
        std::string ins = "INSERT IGNORE INTO schema_version(version, ver_desc) VALUES("
                        + std::to_string(SCHEMA_VERSION) + ",'PMPC v2.1 initial')";
        ExecSQLPrintLocked(ins, "更新 schema_version");
    }
    else {
        std::cout << "[DataRecorder] 数据库版本一致: " << SCHEMA_VERSION << std::endl;
    }

    return true;
}

bool DataRecorder::CreateTables()
{
    if (!CheckSchemaVersion()) return false;
    if (!CreateRTStatus()) return false;
    if (!CreateInstanceTable()) return false;
    if (!CreateDILogLocked(CurrentMonthTable("di_log"))) return false;
    if (!CreateAILog(CurrentMonthTable("ai_log"))) return false;
    return true;
}

bool DataRecorder::CreateInstanceTable()
{
    const char* sql = R"(
        CREATE TABLE IF NOT EXISTS pmpc_instance (
            source      VARCHAR(64) PRIMARY KEY COMMENT '数据源',
            description VARCHAR(256) COMMENT '描述',
            ip          VARCHAR(45) COMMENT 'IP地址',
            last_seen   TIMESTAMP DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP
        ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4
    )";
    return ExecSQLPrintLocked(sql, "建表 pmpc_instance");
}

// ==================== 配置解析 ====================

bool DataRecorder::ParsePointKey(const std::string& params,
                                  uint16_t& ch, uint16_t& dev, uint16_t& pt)
{
    auto kv = ParseKeyValues(params);
    ch = dev = pt = 0;
    for (auto& [k, v] : kv) {
        auto lk = ToLower(k);
        if (lk == "ch")   ch  = static_cast<uint16_t>(SafeStoi(v));
        if (lk == "dev")  dev = static_cast<uint16_t>(SafeStoi(v));
        if (lk == "pt")   pt  = static_cast<uint16_t>(SafeStoi(v));
    }
    return ch > 0 || dev > 0 || pt > 0;
}

bool DataRecorder::ParseConfig(const std::string& path)
{
    IniReader ini;
    if (!ini.Load(path)) {
        std::cerr << "[DataRecorder] 无法打开配置: " << path << std::endl;
        return false;
    }

    // ── [global] ──
    source_    = ini.Get("global", "source", "");
    host_      = ini.Get("global", "host", "127.0.0.1");
    port_      = ini.GetInt("global", "port", 3306);
    dbName_    = ini.Get("global", "db", "pmpc_data");
    user_      = ini.Get("global", "user", "root");
    password_  = ini.Get("global", "password", "");
    // 支持从环境变量 MYSQL_PASSWORD 覆盖密码（优先级高于配置文件）
    { const char* envPwd = std::getenv("MYSQL_PASSWORD"); if (envPwd && envPwd[0]) password_ = envPwd; }
    aiIntervalMs_ = ini.GetInt("global", "ai_interval_ms", 5000);
    retentionMonths_ = ini.GetInt("global", "retention_months", 12);
    int en     = ini.GetInt("global", "enabled", 1);
    enabled_   = (en != 0);

    if (source_.empty()) {
        std::cerr << "[DataRecorder] 错误: source 未设置，必须指定数据源标识" << std::endl;
        return false;
    }

    // ── [di_points] ──
    if (ini.HasSection("di_points")) {
        for (auto& key : ini.Keys("di_points")) {
            auto val = ini.Get("di_points", key, "");
            uint16_t ch, dev, pt;
            if (!ParsePointKey(val, ch, dev, pt)) continue;
            diPoints_[{ch, dev, pt}] = {key};
        }
    }

    // ── [ai_points] ──
    if (ini.HasSection("ai_points")) {
        for (auto& key : ini.Keys("ai_points")) {
            auto val = ini.Get("ai_points", key, "");
            auto kv = ParseKeyValues(val);
            uint16_t ch = 0, dev = 0, pt = 0;
            int intervalMs = aiIntervalMs_;
            for (auto& [k, v] : kv) {
                auto lk = ToLower(k);
                if (lk == "ch")   ch  = static_cast<uint16_t>(SafeStoi(v));
                if (lk == "dev")  dev = static_cast<uint16_t>(SafeStoi(v));
                if (lk == "pt")   pt  = static_cast<uint16_t>(SafeStoi(v));
                if (lk == "interval_ms") intervalMs = SafeStoi(v);
            }
            if (ch > 0 || dev > 0 || pt > 0) {
                aiPoints_[{ch, dev, pt}] = {key, intervalMs, 0};
            }
        }
    }

    // ── [do_points] ──
    if (ini.HasSection("do_points")) {
        for (auto& key : ini.Keys("do_points")) {
            auto val = ini.Get("do_points", key, "");
            uint16_t ch, dev, pt;
            if (!ParsePointKey(val, ch, dev, pt)) continue;
            doPoints_[{ch, dev, pt}] = {key};
        }
    }

    // ── [ao_points] ──
    if (ini.HasSection("ao_points")) {
        for (auto& key : ini.Keys("ao_points")) {
            auto val = ini.Get("ao_points", key, "");
            auto kv = ParseKeyValues(val);
            uint16_t ch = 0, dev = 0, pt = 0;
            int intervalMs = 300000;
            for (auto& [k, v] : kv) {
                auto lk = ToLower(k);
                if (lk == "ch")   ch  = static_cast<uint16_t>(SafeStoi(v));
                if (lk == "dev")  dev = static_cast<uint16_t>(SafeStoi(v));
                if (lk == "pt")   pt  = static_cast<uint16_t>(SafeStoi(v));
                if (lk == "interval_ms") intervalMs = SafeStoi(v);
            }
            if (ch > 0 || dev > 0 || pt > 0) {
                aoPoints_[{ch, dev, pt}] = {key, intervalMs, 0};
            }
        }
    }

    std::cout << "[DataRecorder] 配置加载: source=" << source_
              << " DI=" << diPoints_.size()
              << " AI=" << aiPoints_.size()
              << " DO=" << doPoints_.size()
              << " AO=" << aoPoints_.size()
              << std::endl;
    return true;
}

// ==================== 写入操作 ====================

void DataRecorder::WriteRT(const std::string& type, const std::string& tag,
                            uint16_t ch, uint16_t dev, uint16_t pt,
                            const std::string& val, uint64_t ts_ms)
{
    if (!mysql_ || !enabled_) return;
    std::string src = Escape(source_);
    std::string t = Escape(tag);

    std::string sql = "INSERT INTO rt_status(source,tag,ch,dev,pt,type,value,ts_ms) VALUES('"
        + src + "','" + t + "'," + std::to_string(ch) + "," + std::to_string(dev) + ","
        + std::to_string(pt) + ",'" + type + "','" + Escape(val) + "',"
        + std::to_string(ts_ms)
        + ") ON DUPLICATE KEY UPDATE value=VALUES(value), ts_ms=VALUES(ts_ms), tag=VALUES(tag)";
    ExecSQLPrint(sql, "更新 rt_status");
}

void DataRecorder::WriteDI(const DIChange& ev, const std::string& tag,
                            const std::string& oldVal)
{
    if (!mysql_ || !enabled_) return;

    std::string table = CurrentMonthTable("di_log");
    // 确保月度表存在
    CreateDILog(table);

    std::string src = Escape(source_);
    std::string t = Escape(tag);

    std::string sql = "INSERT INTO " + table
        + "(source,tag,ch,dev,pt,old_val,new_val,ts_ms) VALUES('"
        + src + "','" + t + "',"
        + std::to_string(ev.channel) + "," + std::to_string(ev.device) + ","
        + std::to_string(ev.point) + ","
        + oldVal + "," + (ev.value ? "1" : "0") + ","
        + std::to_string(ev.tsMs) + ")";
    ExecSQLPrint(sql, "INSERT di_log");
}

void DataRecorder::WriteAI()
{
    // 注意：调用者（TimerThread）已持有 mysqlMtx_ 锁
    if (!mysql_ || !enabled_) return;
    if (aiPoints_.empty()) return;

    auto& mgr = RemoteDataMgr::Instance();
    uint64_t now = NowMs();
    std::string table = CurrentMonthTable("ai_log");
    CreateAILog(table);

    std::string src = Escape(source_);

    for (auto& [key, cfg] : aiPoints_) {
        // 检查是否达到存盘间隔
        if (now - cfg.lastSaveMs < static_cast<uint64_t>(cfg.intervalMs))
            continue;

        AiPoint pt;
        if (!mgr.GetAi(key.ch, key.dev, key.pt, pt))
            continue;

        int64_t alignedTs = AlignTimestamp(now, cfg.intervalMs);

        std::string t = Escape(cfg.tag);

        std::string sql = "INSERT INTO " + table
            + "(source,tag,ch,dev,pt,value,ts_ms) VALUES('"
            + src + "','" + t + "',"
            + std::to_string(key.ch) + "," + std::to_string(key.dev) + ","
            + std::to_string(key.pt) + ","
            + std::to_string(pt.value) + ","
            + std::to_string(alignedTs) + ")";
        ExecSQLPrintLocked(sql, "INSERT ai_log");

        cfg.lastSaveMs = static_cast<int64_t>(now);
    }
}

// ==================== EventBus 回调 ====================

void DataRecorder::OnDIChange(const DIChange& ev)
{
    if (!enabled_ || !mysql_) return;

    // 在持 mysqlMtx_ 前先读旧值，避免锁顺序依赖
    std::string oldVal = "0";
    DiPoint pt;
    if (RemoteDataMgr::Instance().GetDi(ev.channel, ev.device, ev.point, pt)) {
        oldVal = pt.lastVal ? "1" : "0";
    }

    std::lock_guard<std::mutex> lock(mysqlMtx_);
    auto it = diPoints_.find({ev.channel, ev.device, ev.point});
    if (it == diPoints_.end()) return;
    WriteDI(ev, it->second.tag, oldVal);
    WriteRT("DI", it->second.tag, ev.channel, ev.device, ev.point,
            ev.value ? "1" : "0", ev.tsMs);
}

void DataRecorder::OnAIChange(const AIChange& ev)
{
    if (!enabled_ || !mysql_) return;
    std::lock_guard<std::mutex> lock(mysqlMtx_);
    auto it = aiPoints_.find({ev.channel, ev.device, ev.point});
    if (it == aiPoints_.end()) return;
    // AI 不立刻写日志（定时器批量写），仅更新实时值
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.6f", ev.value);
    WriteRT("AI", it->second.tag, ev.channel, ev.device, ev.point, buf, ev.tsMs);
}

void DataRecorder::OnDOChange(const DOChange& ev)
{
    if (!enabled_ || !mysql_) return;
    std::lock_guard<std::mutex> lock(mysqlMtx_);
    auto it = doPoints_.find({ev.channel, ev.device, ev.point});
    if (it == doPoints_.end()) return;
    WriteRT("DO", it->second.tag, ev.channel, ev.device, ev.point,
            ev.masterVal ? "1" : "0", ev.tsMs);
}

void DataRecorder::OnAOChange(const AOChange& ev)
{
    if (!enabled_ || !mysql_) return;
    std::lock_guard<std::mutex> lock(mysqlMtx_);
    auto it = aoPoints_.find({ev.channel, ev.device, ev.point});
    if (it == aoPoints_.end()) return;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.6f", ev.value);
    WriteRT("AO", it->second.tag, ev.channel, ev.device, ev.point, buf, ev.tsMs);
}

// ==================== 删除过期月度表 ====================

void DataRecorder::DropOldTables()
{
    if (retentionMonths_ <= 0 || !mysql_) return;
    // 列出所有 di_log_ 和 ai_log_ 开头且日期早于 retention 的表
    auto dropPrefix = [&](const char* prefix) {
        MYSQL_RES* res = nullptr;
        std::string sql = std::string("SHOW TABLES LIKE '") + prefix + "_%'";
        if (mysql_query(mysql_, sql.c_str())) return;
        res = mysql_store_result(mysql_);
        if (!res) return;
        MYSQL_ROW row;
        while ((row = mysql_fetch_row(res))) {
            std::string tblName = row[0];
            // 提取 YYYYMM 后缀
            if (tblName.size() < 10) continue;
            std::string suffix = tblName.substr(tblName.size() - 6);
            int tblMonth = SafeStoi(suffix, 0);
            if (tblMonth <= 0) continue;
            // 计算当前月 - retentionMonths_
            auto now = std::chrono::system_clock::now();
            auto tt = std::chrono::system_clock::to_time_t(now);
            struct tm t;
#ifdef _WIN32
            localtime_s(&t, &tt);
#else
            localtime_r(&tt, &t);
#endif
            int curMonth = (t.tm_year + 1900) * 100 + (t.tm_mon + 1);
            // 计算 retention 边界（忽略日）
            int y = curMonth / 100, m = curMonth % 100;
            m -= retentionMonths_;
            while (m <= 0) { y--; m += 12; }
            int cutoff = y * 100 + m;
            if (tblMonth < cutoff) {
                std::string drop = "DROP TABLE IF EXISTS " + tblName;
                mysql_query(mysql_, drop.c_str());
                std::cout << "[DataRecorder] 删除过期表: " << tblName << std::endl;
            }
        }
        mysql_free_result(res);
    };
    dropPrefix("di_log");
    dropPrefix("ai_log");
}

// ==================== 定时器线程 ====================

void DataRecorder::TimerThread()
{
    std::cout << "[DataRecorder] 定时器线程启动, AI存盘间隔="
              << aiIntervalMs_ << "ms" << std::endl;

    int tickCount = 0;
    while (running_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(aiIntervalMs_));

        if (!running_) break;
        if (!enabled_ || !mysql_) continue;

        // H10 修复：先在锁**外**做慢的 mysql_ping / 断连重连，仅当拿到
        // 新连接（或确认现有连接可用）后再拿 mysqlMtx_ 做 swap + 快速
        // 状态更新 + AI 批量写。之前整个 ping/reconnect 段都在 mysqlMtx_
        // 里，重连若需要几十秒，其间 OnDIChange/OnAIChange 等订阅者被
        // 阻塞，EventBus Publish 也随之停摆。
        bool needReconnect = false;
        {
            std::lock_guard<std::mutex> lock(mysqlMtx_);
            if (mysql_ && mysql_ping(mysql_) != 0) {
                std::cerr << "[DataRecorder] MySQL 连接断开，尝试重连..." << std::endl;
                needReconnect = true;
            }
        }
        // 重连在无锁下进行 —— 建立一个独立的 MYSQL* 连接，成功后再进锁 swap。
        if (needReconnect) {
            MYSQL* newConn = mysql_init(nullptr);
            if (newConn && mysql_real_connect(newConn, host_.c_str(), user_.c_str(),
                                              password_.c_str(), dbName_.c_str(),
                                              static_cast<unsigned int>(port_),
                                              nullptr, 0))
            {
                // 拿到新连接，快速切换（关旧的在锁外完成）
                MYSQL* oldConn = nullptr;
                {
                    std::lock_guard<std::mutex> lock(mysqlMtx_);
                    oldConn = mysql_;
                    mysql_ = newConn;
                    connected_ = true;
                }
                if (oldConn) mysql_close(oldConn);
                {
                    std::lock_guard<std::mutex> lock(mysqlMtx_);
                    CreateTables();   // 保留原先在锁内的表初始化
                }
                std::cout << "[DataRecorder] MySQL 重连成功" << std::endl;
            } else {
                std::cerr << "[DataRecorder] 重连失败: "
                          << (newConn ? mysql_error(newConn) : "mysql_init 失败") << std::endl;
                if (newConn) mysql_close(newConn);
                continue;   // 跳过本轮 timer 后续动作，下一轮再试
            }
        }

        {
            std::lock_guard<std::mutex> lock(mysqlMtx_);

            // 更新实例在线状态
            {
                std::string src = Escape(source_);
                std::string sql = "INSERT INTO pmpc_instance(source,description,ip) VALUES('"
                    + src + "','PMPC DataRecorder','" + src + "') "
                    "ON DUPLICATE KEY UPDATE last_seen=NOW()";
                ExecSQLPrintLocked(sql, "更新 pmpc_instance");
            }

            // AI 批量存盘
            WriteAI();

            // 每隔约2分钟检查一次过期表
            if (++tickCount % (120000 / aiIntervalMs_) == 0) {
                PacketLogger::Instance().CleanupOldLogs();
                DropOldTables();
            }
        }
    }

    std::cout << "[DataRecorder] 定时器线程退出" << std::endl;
}

// ==================== 控制接口 ====================

bool DataRecorder::Init(const std::string& iniPath)
{
    if (!ParseConfig(iniPath)) return false;
    return true;
}

void DataRecorder::Stop()
{
    running_ = false;
    if (timerThr_.joinable()) timerThr_.join();
}

std::string DataRecorder::GetStatus() const
{
    std::ostringstream oss;
    oss << "DataRecorder: source=" << source_
        << " " << (enabled_ ? "已启用" : "已禁用")
        << " MySQL=" << (connected_ ? "已连接" : "未连接")
        << " DI=" << diPoints_.size()
        << " AI=" << aiPoints_.size();
    return oss.str();
}

// ==================== DataRecorderModule ====================

DataRecorderModule* DataRecorderModule::GetInstance()
{
    if (!g_moduleManager) return nullptr;
    auto* mod = g_moduleManager->GetModule("data_recorder");
    if (!mod) return nullptr;
    return dynamic_cast<DataRecorderModule*>(mod);
}

DataRecorderModule::DataRecorderModule() {}
DataRecorderModule::~DataRecorderModule() { Stop(); }

bool DataRecorderModule::LoadConfig(const std::string& cfgPath)
{
    cfgPath_ = cfgPath;
    loaded_ = recorder_.Init(cfgPath);
    return loaded_;
}

bool DataRecorderModule::ValidateConfig(const std::string& cfgPath,
                                         std::vector<std::string>& errors)
{
    DataRecorder dr;
    if (!dr.Init(cfgPath)) {
        errors.push_back("Cannot load: " + cfgPath);
        return false;
    }
    return errors.empty();
}

bool DataRecorderModule::Start()
{
    if (running_) return true;
    if (!loaded_) return false;

    // 确保先取消旧订阅（热重载时可能残留）
    if (tokenDI_) EventBus::Unsubscribe<DIChange>(tokenDI_);
    if (tokenAI_) EventBus::Unsubscribe<AIChange>(tokenAI_);
    if (tokenDO_) EventBus::Unsubscribe<DOChange>(tokenDO_);
    if (tokenAO_) EventBus::Unsubscribe<AOChange>(tokenAO_);

    // 连接 MySQL
    auto& dr = recorder_;
    if (!dr.ConnectMySQL()) {
        std::cerr << "[DataRecorder] MySQL 连接失败，模块启动挂起"
                  << "（将在后台重试）" << std::endl;
    } else {
        if (!dr.CreateTables()) {
            std::cerr << "[DataRecorder] 建表失败" << std::endl;
            dr.DisconnectMySQL();
        }
    }

    // 订阅 EventBus（无论 MySQL 是否连接成功，先订阅事件）
    tokenDI_ = EventBus::Subscribe<DIChange>(
        [this](const DIChange& e) { recorder_.OnDIChange(e); });
    tokenAI_ = EventBus::Subscribe<AIChange>(
        [this](const AIChange& e) { recorder_.OnAIChange(e); });
    tokenDO_ = EventBus::Subscribe<DOChange>(
        [this](const DOChange& e) { recorder_.OnDOChange(e); });
    tokenAO_ = EventBus::Subscribe<AOChange>(
        [this](const AOChange& e) { recorder_.OnAOChange(e); });

    // 启动定时器
    running_ = true;
    dr.running_ = true;
    dr.timerThr_ = std::thread(&DataRecorder::TimerThread, &dr);

    std::cout << "[DataRecorder] 模块启动完成"
              << (dr.IsConnected() ? "" : "（MySQL 待连接）") << std::endl;
    return true;
}

void DataRecorderModule::Stop()
{
    if (!running_) return;

    // 取消订阅
    EventBus::Unsubscribe<DIChange>(tokenDI_);
    EventBus::Unsubscribe<AIChange>(tokenAI_);
    EventBus::Unsubscribe<DOChange>(tokenDO_);
    EventBus::Unsubscribe<AOChange>(tokenAO_);

    recorder_.Stop();
    recorder_.DisconnectMySQL();
    running_ = false;
}

bool DataRecorderModule::IsRunning() const
{
    return running_;
}

REGISTER_MODULE("data_recorder", DataRecorderModule)
