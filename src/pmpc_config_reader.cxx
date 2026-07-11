//=============================================================================
// pmpc_config_reader.cxx — 配置文件解析器
// 包含：ConfigErrorReporter 实现 + RemoteDataMgr::LoadConfig
//=============================================================================

#include "pmpc.h"
#include "str_util.h"

#include <fstream>
#include <sstream>
#include <iostream>
#include <cctype>
#include <cerrno>

// ==================== 跨平台目录创建 ====================

#ifdef _WIN32
#include <direct.h>
#define PMPC_MKDIR(path)  _mkdir(path)
#else
#include <sys/stat.h>
#define PMPC_MKDIR(path)  mkdir(path, 0755)
#endif

// ==================== ConfigErrorReporter 实现 ====================

ConfigErrorReporter::ConfigErrorReporter(const std::string& logDir)
    : logDir_(logDir)
{
}

bool ConfigErrorReporter::CreateDir(const std::string& path)
{
#ifdef _WIN32
    return PMPC_MKDIR(path.c_str()) == 0 || errno == EEXIST;
#else
    return PMPC_MKDIR(path.c_str()) == 0 || errno == EEXIST;
#endif
}

std::string ConfigErrorReporter::MakeTimestamp() const
{
    auto now = std::chrono::system_clock::now();
    auto tt  = std::chrono::system_clock::to_time_t(now);
    char buf[64] = {};

#ifdef _WIN32
    // Windows: localtime_s returns errno
    struct tm tm_buf;
    localtime_s(&tm_buf, &tt);
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_buf);
#else
    // POSIX: localtime_r is thread-safe
    struct tm tm_buf;
    localtime_r(&tt, &tm_buf);
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_buf);
#endif

    return std::string(buf);
}

void ConfigErrorReporter::Report(int lineNum,
                                 const std::string& level,
                                 const std::string& message)
{
    std::ostringstream oss;
    oss << "[第 " << lineNum << " 行] [" << level << "] " << message;
    errors_.push_back(oss.str());
}

void ConfigErrorReporter::Report(const std::string& level,
                                 const std::string& message)
{
    std::ostringstream oss;
    oss << "[全局] [" << level << "] " << message;
    errors_.push_back(oss.str());
}

bool ConfigErrorReporter::HasErrors() const
{
    return !errors_.empty();
}

void ConfigErrorReporter::Clear()
{
    errors_.clear();
}

bool ConfigErrorReporter::Save(const std::string& configPath)
{
    // 确保日志目录存在
    if (!CreateDir(logDir_))
    {
        std::cerr << "[ConfigErrorReporter] 无法创建日志目录: "
                  << logDir_ << std::endl;
        return false;
    }

    // 日志文件名：config_error_YYYYMMDD_HHMMSS.log
    auto now = std::chrono::system_clock::now();
    auto tt  = std::chrono::system_clock::to_time_t(now);

#ifdef _WIN32
    struct tm tm_buf;
    localtime_s(&tm_buf, &tt);
#else
    struct tm tm_buf;
    localtime_r(&tt, &tm_buf);
#endif

    char nameBuf[128] = {};
    std::strftime(nameBuf, sizeof(nameBuf),
                  "config_error_%Y%m%d_%H%M%S.log", &tm_buf);

    std::string fullPath = logDir_ + "/" + nameBuf;
    std::ofstream fout(fullPath);
    if (!fout.is_open())
    {
        std::cerr << "[ConfigErrorReporter] 无法写入日志文件: "
                  << fullPath << std::endl;
        return false;
    }

    // 写文件头
    fout << "========================================\n";
    fout << "  四遥系统配置错误报告\n";
    fout << "  生成时间: " << MakeTimestamp() << "\n";
    if (!configPath.empty())
        fout << "  配置文件: " << configPath << "\n";
    fout << "  错误总数: " << errors_.size() << "\n";
    fout << "========================================\n\n";

    for (size_t i = 0; i < errors_.size(); i++)
    {
        fout << (i + 1) << ".  " << errors_[i] << "\n";
    }

    fout << "\n--- 报告结束 ---\n";
    fout.close();

    std::cout << "[ConfigErrorReporter] 错误日志已保存 → "
              << fullPath << std::endl;
    return true;
}

