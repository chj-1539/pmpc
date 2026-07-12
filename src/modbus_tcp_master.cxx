//=============================================================================
// modbus_tcp_master.cxx — Modbus TCP 主站采集框架实现
//
// 配置解析(global→Channel→Template→Device) + 协议执行 + 模板继承
//=============================================================================

#include "modbus_tcp_master.h"
#include "str_util.h"
#include "packet_logger.h"
#include <chrono>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <fstream>
#include <cstring>
#include <cmath>
#include <cstdarg>
#include <algorithm>

// ─── 常量 ───────────────────────────────────────────────────────────────────
constexpr size_t MBAP_LEN = 7;
constexpr uint8_t EXCEPTION_MASK = 0x80;
constexpr int RECV_BUF_SIZE = 4096;

// ==================== MLogger ====================

std::atomic<int> MLogger::verbosity{0};
int MLogger::hexDump = 0;

void MLogger::Print(Level lv, const char* tag, const std::string& msg)
{
    int v = verbosity.load(std::memory_order_relaxed);
    if (lv == DEBUG && v < 2) return;
    if (lv == INFO && v < 1) return;
    const char* prefix = "";
    switch (lv) {
    case ERROR: prefix = "[ERR]"; break;
    case WARN:  prefix = "[WARN]"; break;
    case INFO:  prefix = "[INFO]"; break;
    case DEBUG: prefix = "[DEBUG]"; break;
    }
    std::cout << prefix << " [" << tag << "] " << msg << std::endl;
}

void MLogger::Printf(Level lv, const char* tag, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int n = std::vsnprintf(nullptr, 0, fmt, args);
    va_end(args);
    if (n < 0) return;
    std::string buf(static_cast<size_t>(n) + 1, '\0');
    va_start(args, fmt);
    std::vsnprintf(buf.data(), buf.size(), fmt, args);
    va_end(args);
    buf.resize(static_cast<size_t>(n));
    Print(lv, tag, buf);
}

void MLogger::HexDump(const char* tag, const uint8_t* data, size_t len)
{
    if (!hexDump) return;
    constexpr size_t PER_LINE = 16;
    std::cout << "[HEX][" << tag << "] (" << len << " bytes)" << std::endl;
    for (size_t i = 0; i < len; i += PER_LINE)
    {
        std::cout << "  ";
        for (size_t j = 0; j < PER_LINE && (i + j) < len; j++)
            std::cout << std::hex << std::setw(2) << std::setfill('0')
                      << static_cast<int>(data[i + j]) << " ";
        std::cout << std::dec << std::setfill(' ') << std::endl;
    }
}

// ==================== EndianConvert ====================

uint64_t EndianConvert::Rearrange(const uint8_t* regs, int /*regCount*/, MEndian endian)
{
    // regs 是按大端排列的寄存器数据 (Modbus 原始数据)
    // regCount: 寄存器数量 (1/2/4)
    uint64_t val = 0;

    switch (endian)
    {
    // 2 字节 (1 个寄存器)
    case MEndian::AB:
        val = (static_cast<uint64_t>(regs[0]) << 8) | regs[1];
        break;
    case MEndian::BA:
        val = (static_cast<uint64_t>(regs[1]) << 8) | regs[0];
        break;

    // 4 字节 (2 个寄存器, regs[0..3])
    case MEndian::ABCD:
        val = (static_cast<uint64_t>(regs[0]) << 24)
            | (static_cast<uint64_t>(regs[1]) << 16)
            | (static_cast<uint64_t>(regs[2]) << 8)
            | regs[3];
        break;
    case MEndian::CDAB:
        val = (static_cast<uint64_t>(regs[2]) << 24)
            | (static_cast<uint64_t>(regs[3]) << 16)
            | (static_cast<uint64_t>(regs[0]) << 8)
            | regs[1];
        break;
    case MEndian::BADC:
        val = (static_cast<uint64_t>(regs[1]) << 24)
            | (static_cast<uint64_t>(regs[0]) << 16)
            | (static_cast<uint64_t>(regs[3]) << 8)
            | regs[2];
        break;
    case MEndian::DCBA:
        val = (static_cast<uint64_t>(regs[3]) << 24)
            | (static_cast<uint64_t>(regs[2]) << 16)
            | (static_cast<uint64_t>(regs[1]) << 8)
            | regs[0];
        break;

    // 8 字节 (4 个寄存器, regs[0..7])
    case MEndian::ABCDEFGH:
        for (int i = 0; i < 8; i++)
            val = (val << 8) | regs[i];
        break;
    case MEndian::GHEFCDAB:
        // high 4 bytes [4-7] swapped, low 4 bytes [0-3] CDAB
        val = (static_cast<uint64_t>(regs[6]) << 56)
            | (static_cast<uint64_t>(regs[7]) << 48)
            | (static_cast<uint64_t>(regs[4]) << 40)
            | (static_cast<uint64_t>(regs[5]) << 32)
            | (static_cast<uint64_t>(regs[2]) << 24)
            | (static_cast<uint64_t>(regs[3]) << 16)
            | (static_cast<uint64_t>(regs[0]) << 8)
            | regs[1];
        break;
    case MEndian::HGFEDCBA:
        for (int i = 7; i >= 0; i--)
            val = (val << 8) | regs[i];
        break;

    default:
        val = (static_cast<uint64_t>(regs[0]) << 8) | regs[1];
        break;
    }
    return val;
}

// ==================== DataConvert ====================

int DataConvert::GetRegCount(MDataType dtype)
{
    switch (dtype)
    {
    case MDataType::Int16:
    case MDataType::UInt16:    return 1;
    case MDataType::Int32:
    case MDataType::UInt32:
    case MDataType::Float:     return 2;
    case MDataType::Int64:
    case MDataType::UInt64:
    case MDataType::Double:    return 4;
    default:                   return 1;
    }
}

double DataConvert::ParseValue(const uint8_t* data, size_t offset,
                               MDataType dtype, MEndian endian)
{
    int regCnt = GetRegCount(dtype);
    uint64_t raw = EndianConvert::Rearrange(data + offset, regCnt, endian);
    double result = 0.0;

    switch (dtype)
    {
    case MDataType::Int16:
        result = static_cast<double>(static_cast<int16_t>(raw & 0xFFFF));
        break;
    case MDataType::UInt16:
        result = static_cast<double>(raw & 0xFFFF);
        break;
    case MDataType::Int32:
        result = static_cast<double>(static_cast<int32_t>(raw & 0xFFFFFFFF));
        break;
    case MDataType::UInt32:
        result = static_cast<double>(raw & 0xFFFFFFFF);
        break;
    case MDataType::Int64:
        result = static_cast<double>(static_cast<int64_t>(raw));
        break;
    case MDataType::UInt64:
        result = static_cast<double>(raw);
        break;
    case MDataType::Float:
    {
        uint32_t tmp = static_cast<uint32_t>(raw & 0xFFFFFFFF);
        float f;
        std::memcpy(&f, &tmp, sizeof(f));
        result = static_cast<double>(f);
        break;
    }
    case MDataType::Double:
    {
        double d;
        std::memcpy(&d, &raw, sizeof(d));
        result = d;
        break;
    }
    default:
        result = static_cast<double>(raw & 0xFFFF);
        break;
    }
    return result;
}