void ConfigErrorReporter::PrintSummary() const
{
    if (errors_.empty())
    {
        std::cout << "[ConfigErrorReporter] 无错误" << std::endl;
        return;
    }

    std::cerr << "\n========== 配置错误摘要 ==========\n";
    for (const auto& e : errors_)
        std::cerr << "  " << e << "\n";
    std::cerr << "==================================\n";
    std::cerr << "共 " << errors_.size() << " 条错误/警告\n" << std::endl;
}

// ==================== RemoteDataMgr::LoadConfig ====================

bool RemoteDataMgr::LoadConfig(const std::string& iniPath,
                               ConfigErrorReporter* reporter)
{
    std::lock_guard<std::mutex> lock(structMtx_);
    ClearAll();

    std::ifstream fin(iniPath);
    if (!fin.is_open())
    {
        std::string msg = "无法打开配置文件: " + iniPath;
        std::cerr << msg << std::endl;
        if (reporter)
            reporter->Report("错误", msg);
        return false;
    }

    std::string line, curSec;
    uint16_t curChId = 0;
    int lineNum = 0;

    while (std::getline(fin, line))
    {
        lineNum++;

        // 统一处理 Windows/Unix 换行 (\r\n / \n)
        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        // 去掉首尾空白
        auto first = line.find_first_not_of(" \t");
        if (first == std::string::npos) { line.clear(); }
        else                             { line = line.substr(first); }

        auto last = line.find_last_not_of(" \t\r");
        if (last != std::string::npos)
            line = line.substr(0, last + 1);

        // 跳过空行和注释（; 或 #）
        if (line.empty() || line[0] == ';' || line[0] == '#')
            continue;

        // ---- 节头: [Channel_X] ----
        if (line.front() == '[')
        {
            if (line.back() != ']')
            {
                if (reporter)
                    reporter->Report(lineNum, "警告",
                        "节头缺少闭合 ']'，已跳过: " + line);
                curChId = 0;
                continue;
            }

            curSec = line.substr(1, line.size() - 2);
            // [global] 为描述性节头，静默跳过
            if (ToLower(curSec) == "global") { curChId = 0; continue; }
            curChId = ParseChannelId(curSec);

            if (curChId == 0)
            {
                if (reporter)
                    reporter->Report(lineNum, "警告",
                        "无法从节头解析出通道ID，已跳过: " + line);
                continue;
            }

            // 检查重复通道
            // L4 修复：以前柔性追加（push_back 一个同 chId 的 Channel），
            // 结果 channels_ 里有两条同 chId 的记录，FindCh 只返回第一条，
            // 后续 push 的 Channel 变成"死"数据。加上重复调用 LoadConfig
            // 就更混乱。改为拒绝第二个 [Channel_N]：报 error、清空
            // curChId 让后续 Dev_M 行被跳过（不会被误加到第一个通道）。
            if (FindCh(curChId) != nullptr)
            {
                if (reporter)
                    reporter->Report(lineNum, "错误",
                        "通道 " + std::to_string(curChId)
                        + " 重复定义，已忽略第二次及后续 [Channel_"
                        + std::to_string(curChId) + "] 段");
                curChId = 0;   // 跳过后续 Dev_M 行
                continue;
            }

            Channel newCh;
            newCh.chId = curChId;
            channels_.push_back(std::move(newCh));
            continue;
        }

        // ---- 节外行（无有效节头时跳过） ----
        if (curChId == 0)
            continue;

        // ---- 键值对: Dev_X = diCnt,aiCnt,doCnt,aoCnt ----
        size_t eq = line.find('=');
        if (eq == std::string::npos)
        {
            if (reporter)
                reporter->Report(lineNum, "警告",
                    "行格式错误（缺少 '='），已跳过: " + line);
            continue;
        }

        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);

        // 裁剪 key 两端空白
        auto kFirst = key.find_first_not_of(" \t");
        auto kLast  = key.find_last_not_of(" \t");
        if (kFirst != std::string::npos)
            key = key.substr(kFirst, kLast - kFirst + 1);

        if (key.substr(0, 4) != "Dev_")
            continue;   // 非设备行，忽略（如 desc=xxx）

        uint16_t devNo = ParseDevNo(key);
        if (devNo == 0)
        {
            if (reporter)
                reporter->Report(lineNum, "错误",
                    "设备键 \"" + key + "\" 中无法解析设备编号，已跳过");
            continue;
        }

        // ---- 解析点位数量 ----
        auto nums = Split(val, ',');
        uint16_t diCnt = 0, aiCnt = 0, doCnt = 0, aoCnt = 0;

        try {
            if (nums.size() >= 1) diCnt = static_cast<uint16_t>(std::stoi(nums[0]));
            if (nums.size() >= 2) aiCnt = static_cast<uint16_t>(std::stoi(nums[1]));
            if (nums.size() >= 3) doCnt = static_cast<uint16_t>(std::stoi(nums[2]));
            if (nums.size() >= 4) aoCnt = static_cast<uint16_t>(std::stoi(nums[3]));
        }
        catch (const std::exception& e)
        {
            if (reporter)
                reporter->Report(lineNum, "错误",
                    "点位数量格式错误 (" + val + "): " + e.what()
                    + "，该设备已跳过");
            continue;
        }

        // 上限校验：pointNo 是 uint16_t（最大 65535）。DI 需要额外预留 pt=1
        // 作为通讯状态位，故业务 diCnt ≤ 65534；AI/DO/AO 上限为 65535。
        // 见 CLAUDE.md 「已知陷阱」M13。
        if (diCnt > 65534)
        {
            if (reporter)
                reporter->Report(lineNum, "错误",
                    "diCnt=" + std::to_string(diCnt)
                    + " 超过上限 65534（pt=1 保留给通讯状态），该设备已跳过");
            continue;
        }

        // ---- 查找通道 ----
        // 注：这里不能用 curChId 直接索引，因为可能有多段相同 ID 的 channel
        Channel* pCh = FindCh(curChId);
        if (!pCh)
        {
            // 理论不应发生，但防御性处理
            if (reporter)
                reporter->Report(lineNum, "错误",
                    "内部错误：找不到通道 " + std::to_string(curChId));
            continue;
        }

        // ---- 检查设备重复 ----
        Device* pDev = FindDev(pCh, devNo);
        if (pDev)
        {
            if (reporter)
                reporter->Report(lineNum, "警告",
                    "通道 " + std::to_string(curChId)
                    + " 中设备 " + std::to_string(devNo) + " 重复，已跳过");
            continue;
        }

        // ---- 创建设备 ----
        Device newDev;
        newDev.devNo = devNo;

        // DI
        // 点号 1 固定为通讯状态指示，业务遥信从点号 2 开始
        // 配置文件 diCnt 表示业务遥信数量，总计分配 diCnt+1 个点
        // 注意：diCnt 是 uint16_t，若为 65535，diCnt+1 在 uint16_t 语境下会
        // 回绕到 0，循环一次不执行。这里显式提升到 uint32_t 计算总数。
        const uint32_t diTotal = static_cast<uint32_t>(diCnt) + 1u;
        for (uint32_t i = 1; i <= diTotal; i++)
        {
            DiPoint dp{};
            dp.pointNo  = static_cast<uint16_t>(i);
            dp.value    = false;
            dp.lastVal  = false;
            dp.tsMs     = 0;
            newDev.diList.push_back(dp);
        }

        // AI
        for (uint32_t i = 1; i <= aiCnt; i++)
        {
            AiPoint ap{};
            ap.pointNo = static_cast<uint16_t>(i);
            ap.value   = 0.0;
            ap.lastVal = 0.0;
            newDev.aiList.push_back(ap);
        }

        // DO
        for (uint32_t i = 1; i <= doCnt; i++)
        {
            DoPoint dp{};
            dp.pointNo    = static_cast<uint16_t>(i);
            dp.masterVal  = false;
            dp.slaveVal   = false;
            dp.lastMaster = false;
            dp.lastSlave  = false;
            newDev.doList.push_back(dp);
        }

        // AO
        for (uint32_t i = 1; i <= aoCnt; i++)
        {
            AoPoint ap{};
            ap.pointNo = static_cast<uint16_t>(i);
            ap.value   = 0.0;
            ap.lastVal = 0.0;
            newDev.aoList.push_back(ap);
        }

        pCh->devList.push_back(std::move(newDev));
    }

    fin.close();

    // ---- 统计结果 ----
    size_t totalDevices = 0;
    for (const auto& ch : channels_)
        totalDevices += ch.devList.size();

    std::cout << "[LoadConfig] 加载完成: "
              << channels_.size() << " 通道, "
              << totalDevices << " 设备" << std::endl;

    if (reporter && reporter->HasErrors())
    {
        reporter->PrintSummary();
        reporter->Save(iniPath);
    }

    return !channels_.empty();
}