double DataConvert::ApplyScale(double raw, double scale, double offset)
{
    return raw * scale + offset;
}

double DataConvert::GetCommLossValue(MCommLoss loss, double customVal)
{
    switch (loss)
    {
    case MCommLoss::Keep:     return 0.0; // 调用者自行处理保持逻辑
    case MCommLoss::Zero:     return 0.0;
    case MCommLoss::SetMax:   return 3.4e38;
    case MCommLoss::SetMin:   return -3.4e38;
    case MCommLoss::SetCustom: return customVal;
    default:                  return 0.0;
    }
}

bool DataConvert::GetCommLossBool(MCommLoss loss, bool /*customVal*/)
{
    switch (loss)
    {
    case MCommLoss::Keep:     return false; // 调用者自行处理
    case MCommLoss::Zero:     return false;
    case MCommLoss::SetMax:
    case MCommLoss::SetMin:
    case MCommLoss::SetCustom: return false;
    default:                  return false;
    }
}

int AIMapping::regCount() const
{
    return DataConvert::GetRegCount(dtype);
}

// ==================== ModbusTcpMaster ====================

ModbusTcpMaster::ModbusTcpMaster() {}
ModbusTcpMaster::~ModbusTcpMaster() { Stop(); }

// ==================== 配置加载 ====================

static std::string IniSectionName(const std::string& line)
{
    // line = "[SectionName]"
    return Trim(line.substr(1, line.size() - 2));
}

bool ModbusTcpMaster::LoadConfig(const std::string& path)
{
    std::ifstream fin(path);
    if (!fin.is_open())
    {
        MLogger::Print(MLogger::ERROR, "Config", "无法打开: " + path);
        return false;
    }

    config_ = MasterConfig{};
    templates_.clear();

    std::string line, curSection;
    int lineNum = 0;

    // 临时存储: 通道配置、模板配置、设备配置
    std::map<std::string, std::string> globalKv;
    std::map<std::string, std::string> channelKv;
    std::map<std::string, std::string> templateKv;
    std::map<std::string, std::string> deviceKv;
    std::string curSectionType; // "global", "channel", "template", "device", ""

    auto flushSection = [&]() {
        if (curSectionType == "global") {
            for (auto& [k, v] : globalKv) ParseGlobal(k, v);
        }
        else if (curSectionType == "channel" && !channelKv.empty()) {
            ChannelConfig ch;
            for (auto& [k, v] : channelKv) ParseChannel(k, v, ch);
            config_.channels.push_back(ch);
        }
        else if (curSectionType == "template" && !templateKv.empty()) {
            DeviceConfig tmpl;
            for (auto& [k, v] : templateKv)
            {
                if (StartsWith(k, "di_") || StartsWith(k, "ai_") ||
                    StartsWith(k, "do_") || StartsWith(k, "ao_"))
                    ParseTemplateEntry(k, v, tmpl);
            }
            templates_[curSection] = tmpl;
        }
        else if (curSectionType == "device" && !deviceKv.empty()) {
            DeviceConfig dev;
            std::string templateName;
            int chIdx = -1;
            for (auto& [k, v] : deviceKv)
            {
                if (k == "channel") chIdx = SafeStoi(v) - 1; // 1-based→0-based
                else if (k == "station_id") dev.stationId = static_cast<uint8_t>(SafeStoi(v));
                else if (k == "template") templateName = v;
                else if (k == "desc") dev.name = v;
                else if (StartsWith(k, "di_") || StartsWith(k, "ai_") ||
                         StartsWith(k, "do_") || StartsWith(k, "ao_"))
                    ParseDeviceEntry(k, v, dev);
            }
            // 引用模板
            auto it = templates_.find(templateName);
            if (it != templates_.end())
            {
                dev.templateName = templateName;
                MergeTemplate(dev, it->second);
            }
            else if (!templateName.empty())
            {
                MLogger::Print(MLogger::WARN, "Config",
                    "模板未找到: " + templateName + " (设备: " + curSection + ")");
            }
            if (chIdx >= 0 && chIdx < static_cast<int>(config_.channels.size()))
            {
                dev.channel = static_cast<uint8_t>(chIdx + 1);
                config_.channels[chIdx].devices.push_back(dev);
            }
        }
        globalKv.clear(); channelKv.clear(); templateKv.clear(); deviceKv.clear();
    };

    while (std::getline(fin, line))
    {
        lineNum++;
        // 处理换行
        if (!line.empty() && line.back() == '\r') line.pop_back();
        // 去首尾空白
        line = Trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;

        if (line.front() == '[')
        {
            flushSection();
            std::string name = IniSectionName(line);
            curSection = name;
            if (name == "global") curSectionType = "global";
            else if (StartsWith(name, "Channel_") || StartsWith(name, "channel_"))
                curSectionType = "channel";
            else if (StartsWith(name, "Template_") || StartsWith(name, "template_"))
            {
                curSectionType = "template";
                // 模板名 = 去掉 "Template_" 前缀
                curSection = name.substr(9); // "Template_" = 9 chars
            }
            else if (StartsWith(name, "Device_") || StartsWith(name, "device_"))
                curSectionType = "device";
            else
                curSectionType = "";
            continue;
        }

        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = Trim(line.substr(0, eq));
        std::string val = Trim(line.substr(eq + 1));
        if (key.empty()) continue;

        if (curSectionType == "global") globalKv[key] = val;
        else if (curSectionType == "channel") channelKv[key] = val;
        else if (curSectionType == "template") templateKv[key] = val;
        else if (curSectionType == "device") deviceKv[key] = val;
    }
    flushSection();     // 处理最后一个 section
    flushSection();     // 再次调用确保 Device 被刷新（因 flushSection 内无数据时不操作）

    // 二次刷新 Device: flushSection 只处理当前 sectionType 的数据，
    // 如果最后是 device 类型则已处理，否则需要确保所有数据被消费。
    // 上面的第二次调用是安全的因为如果 sectionType 为空则不操作。

    fin.close();

    // 打印摘要
    int devCount = 0;
    for (auto& ch : config_.channels) devCount += static_cast<int>(ch.devices.size());
    MLogger::Print(MLogger::INFO, "Config",
        "加载完成: " + std::to_string(config_.channels.size()) + " 通道, "
        + std::to_string(templates_.size()) + " 模板, "
        + std::to_string(devCount) + " 设备");

    return !config_.channels.empty() || devCount > 0;
}

// ==================== 解析 Global ====================

bool ModbusTcpMaster::ParseGlobal(const std::string& key, const std::string& val)
{
    if (key == "timeout_ms") config_.timeoutMs = SafeStoi(val);
    else if (key == "retry_count") config_.retryCount = SafeStoi(val);
    else if (key == "retry_sleep_ms") config_.retrySleepMs = SafeStoi(val);
    else if (key == "verbose") config_.verbose = SafeStoi(val);
    else if (key == "hex_dump") config_.hexDump = SafeStoi(val);
    else if (key == "comm_loss") {
        auto l = ToLower(val);
        if (l == "保持" || l == "keep") config_.commLoss = MCommLoss::Keep;
        else if (l == "归零" || l == "zero") config_.commLoss = MCommLoss::Zero;
        else if (l == "最大值" || l == "max") config_.commLoss = MCommLoss::SetMax;
        else if (l == "最小值" || l == "min") config_.commLoss = MCommLoss::SetMin;
    }
    else if (key == "max_di_read") config_.maxDiRead = SafeStoi(val, 2000);
    else if (key == "max_ai_read") config_.maxAiRead = SafeStoi(val, 125);
    // 更新日志级别
    MLogger::verbosity.store(config_.verbose, std::memory_order_relaxed);
    MLogger::hexDump = config_.hexDump;
    return true;
}

// ==================== 解析 Channel ====================

bool ModbusTcpMaster::ParseChannel(const std::string& key, const std::string& val,
                                ChannelConfig& ch)
{
    if (key == "ip") ch.ip = val;
    else if (key == "port") ch.port = static_cast<uint16_t>(SafeStoi(val));
    else if (key == "scan_ms") ch.scanMs = SafeStoi(val);
    else if (key == "timeout_ms") ch.timeoutMs = SafeStoi(val);
    else if (key == "retry_count") ch.retryCount = SafeStoi(val);
    else if (key == "retry_sleep_ms") ch.retrySleepMs = SafeStoi(val);
    else if (key == "verbose") ch.verbose = SafeStoi(val);
    else if (key == "hex_dump") ch.hexDump = SafeStoi(val);
    else if (key == "standby_ip") ch.standbyIp = val;
    else if (key == "standby_port") ch.standbyPort = SafeStoi(val);
    else if (key == "fallback") ch.fallback = SafeStoi(val, 1);
    else if (key == "max_di_read") ch.maxDiRead = SafeStoi(val);
    else if (key == "max_ai_read") ch.maxAiRead = SafeStoi(val);
    else if (key == "keep_alive_addr") ch.keepAliveAddr = SafeStoi(val);
    // 继承全局
    if (ch.timeoutMs <= 0) ch.timeoutMs = config_.timeoutMs;
    if (ch.retryCount <= 0) ch.retryCount = config_.retryCount;
    if (ch.retrySleepMs <= 0) ch.retrySleepMs = config_.retrySleepMs;
    if (ch.verbose == 0 && config_.verbose > 0) ch.verbose = config_.verbose;
    if (ch.hexDump == 0 && config_.hexDump > 0) ch.hexDump = config_.hexDump;
    if (ch.maxDiRead <= 0) ch.maxDiRead = config_.maxDiRead;
    if (ch.maxAiRead <= 0) ch.maxAiRead = config_.maxAiRead;
    return true;
}

// ==================== 解析模板条目 ====================

bool ModbusTcpMaster::ParseTemplateEntry(const std::string& prefix,
                                      const std::string& params,
                                      DeviceConfig& tmpl)
{
    auto kv = ParseKeyValues(params);
    if (kv.empty()) return false;

    if (StartsWith(prefix, "di_"))
    {
        DIMapping di;
        if (ParseDIMapping(kv, di)) tmpl.diList.push_back(di);
    }
    else if (StartsWith(prefix, "ai_"))
    {
        AIMapping ai;
        if (ParseAIMapping(kv, ai)) tmpl.aiList.push_back(ai);
    }
    else if (StartsWith(prefix, "do_"))
    {
        DOMapping do_;
        if (ParseDOMapping(kv, do_)) tmpl.doList.push_back(do_);
    }
    else if (StartsWith(prefix, "ao_"))
    {
        AOMapping ao;
        if (ParseAOMapping(kv, ao)) tmpl.aoList.push_back(ao);
    }
    return true;
}

bool ModbusTcpMaster::ParseDeviceEntry(const std::string& prefix,
                                    const std::string& params,
                                    DeviceConfig& dev)
{
    // 同 ParseTemplateEntry，只是条目追加到 dev
    return ParseTemplateEntry(prefix, params, dev);
}

// ==================== 解析映射条目 ====================

static MDataType ParseDataType(const std::string& s)
{
    auto l = ToLower(s);
    if (l == "int16" || l == "int16_t") return MDataType::Int16;
    if (l == "uint16" || l == "uint16_t" || l == "word") return MDataType::UInt16;
    if (l == "int32" || l == "int32_t" || l == "dint") return MDataType::Int32;
    if (l == "uint32" || l == "uint32_t" || l == "dword") return MDataType::UInt32;
    if (l == "int64" || l == "int64_t" || l == "lint") return MDataType::Int64;
    if (l == "uint64" || l == "uint64_t" || l == "ulint") return MDataType::UInt64;
    if (l == "float" || l == "real") return MDataType::Float;
    if (l == "double" || l == "lreal") return MDataType::Double;
    return MDataType::UInt16;
}

static MEndian ParseEndian(const std::string& s)
{
    auto l = ToLower(s);
    if (l == "ab") return MEndian::AB;
    if (l == "ba") return MEndian::BA;
    if (l == "abcd") return MEndian::ABCD;
    if (l == "cdab") return MEndian::CDAB;
    if (l == "badc") return MEndian::BADC;
    if (l == "dcba") return MEndian::DCBA;
    if (l == "abcdefgh") return MEndian::ABCDEFGH;
    if (l == "ghefcdab") return MEndian::GHEFCDAB;
    if (l == "hgfedcba") return MEndian::HGFEDCBA;
    return MEndian::AB;
}

static MCommLoss ParseCommLoss(const std::string& s)
{
    auto l = ToLower(s);
    if (l == "保持" || l == "keep") return MCommLoss::Keep;
    if (l == "归零" || l == "zero" || l == "0") return MCommLoss::Zero;
    if (l == "最大值" || l == "max") return MCommLoss::SetMax;
    if (l == "最小值" || l == "min") return MCommLoss::SetMin;
    if (l == "自定义" || l == "custom") return MCommLoss::SetCustom;
    return MCommLoss::Keep;
}

bool ModbusTcpMaster::ParseDIMapping(
    const std::vector<std::pair<std::string, std::string>>& kv, DIMapping& di)
{
    for (auto& [k, v] : kv)
    {
        if (k == "addr") di.addr = static_cast<uint16_t>(SafeStoi(v));
        else if (k == "func") di.func = static_cast<uint8_t>(SafeStoi(v));
        else if (k == "qty") di.qty = static_cast<uint16_t>(SafeStoi(v));
        else if (k == "bit") di.bit = SafeStoi(v);
        else if (k == "point") di.point = static_cast<uint16_t>(SafeStoi(v));
        else if (k == "invert") di.invert = (v == "1" || v == "true" || v == "yes");
        else if (k == "comm_loss") di.commLoss = ParseCommLoss(v);
    }
    return true;
}

bool ModbusTcpMaster::ParseAIMapping(
    const std::vector<std::pair<std::string, std::string>>& kv, AIMapping& ai)
{
    for (auto& [k, v] : kv)
    {
        if (k == "addr") ai.addr = static_cast<uint16_t>(SafeStoi(v));
        else if (k == "func") ai.func = static_cast<uint8_t>(SafeStoi(v));
        else if (k == "point") ai.point = static_cast<uint16_t>(SafeStoi(v));
        else if (k == "dtype") ai.dtype = ParseDataType(v);
        else if (k == "endian") ai.endian = ParseEndian(v);
        else if (k == "scale") ai.scale = SafeStod(v);
        else if (k == "offset") ai.offset = SafeStod(v);
        else if (k == "comm_loss") ai.commLoss = ParseCommLoss(v);
        else if (k == "comm_loss_val") ai.commLossVal = SafeStod(v);
    }
    return true;
}

bool ModbusTcpMaster::ParseDOMapping(
    const std::vector<std::pair<std::string, std::string>>& kv, DOMapping& do_)
{
    for (auto& [k, v] : kv)
    {
        if (k == "addr") do_.addr = static_cast<uint16_t>(SafeStoi(v));
        else if (k == "func") do_.func = static_cast<uint8_t>(SafeStoi(v));
        else if (k == "point") do_.point = static_cast<uint16_t>(SafeStoi(v));
        else if (k == "invert") do_.invert = (v == "1" || v == "true" || v == "yes");
        else if (k == "pulse_ms") do_.pulseMs = SafeStoi(v);
    }
    return true;
}

bool ModbusTcpMaster::ParseAOMapping(
    const std::vector<std::pair<std::string, std::string>>& kv, AOMapping& ao)
{
    for (auto& [k, v] : kv)
    {
        if (k == "addr") ao.addr = static_cast<uint16_t>(SafeStoi(v));
        else if (k == "func") ao.func = static_cast<uint8_t>(SafeStoi(v));
        else if (k == "point") ao.point = static_cast<uint16_t>(SafeStoi(v));
        else if (k == "dtype") ao.dtype = ParseDataType(v);
        else if (k == "endian") ao.endian = ParseEndian(v);
        else if (k == "scale") ao.scale = SafeStod(v);
        else if (k == "offset") ao.offset = SafeStod(v);
        else if (k == "pulse_ms") ao.pulseMs = SafeStoi(v);
    }
    return true;
}

// ==================== 模板合并 ====================

void ModbusTcpMaster::MergeTemplate(DeviceConfig& dev, const DeviceConfig& tmpl)
{
    // 模板的条目在前，设备自定义的条目在后（追加）
    dev.diList.insert(dev.diList.begin(), tmpl.diList.begin(), tmpl.diList.end());
    dev.aiList.insert(dev.aiList.begin(), tmpl.aiList.begin(), tmpl.aiList.end());
    dev.doList.insert(dev.doList.begin(), tmpl.doList.begin(), tmpl.doList.end());
    dev.aoList.insert(dev.aoList.begin(), tmpl.aoList.begin(), tmpl.aoList.end());
}

// ==================== 启停 ====================

bool ModbusTcpMaster::Start()
{
    if (running_) return false;
    if (config_.channels.empty())
    {
        MLogger::Print(MLogger::ERROR, "Master", "无通道配置");
        return false;
    }

    MLogger::verbosity.store(config_.verbose, std::memory_order_relaxed);
    MLogger::hexDump = config_.hexDump;

    running_ = true;
    threads_.reserve(config_.channels.size());
    for (size_t i = 0; i < config_.channels.size(); i++)
    {
        threads_.emplace_back(&ModbusTcpMaster::ChannelThread, this, static_cast<int>(i));
        char buf[64];
        std::snprintf(buf, sizeof(buf), "通道%d (%s:%d)", (int)(i+1),
                      config_.channels[i].ip.c_str(), config_.channels[i].port);
        std::cout << "[INFO] [Master] 启动" << buf << std::endl;
    }
    return true;
}

void ModbusTcpMaster::Stop()
{
    running_ = false;
    for (auto& t : threads_)
        if (t.joinable()) t.join();
    threads_.clear();

    // 清除发送跟踪和脉冲队列，确保热重载后重新计算
    // （ReloadModule 流程为 Stop → LoadConfig → Start）
    {
        std::lock_guard<std::mutex> lock(sentMtx_);
        doSent_.clear();
        aoSent_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(pulseMtx_);
        pulseQueue_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(commMtx_);
        commStatus_.clear();
    }
}

// ==================== 通道线程 ====================

void ModbusTcpMaster::ChannelThread(int chIdx)
{
    if (chIdx < 0 || chIdx >= static_cast<int>(config_.channels.size())) return;
    ChannelConfig& ch = config_.channels[chIdx];
    int scanMs = ch.scanMs > 0 ? ch.scanMs : 3000;

    char tag[32];
    std::snprintf(tag, sizeof(tag), "Ch%d", chIdx + 1);

    // 强制使用 cout 确保输出不被 MLogger 的 verbosity 过滤
    std::cout << "[INFO] [" << tag << "] 线程启动: " << ch.ip << ":" << ch.port
              << " 备用=" << (ch.standbyIp.empty() ? "无" : ch.standbyIp + ":" + std::to_string(ch.standbyPort))
              << " 扫描=" << scanMs << "ms"
              << " 设备数=" << ch.devices.size() << std::endl;

    // ── 双连接冗余 ──
    struct Endpoint { std::string ip; int port; };
    Endpoint endpoints[] = {{ch.ip, ch.port}, {ch.standbyIp, ch.standbyPort > 0 ? ch.standbyPort : ch.port}};
    int currentEp = 0;

    while (running_)
    {
        socket sock;
        bool connected = false;

        // ── 遍历 endpoints 连接（从 currentEp 开始） ──
        int tried = 0;
        while (running_ && !connected && tried < 2) {
            int idx = (currentEp + tried) % 2;
            tried++;
            if (endpoints[idx].port == 0) continue;
            if ((idx == 1) && ch.standbyIp.empty()) continue;

            int retryLeft = ch.retryCount;
            while (running_ && !connected && retryLeft > 0) {
                try {
                    sock.connect(endpoints[idx].ip, static_cast<uint16_t>(endpoints[idx].port));
                    sock.set_recv_timeout(std::chrono::milliseconds(ch.timeoutMs));
                    connected = true;
                    currentEp = idx;
                    std::cout << "[INFO] [" << tag << "] 已连接 " << endpoints[idx].ip << ":" << endpoints[idx].port
                              << (idx == 1 ? " (备用)" : "") << std::endl;

                    for (auto& dev : ch.devices) {
                        std::string key = std::to_string(dev.channel) + "_" + std::to_string(dev.stationId);
                        { std::lock_guard<std::mutex> lock(commMtx_); commStatus_[key] = true; }
                        auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count();
                        RemoteDataMgr::Instance().SetDi(dev.channel, dev.stationId, 1, true, ts, false);

                        // 预填充 doSent_/aoSent_，避免首次连接时发出意外初始写回
                        // 注意：使用 emplace 不覆盖已有条目，确保断线重连期间变化的 DO/AO 能被重新发送
                        for (auto& do_ : dev.doList)
                        {
                            DoPoint pt;
                            if (RemoteDataMgr::Instance().GetDo(dev.channel, dev.stationId, do_.point, pt))
                            {
                                bool val = (do_.invert) ? !pt.masterVal : pt.masterVal;
                                std::string sk = key + "_" + std::to_string(do_.point);
                                { std::lock_guard<std::mutex> lock(sentMtx_); doSent_.emplace(sk, val); }
                            }
                        }
                        for (auto& ao : dev.aoList)
                        {
                            AoPoint pt;
                            if (RemoteDataMgr::Instance().GetAo(dev.channel, dev.stationId, ao.point, pt))
                            {
                                std::string sk = key + "_" + std::to_string(ao.point);
                                { std::lock_guard<std::mutex> lock(sentMtx_); aoSent_.emplace(sk, pt.value); }
                            }
                        }
                    }
                    retryLeft = ch.retryCount;
                } catch (const std::exception& e) {
                    retryLeft--;
                    std::cout << "[WARN] [" << tag << "] " << endpoints[idx].ip << ":" << endpoints[idx].port
                              << " 连接失败 (" << (ch.retryCount - retryLeft) << "/" << ch.retryCount
                              << "): " << e.what() << std::endl;
                    if (retryLeft > 0) {
                        for (int i = 0; i < 50 && running_; i++) std::this_thread::sleep_for(std::chrono::milliseconds(200));
                    }
                }
            }
        }

        if (!connected) {
            // 全部 endpoint 失败，标记离线 + 休眠
            for (auto& dev : ch.devices) {
                std::string key = std::to_string(dev.channel) + "_" + std::to_string(dev.stationId);
                { std::lock_guard<std::mutex> lock(commMtx_); commStatus_[key] = false; }
                RemoteDataMgr::Instance().SetDi(dev.channel, dev.stationId, 1, false, 0, false);
            }
            int sleepMs = ch.retrySleepMs > 0 ? ch.retrySleepMs : 10000;
            std::cout << "[WARN] [" << tag << "] 全部 endpoint 失败，休眠 " << sleepMs << "ms" << std::endl;
            for (int i = 0; i < sleepMs / 100 && running_; i++)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        // ── 采集循环 ──
        uint16_t transId = 0;
        while (running_ && connected)
        {
            try {
                // 遍历设备的采集任务
                for (auto& dev : ch.devices)
                {
                    if (!running_) break;
                    PollDevice(sock, dev, ch.maxDiRead, ch.maxAiRead, ch.keepAliveAddr);
                }

                // DO/AO 回写
                for (auto& dev : ch.devices)
                {
                    if (!running_) break;
                    WriteDOChanges(sock, transId, dev);
                    WriteAOChanges(sock, transId, dev);
                }

                // DO/AO 脉冲复位（到期自动发送复位报文）
                ProcessPulseQueue(sock, transId);

                // 更新通讯状态
                for (auto& dev : ch.devices)
                {
                    std::string key = std::to_string(dev.channel) + "_"
                                    + std::to_string(dev.stationId);
                    bool wasOffline = false;
                    {
                        std::lock_guard<std::mutex> lock(commMtx_);
                        auto it = commStatus_.find(key);
                        if (it != commStatus_.end() && !it->second)
                        {
                            it->second = true;
                            wasOffline = true;
                        }
                    }
                    if (wasOffline) {
                        auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count();
                        RemoteDataMgr::Instance().SetDi(dev.channel, dev.stationId, 1, true, ts, false);
                        std::cout << "[INFO] [" << tag << "] 设备 " << static_cast<int>(dev.stationId) << " 恢复通讯" << std::endl;
                    }
                }
            }
            catch (const socket_error& e) {
                std::cout << "[WARN] [" << tag << "] 通讯中断: " << e.what() << std::endl;
                connected = false;

                // 标记设备离线
                for (auto& dev : ch.devices) {
                    std::string key = std::to_string(dev.channel) + "_" + std::to_string(dev.stationId);
                    { std::lock_guard<std::mutex> lock(commMtx_); commStatus_[key] = false; }
                    RemoteDataMgr::Instance().SetDi(dev.channel, dev.stationId, 1, false, 0, false);
                }
                // fallback: 备线断后尝试回切主
                if (ch.fallback && currentEp != 0) currentEp = 0;
                break;
            }

            // 扫描间隔
            if (connected && running_)
            {
                for (int i = 0; i < scanMs / 100 && running_; i++)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }

        // 断开后自动重连
        try { sock.close(); } catch (...) {}
        std::cout << "[WARN] [" << tag << "] 断开，准备重连..." << std::endl;
    }

    std::cout << "[INFO] [" << tag << "] 线程退出" << std::endl;
}

// ==================== 设备采集 ====================

void ModbusTcpMaster::PollDevice(socket& sock, const DeviceConfig& dev,
                                 int maxDiRead, int maxAiRead, int keepAliveAddr)
{
    uint16_t transId = 0;

    // ── 收集所有读请求 ──
    // DI: 按 func 分组
    std::map<uint8_t, std::vector<size_t>> diByFunc;
    for (size_t i = 0; i < dev.diList.size(); i++)
        diByFunc[dev.diList[i].func].push_back(i);

    // AI: 按 func 分组
    std::map<uint8_t, std::vector<size_t>> aiByFunc;
    for (size_t i = 0; i < dev.aiList.size(); i++)
        aiByFunc[dev.aiList[i].func].push_back(i);

    // ── FC01/02 读取（DI 线圈） ──
    // 不检查地址连续性；在 maxDiRead 范围内合并为一包
    for (auto& [func, indices] : diByFunc)
    {
        if (func != 1 && func != 2) continue;
        if (indices.empty()) continue;

        // 按地址排序
        std::sort(indices.begin(), indices.end(),
                  [&](size_t a, size_t b) { return dev.diList[a].addr < dev.diList[b].addr; });

        size_t start = 0;
        while (start < indices.size())
        {
            uint16_t groupStart = dev.diList[indices[start]].addr;
            uint16_t groupEnd   = static_cast<uint16_t>(groupStart + dev.diList[indices[start]].qty - 1);
            size_t end = start;

            for (size_t i = start + 1; i < indices.size(); i++)
            {
                auto& di = dev.diList[indices[i]];
                uint16_t diEnd = static_cast<uint16_t>(di.addr + di.qty - 1);
                uint16_t newEnd = (diEnd > groupEnd) ? diEnd : groupEnd;
                // 仅检查是否超出最大读取长度，不检查地址连续性
                if (static_cast<uint16_t>(newEnd - groupStart + 1) > static_cast<uint16_t>(maxDiRead))
                    break;
                groupEnd = newEnd;
                end = i;
            }

            // 收集本组 DI 索引
            std::vector<size_t> groupIndices;
            for (size_t i = start; i <= end; i++)
                groupIndices.push_back(indices[i]);

            transId++;
            ReadAndDispatch(sock, transId, dev, func, groupIndices, {});

            start = end + 1;
        }
    }

    // ── FC03/04 读取（AI 寄存器 + DI 按位） ──
    // 不检查地址连续性；在 maxAiRead 范围内合并为一包
    struct FC34Entry {
        uint16_t addr;
        int regs;
        bool isDI;
        size_t listIdx;
    };

    for (auto func : {3, 4})
    {
        std::vector<FC34Entry> entries;

        auto aiIt = aiByFunc.find(static_cast<uint8_t>(func));
        if (aiIt != aiByFunc.end())
            for (size_t idx : aiIt->second)
                entries.push_back({dev.aiList[idx].addr,
                                   dev.aiList[idx].regCount(), false, idx});

        auto diIt = diByFunc.find(static_cast<uint8_t>(func));
        if (diIt != diByFunc.end())
            for (size_t idx : diIt->second)
                entries.push_back({dev.diList[idx].addr, 1, true, idx});

        if (entries.empty()) continue;

        // 按地址排序
        std::sort(entries.begin(), entries.end(),
                  [](const FC34Entry& a, const FC34Entry& b) {
                      return a.addr < b.addr;
                  });

        // 按 maxAiRead 长度合并，不检查地址连续性
        size_t start = 0;
        while (start < entries.size())
        {
            uint16_t blockStart = entries[start].addr;
            uint16_t blockEnd   = static_cast<uint16_t>(entries[start].addr + entries[start].regs - 1);
            size_t end = start;

            for (size_t i = start + 1; i < entries.size(); i++)
            {
                uint16_t eAddr  = entries[i].addr;
                uint16_t eEnd   = static_cast<uint16_t>(eAddr + entries[i].regs - 1);
                uint16_t newEnd = (eEnd > blockEnd) ? eEnd : blockEnd;
                // 仅检查是否超出最大读取长度，不检查地址连续性
                if (static_cast<uint16_t>(newEnd - blockStart + 1) > static_cast<uint16_t>(maxAiRead))
                    break;
                blockEnd = newEnd;
                end = i;
            }

            uint16_t qty = static_cast<uint16_t>(blockEnd - blockStart + 1);

            std::vector<size_t> diIdxs, aiIdxs;
            for (size_t i = start; i <= end; i++)
            {
                if (entries[i].isDI) diIdxs.push_back(entries[i].listIdx);
                else                 aiIdxs.push_back(entries[i].listIdx);
            }

            transId++;
            ReadAndDispatch(sock, transId, dev,
                           static_cast<uint8_t>(func),
                           diIdxs, aiIdxs, blockStart, qty);

            start = end + 1;
        }
    }

    // ── Keep-alive：仅 DO 无 DI/AI 时发送连接测试帧 ──
    // 目的：检测 TCP 静默断开，确保 DI 点号 1 通讯状态准确
    if (keepAliveAddr >= 0 && dev.diList.empty() && dev.aiList.empty())
    {
        uint8_t pdu[] = {0x03,
            static_cast<uint8_t>((keepAliveAddr >> 8) & 0xFF),
            static_cast<uint8_t>(keepAliveAddr & 0xFF),
            0x00, 0x01};
        transId++;
        if (BuildAndSend(sock, transId, dev.stationId, pdu, sizeof(pdu)))
        {
            uint8_t resp[256];
            size_t respLen = 0;
            RecvFrame(sock, resp, sizeof(resp), transId, respLen);
        }
        else
        {
            throw socket_error(socket_errc::send_failed, "keep-alive send failed");
        }
    }
}

// ==================== 批量读取与分发 ====================

bool ModbusTcpMaster::ReadAndDispatch(socket& sock, uint16_t transId,
                                   const DeviceConfig& dev,
                                   uint8_t devFunc,
                                   const std::vector<size_t>& diIndices,
                                   const std::vector<size_t>& /*aiIndices*/)
{
    // 如果只有 AI 且无 DI 的 FC03/04, 或只有 DI 的 FC01/02
    // 需要计算实际读范围
    if (devFunc == 1 || devFunc == 2)
    {
        if (diIndices.empty()) return true;
        uint16_t minAddr = 0xFFFF, maxAddr = 0;
        uint16_t maxQty = 0;
        for (auto idx : diIndices)
        {
            auto& di = dev.diList[idx];
            if (di.addr < minAddr) minAddr = di.addr;
            uint16_t end = static_cast<uint16_t>(di.addr + di.qty - 1);
            if (end > maxAddr) { maxAddr = end; maxQty = di.qty; }
        }
        if (minAddr == 0xFFFF) return true;
        uint16_t qty = static_cast<uint16_t>(maxAddr - minAddr + 1);
        if (qty < maxQty) qty = maxQty;

        // 发送请求
        uint8_t pdu[] = {
            devFunc,
            static_cast<uint8_t>((minAddr >> 8) & 0xFF),
            static_cast<uint8_t>(minAddr & 0xFF),
            static_cast<uint8_t>((qty >> 8) & 0xFF),
            static_cast<uint8_t>(qty & 0xFF)
        };
        if (!BuildAndSend(sock, transId, dev.stationId, pdu, sizeof(pdu)))
            return false;

        {
            PacketLogger::Instance().Log(PktDir::TX, dev.channel, dev.stationId,
                dev.stationId, devFunc, transId, pdu, sizeof(pdu));
        }

        uint8_t resp[RECV_BUF_SIZE];
        size_t respLen = 0;
        if (!RecvFrame(sock, resp, sizeof(resp), transId, respLen))
            return false;

        {
            PacketLogger::Instance().Log(PktDir::RX, dev.channel, dev.stationId,
                dev.stationId, devFunc, transId,
                resp + MBAP_LEN, respLen - MBAP_LEN);
        }

        if (resp[MBAP_LEN] & EXCEPTION_MASK)
        {
            MLogger::Print(MLogger::WARN, "Modbus",
                "FC" + std::to_string((int)devFunc) + " 异常码: 0x"
                + std::to_string(resp[MBAP_LEN + 1]));
            return false;
        }

        MLogger::Print(MLogger::INFO, "Modbus",
            "FC" + std::to_string((int)devFunc) + " station="
            + std::to_string(dev.stationId) + " addr=" + std::to_string(minAddr)
            + " qty=" + std::to_string(qty) + " 成功");

        // 分发到各 DI 条目
        uint8_t byteCount = resp[MBAP_LEN + 1];
        auto& mgr = RemoteDataMgr::Instance();
        uint64_t nowTs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        for (auto idx : diIndices)
        {
            auto& di = dev.diList[idx];
            uint16_t relAddr = static_cast<uint16_t>(di.addr - minAddr);

            for (uint16_t j = 0; j < di.qty; j++)
            {
                uint16_t byteIdx = static_cast<uint16_t>((relAddr + j) / 8);
                uint8_t bitIdx = static_cast<uint8_t>((relAddr + j) % 8);
                bool val = false;
                if (byteIdx < byteCount)
                    val = (resp[MBAP_LEN + 2 + byteIdx] >> bitIdx) & 1;

                if (di.invert) val = !val;
                mgr.SetDi(dev.channel, dev.stationId,
                          static_cast<uint16_t>(di.point + j),
                          val, nowTs, true);
            }
        }
        return true;
    }

    // FC03/04 由上层 ReadAndDispatch 重载处理
    return true;
}

// 重载：指定地址范围的 FC03/04 读取
bool ModbusTcpMaster::ReadAndDispatch(socket& sock, uint16_t transId,
                                   const DeviceConfig& dev,
                                   uint8_t devFunc,
                                   const std::vector<size_t>& diIndices,
                                   const std::vector<size_t>& aiIndices,
                                   uint16_t minAddr, uint16_t qty)
{
    if (diIndices.empty() && aiIndices.empty()) return true;
    if (qty == 0) return true;

    uint8_t pdu[] = {
        devFunc,
        static_cast<uint8_t>((minAddr >> 8) & 0xFF),
        static_cast<uint8_t>(minAddr & 0xFF),
        static_cast<uint8_t>((qty >> 8) & 0xFF),
        static_cast<uint8_t>(qty & 0xFF)
    };

    if (!BuildAndSend(sock, transId, dev.stationId, pdu, sizeof(pdu)))
        return false;

    {
        PacketLogger::Instance().Log(PktDir::TX, dev.channel, dev.stationId,
            dev.stationId, devFunc, transId, pdu, sizeof(pdu));
    }

    uint8_t resp[RECV_BUF_SIZE];
    size_t respLen = 0;
    if (!RecvFrame(sock, resp, sizeof(resp), transId, respLen))
        return false;

    {
        PacketLogger::Instance().Log(PktDir::RX, dev.channel, dev.stationId,
            dev.stationId, devFunc, transId,
            resp + MBAP_LEN, respLen - MBAP_LEN);
    }

    if (resp[MBAP_LEN] & EXCEPTION_MASK)
    {
        MLogger::Print(MLogger::WARN, "Modbus",
            "FC" + std::to_string((int)devFunc) + " 异常码: 0x"
            + std::to_string(resp[MBAP_LEN + 1]));
        return false;
    }

    MLogger::Print(MLogger::INFO, "Modbus",
        "FC" + std::to_string((int)devFunc) + " station="
        + std::to_string(dev.stationId) + " addr=" + std::to_string(minAddr)
        + " qty=" + std::to_string(qty) + " 成功");

    uint8_t byteCount = resp[MBAP_LEN + 1];
    uint8_t* rawData = resp + MBAP_LEN + 2; // 跳过 FC + byteCount
    auto& mgr = RemoteDataMgr::Instance();
    uint64_t nowTs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    // ── 分发 DI (FC03/04 按位) ──
    for (auto idx : diIndices)
    {
        auto& di = dev.diList[idx];
        if (di.bit < 0 || di.bit > 15) continue;
        uint16_t regOffset = static_cast<uint16_t>((di.addr - minAddr) * 2);
        if (regOffset + 1 >= byteCount) continue;

        uint16_t regVal = static_cast<uint16_t>(
            (rawData[regOffset] << 8) | rawData[regOffset + 1]);
        bool val = (regVal >> di.bit) & 1;
        if (di.invert) val = !val;
        mgr.SetDi(dev.channel, dev.stationId, di.point, val, nowTs, true);
    }

    // ── 分发 AI ──
    for (auto idx : aiIndices)
    {
        auto& ai = dev.aiList[idx];
        uint16_t byteOffset = static_cast<uint16_t>((ai.addr - minAddr) * 2);
        if (byteOffset + ai.regCount() * 2 > byteCount) continue;

        double rawVal = DataConvert::ParseValue(
            rawData, byteOffset, ai.dtype, ai.endian);
        double engVal = DataConvert::ApplyScale(rawVal, ai.scale, ai.offset);
        mgr.SetAi(dev.channel, dev.stationId, ai.point, engVal);
    }

    return true;
}

// ==================== DO/AO 脉冲复位 ====================

void ModbusTcpMaster::ProcessPulseQueue(socket& sock, uint16_t& transId)
{
    if (pulseQueue_.empty()) return;
    uint64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    std::lock_guard<std::mutex> lock(pulseMtx_);
    for (size_t i = 0; i < pulseQueue_.size(); )
    {
        auto& pe = pulseQueue_[i];
        if (now < pe.readyAtMs) { i++; continue; }

        // 到期，发送复位报文
        uint8_t pdu[5];
        pdu[0] = pe.func;
        pdu[1] = static_cast<uint8_t>((pe.addr >> 8) & 0xFF);
        pdu[2] = static_cast<uint8_t>(pe.addr & 0xFF);
        pdu[3] = pe.valHi;
        pdu[4] = pe.valLo;
        size_t pduLen = 5;

        transId++;
        if (BuildAndSend(sock, transId, pe.stationId, pdu, pduLen))
        {
            uint8_t resp[256];
            size_t respLen = 0;
            if (RecvFrame(sock, resp, sizeof(resp), transId, respLen) &&
                !(resp[MBAP_LEN] & EXCEPTION_MASK))
            {
                MLogger::Print(MLogger::INFO, "Modbus",
                    "Pulse reset FC" + std::to_string(pe.func)
                    + " station=" + std::to_string(pe.stationId)
                    + " addr=" + std::to_string(pe.addr));
            }
        }
        pulseQueue_.erase(pulseQueue_.begin() + i);
        // i stays same (next element shifts here)
    }
}

// ==================== DO/AO 回写 ====================

void ModbusTcpMaster::WriteDOChanges(socket& sock, uint16_t& transId,
                                  const DeviceConfig& dev)
{
    if (dev.doList.empty()) return;
    auto& mgr = RemoteDataMgr::Instance();

    for (auto& do_ : dev.doList)
    {
        DoPoint pt;
        if (!mgr.GetDo(dev.channel, dev.stationId, do_.point, pt))
            continue;

        bool current = (do_.invert) ? !pt.masterVal : pt.masterVal;

        // 使用模块级 doSent_ 跟踪上次已发送值（不可依赖 DoPoint::lastMaster，
        // 因 CheckAllPointChange 每 200ms 将其同步为 masterVal）
        std::string key = std::to_string(dev.channel) + "_"
                        + std::to_string(dev.stationId) + "_"
                        + std::to_string(do_.point);
        {
            std::lock_guard<std::mutex> lock(sentMtx_);
            auto it = doSent_.find(key);
            if (it != doSent_.end() && it->second == current)
                continue;
        }

        // 写入 Modbus
        transId++;
        if (do_.func == 5)
        {
            uint8_t pdu[] = {
                MFunc::WRITE_SINGLE_COIL,
                static_cast<uint8_t>((do_.addr >> 8) & 0xFF),
                static_cast<uint8_t>(do_.addr & 0xFF),
                current ? static_cast<uint8_t>(0xFF) : static_cast<uint8_t>(0x00),
                0x00
            };
            if (!BuildAndSend(sock, transId, dev.stationId, pdu, sizeof(pdu)))
                continue;

            {
                PacketLogger::Instance().Log(PktDir::TX, dev.channel, dev.stationId,
                    dev.stationId, 0x05, transId, pdu, sizeof(pdu));
            }

            uint8_t resp[256];
            size_t respLen = 0;
            if (!RecvFrame(sock, resp, sizeof(resp), transId, respLen))
                continue;

            if (resp[MBAP_LEN] & EXCEPTION_MASK)
                continue;

            {
                PacketLogger::Instance().Log(PktDir::RX, dev.channel, dev.stationId,
                    dev.stationId, 0x05, transId,
                    resp + MBAP_LEN, respLen - MBAP_LEN);
            }

            MLogger::Print(MLogger::INFO, "Modbus",
                "FC05 station=" + std::to_string(dev.stationId)
                + " addr=" + std::to_string(do_.addr) + " val=" + std::to_string(current));

            // 记录已成功发送的值，避免下一轮重复发送
            {
                std::lock_guard<std::mutex> lock(sentMtx_);
                doSent_[key] = current;
            }

            // 遥控 ON 执行完成后立即复位 masterVal，使下一次遥控可再次触发
            // 物理设备自动复位或 pulseMs 定时器负责恢复线圈状态
            if (current)
            {
                RemoteDataMgr::Instance().SetDoMaster(
                    dev.channel, dev.stationId, do_.point, false);
                {
                    std::lock_guard<std::mutex> lock(sentMtx_);
                    doSent_[key] = false;
                }
            }

            // 脉冲复位：pulseMs>0 时，到期自动写相反值
            if (do_.pulseMs > 0)
            {
                uint64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                PulseEntry pe;
                pe.channel   = dev.channel;
                pe.stationId = dev.stationId;
                pe.func      = 5;
                pe.addr      = do_.addr;
                pe.valHi     = current ? 0x00 : 0xFF;  // opposite
                pe.valLo     = 0x00;
                pe.readyAtMs = now + do_.pulseMs;
                {
                    std::lock_guard<std::mutex> lock(pulseMtx_);
                    pulseQueue_.push_back(pe);
                }
                MLogger::Print(MLogger::INFO, "Modbus",
                    "FC05 pulse scheduled: station=" + std::to_string(dev.stationId)
                    + " addr=" + std::to_string(do_.addr)
                    + " reset=" + std::to_string(!current)
                    + " after=" + std::to_string(do_.pulseMs) + "ms");
            }
        }
        // FC15 写多线圈暂简化为 FC05
    }
}

void ModbusTcpMaster::WriteAOChanges(socket& sock, uint16_t& transId,
                                  const DeviceConfig& dev)
{
    if (dev.aoList.empty()) return;
    auto& mgr = RemoteDataMgr::Instance();

    for (auto& ao : dev.aoList)
    {
        AoPoint pt;
        if (!mgr.GetAo(dev.channel, dev.stationId, ao.point, pt))
            continue;

        // 使用模块级 aoSent_ 跟踪上次已发送值（不可依赖 AoPoint::lastVal，
        // 因 CheckAllPointChange 每 200ms 将其同步为 value）
        std::string key = std::to_string(dev.channel) + "_"
                        + std::to_string(dev.stationId) + "_"
                        + std::to_string(ao.point);
        {
            std::lock_guard<std::mutex> lock(sentMtx_);
            auto it = aoSent_.find(key);
            // H9 修复：改用相对+绝对容差，见 ModbusTcpMaster::AoAlmostEqual。
            if (it != aoSent_.end() && AoAlmostEqual(it->second, pt.value))
                continue;
        }

        // 单寄存器写入
        if (ao.func == 6 && ao.dtype == MDataType::UInt16)
        {
            uint16_t regVal = static_cast<uint16_t>(std::round(pt.value));
            uint8_t pdu[] = {
                MFunc::WRITE_SINGLE_REG,
                static_cast<uint8_t>((ao.addr >> 8) & 0xFF),
                static_cast<uint8_t>(ao.addr & 0xFF),
                static_cast<uint8_t>((regVal >> 8) & 0xFF),
                static_cast<uint8_t>(regVal & 0xFF)
            };
            transId++;
            if (!BuildAndSend(sock, transId, dev.stationId, pdu, sizeof(pdu)))
                continue;

            {
                PacketLogger::Instance().Log(PktDir::TX, dev.channel, dev.stationId,
                    dev.stationId, 0x06, transId, pdu, sizeof(pdu));
            }

            uint8_t resp[256];
            size_t respLen = 0;
            if (!RecvFrame(sock, resp, sizeof(resp), transId, respLen))
                continue;

            if (resp[MBAP_LEN] & EXCEPTION_MASK) continue;

            {
                PacketLogger::Instance().Log(PktDir::RX, dev.channel, dev.stationId,
                    dev.stationId, 0x06, transId,
                    resp + MBAP_LEN, respLen - MBAP_LEN);
            }

            MLogger::Print(MLogger::INFO, "Modbus",
                "FC06 station=" + std::to_string(dev.stationId)
                + " addr=" + std::to_string(ao.addr) + " val=" + std::to_string(regVal));

            // 记录已成功发送的值，避免下一轮重复发送
            {
                std::lock_guard<std::mutex> lock(sentMtx_);
                aoSent_[key] = pt.value;
            }

            // 脉冲复位：pulseMs>0 时，到期自动写 0
            if (ao.pulseMs > 0)
            {
                uint64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                PulseEntry pe;
                pe.channel   = dev.channel;
                pe.stationId = dev.stationId;
                pe.func      = 6;
                pe.addr      = ao.addr;
                pe.valHi     = 0x00;  // reset to 0
                pe.valLo     = 0x00;
                pe.readyAtMs = now + ao.pulseMs;
                {
                    std::lock_guard<std::mutex> lock(pulseMtx_);
                    pulseQueue_.push_back(pe);
                }
                MLogger::Print(MLogger::INFO, "Modbus",
                    "FC06 pulse scheduled: station=" + std::to_string(dev.stationId)
                    + " addr=" + std::to_string(ao.addr)
                    + " reset=0 after=" + std::to_string(ao.pulseMs) + "ms");
            }
        }
        // FC16 可在后续扩展
    }
}

// ==================== 帧收发（静态） ====================

bool ModbusTcpMaster::BuildAndSend(socket& s, uint16_t transId, uint8_t station,
                                const uint8_t* pdu, size_t pduLen)
{
    uint8_t buf[512];
    size_t pos = 0;
    buf[pos++] = static_cast<uint8_t>((transId >> 8) & 0xFF);
    buf[pos++] = static_cast<uint8_t>(transId & 0xFF);
    buf[pos++] = 0x00; buf[pos++] = 0x00; // Protocol ID
    size_t rawLen = 1 + pduLen;
    uint16_t len = static_cast<uint16_t>(rawLen & 0xFFFF);
    buf[pos++] = static_cast<uint8_t>((len >> 8) & 0xFF);
    buf[pos++] = static_cast<uint8_t>(len & 0xFF);
    buf[pos++] = station;
    std::memcpy(buf + pos, pdu, pduLen);
    pos += pduLen;

    size_t total = 0;
    while (total < pos)
    {
        size_t n = s.send(buf + total, pos - total);
        if (n == 0) return false;
        total += n;
    }
    MLogger::HexDump("TX", buf, pos);
    return true;
}

bool ModbusTcpMaster::RecvFrame(socket& s, uint8_t* buf, size_t bufSize,
                             uint16_t expectedTransId, size_t& outLen)
{
    if (bufSize < MBAP_LEN + 2) return false;

    size_t total = 0;
    while (total < MBAP_LEN)
    {
        size_t n = s.recv(buf + total, MBAP_LEN - total);
        if (n == 0) return false;
        total += n;
    }

    unsigned int tH = static_cast<unsigned int>(buf[0]);
    unsigned int tL = static_cast<unsigned int>(buf[1]);
    uint16_t rcvTransId = static_cast<uint16_t>((tH << 8) | tL);
    unsigned int fH = static_cast<unsigned int>(buf[4]);
    unsigned int fL = static_cast<unsigned int>(buf[5]);
    uint16_t frameLen = static_cast<uint16_t>((fH << 8) | fL);

    if (rcvTransId != expectedTransId) return false;
    if (frameLen < 1) return false;
    uint16_t pduLen = static_cast<uint16_t>(frameLen - 1);
    if (MBAP_LEN + pduLen > bufSize) return false;

    total = 0;
    while (total < pduLen)
    {
        size_t n = s.recv(buf + MBAP_LEN + total, pduLen - total);
        if (n == 0) return false;
        total += n;
    }

    outLen = MBAP_LEN + pduLen;
    MLogger::HexDump("RX", buf, outLen);
    return true;
}
