//=============================================================================
// packet_logger.cxx — 报文记录器 + Modbus TCP 解析器实现
//=============================================================================

#include "packet_logger.h"
#include "module_factory.h"
#include "ini_reader.h"
#include "str_util.h"
#include <iostream>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#include <iomanip>
#include <cstring>
#include <chrono>
#include <ctime>

// ==================== 时间戳工具 ====================

static std::string CurrentDateStr()
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
    std::strftime(buf, sizeof(buf), "%Y%m%d", &t);
    return buf;
}

// ==================== ModbusTcpParser ====================

bool ModbusTcpParser::CanParse(const ParseContext& /*ctx*/) const
{
    // Modbus TCP 作为默认解析器，总是可以尝试
    return true;
}

std::string ModbusTcpParser::Parse(const ParseContext& ctx) const
{
    // TX: PDU = func + addr(2) + data...
    // RX: PDU = func + byteCount + data...
    if (ctx.len < 2) return "";

    uint8_t func = ctx.func;

    // ── 解析请求（从 PDU 中提取地址/数量） ──
    // FC01/02: func(1) + addr(2) + qty(2) = 5
    // FC03/04: func(1) + addr(2) + qty(2) = 5
    // FC05: func(1) + addr(2) + val(2) = 5
    // FC06: func(1) + addr(2) + val(2) = 5
    // FC15: func(1) + addr(2) + qty(2) + byteCount(1) + data...
    // FC16: func(1) + addr(2) + qty(2) + byteCount(1) + data...

    uint16_t addr = 0, qty = 0;
    bool hasAddr = false, hasQty = false;

    auto readAddr = [&]() {
        if (ctx.len >= 3) {
            addr = ctx.ReadU16(1);
            hasAddr = true;
        }
    };
    auto readQty = [&]() {
        if (ctx.len >= 5) {
            qty = ctx.ReadU16(3);
            hasQty = true;
        }
    };

    // ── TX (主站发出 / 从站收到) ──
    if (ctx.dir == PktDir::TX) {
        switch (func) {
        case 0x01: readAddr(); readQty();
            return "FC01 读线圈 addr=" + std::to_string(addr)
                 + " qty=" + std::to_string(qty);
        case 0x02: readAddr(); readQty();
            return "FC02 读离散输入 addr=" + std::to_string(addr)
                 + " qty=" + std::to_string(qty);
        case 0x03: readAddr(); readQty();
            return "FC03 读保持寄存器 addr=" + std::to_string(addr)
                 + " qty=" + std::to_string(qty);
        case 0x04: readAddr(); readQty();
            return "FC04 读输入寄存器 addr=" + std::to_string(addr)
                 + " qty=" + std::to_string(qty);
        case 0x05: readAddr();
            return "FC05 写线圈 addr=" + std::to_string(addr)
                 + " val=" + ((ctx.len >= 4 && ctx.data[3] == 0xFF) ? "ON" : "OFF");
        case 0x06: readAddr();
            return "FC06 写寄存器 addr=" + std::to_string(addr)
                 + " val=" + (ctx.len >= 5 ? std::to_string(ctx.ReadU16(3)) : "?");
        case 0x0F: readAddr(); readQty();
            return "FC15 写多线圈 addr=" + std::to_string(addr)
                 + " qty=" + std::to_string(qty);
        case 0x10: readAddr(); readQty();
            return "FC16 写多寄存器 addr=" + std::to_string(addr)
                 + " qty=" + std::to_string(qty);
        default:
            if ((func & 0x80) && ctx.len >= 2)
                return "异常响应 func=FC" + std::to_string(func & 0x7F)
                     + " err=0x" + std::to_string(ctx.data[1]);
            return "FC" + std::to_string(func) + " 请求";
        }
    }

    // ── RX (主站收到 / 从站发出) ──
    if (ctx.dir == PktDir::RX) {
        // 异常响应
        if ((func & 0x80) && ctx.len >= 2)
            return "异常 func=FC" + std::to_string(func & 0x7F)
                 + " err=0x" + std::to_string(ctx.data[1]);

        switch (func) {
        case 0x01:
        case 0x02: {
            if (ctx.len >= 2)
                return "响应 " + std::to_string(ctx.data[1]) + " 字节线圈数据";
            return "响应";
        }
        case 0x03:
        case 0x04: {
            if (ctx.len >= 2)
                return "响应 " + std::to_string(ctx.data[1] / 2) + " 个寄存器";
            return "响应";
        }
        case 0x05:
        case 0x06:
            return "写操作响应";
        case 0x0F:
        case 0x10:
            return "多写操作响应";
        default:
            return "响应";
        }
    }

    return "";
}

// ==================== PacketLogger ====================

PacketLogger& PacketLogger::Instance()
{
    static PacketLogger inst;
    return inst;
}

PacketLogger::~PacketLogger()
{
    CloseAll();
}

void PacketLogger::CloseAll()
{
    std::lock_guard<std::mutex> lock(fileMtx_);
    for (auto& [path, entry] : files_) {
        if (entry.stream.is_open())
            entry.stream.close();
    }
    files_.clear();
}

bool PacketLogger::Init(const std::string& logDir)
{
    logDir_ = logDir;
#ifdef _WIN32
    _mkdir(logDir_.c_str());
#else
    mkdir(logDir_.c_str(), 0755);
#endif
    return true;
}

void PacketLogger::EnableDevice(uint16_t ch, uint16_t dev)
{
    std::lock_guard<std::mutex> lock(filterMtx_);
    filter_.insert({ch, dev});
}

void PacketLogger::DisableDevice(uint16_t ch, uint16_t dev)
{
    std::lock_guard<std::mutex> lock(filterMtx_);
    filter_.erase({ch, dev});
}

void PacketLogger::EnableAll()
{
    std::lock_guard<std::mutex> lock(filterMtx_);
    filter_.clear();
}

bool PacketLogger::IsDeviceEnabled(uint16_t ch, uint16_t dev) const
{
    if (!enabled_) return false;
    std::lock_guard<std::mutex> lock(filterMtx_);
    if (filter_.empty()) return true;
    return filter_.count({ch, dev}) > 0;
}

void PacketLogger::RegisterParser(std::unique_ptr<ProtocolParser> parser)
{
    std::lock_guard<std::mutex> lock(parserMtx_);
    parsers_.push_back(std::move(parser));
}

std::string PacketLogger::Timestamp() const
{
    auto now = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count() % 1000;

    struct tm t;
#ifdef _WIN32
    localtime_s(&t, &tt);
#else
    localtime_r(&tt, &t);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &t);
    std::snprintf(buf + 19, sizeof(buf) - 19, ".%03llu", (unsigned long long)ms);
    return buf;
}

std::string PacketLogger::HexDump(const uint8_t* data, size_t len) const
{
    if (len == 0) return "(空)";
    std::ostringstream oss;
    constexpr size_t PER_LINE = 16;
    for (size_t i = 0; i < len; i++) {
        if (i > 0 && i % PER_LINE == 0)
            oss << "\n  ";
        else if (i > 0)
            oss << " ";
        oss << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<int>(data[i]);
    }
    return oss.str();
}

std::string PacketLogger::LogFilePath(uint16_t ch, uint16_t dev) const
{
    return logDir_ + "/ch" + std::to_string(ch)
         + "_dev" + std::to_string(dev)
         + "_" + CurrentDateStr() + ".log";
}

void PacketLogger::Log(PktDir dir, uint16_t ch, uint16_t dev,
                        uint8_t station, uint8_t func,
                        uint16_t transId,
                        const uint8_t* data, size_t len)
{
    if (!enabled_) return;
    if (!IsDeviceEnabled(ch, dev)) return;

    std::string dateStr = CurrentDateStr();
    std::string path = LogFilePath(ch, dev);
    // PacketLogger 日切（第二轮）修复：files_ 的 key 原来是完整路径（含日期），
    // 日切时 LogFilePath 返回新路径 → 找不到旧 entry → 建新 entry → old stream
    // 永远不关闭。文件句柄按天累积，最终耗尽 FD。改为 key = "ch{ch}_dev{dev}"
    // （日期无关），当 dateStr 不匹配时 close 旧句柄并 reopen 新路径。
    std::string key = "ch" + std::to_string(ch) + "_dev" + std::to_string(dev);

    // ── 自动解析 ──
    std::string parse;
    if (parseEnabled_) {
        ParseContext ctx;
        ctx.dir     = dir;
        ctx.ch      = ch;
        ctx.dev     = dev;
        ctx.station = station;
        ctx.func    = func;
        ctx.transId = transId;
        ctx.data    = data;
        ctx.len     = len;

        std::lock_guard<std::mutex> lock(parserMtx_);
        for (const auto& p : parsers_) {
            if (p->CanParse(ctx)) {
                parse = p->Parse(ctx);
                if (!parse.empty()) {
                    parse = std::string("[") + p->ProtocolName() + "] " + parse;
                }
                break;
            }
        }
    }

    // ── 写文件 ──
    std::lock_guard<std::mutex> lock(fileMtx_);
    auto it = files_.find(key);
    if (it == files_.end() || !it->second.stream.is_open()) {
        FileEntry fe;
        fe.dateStr = dateStr;
        fe.stream.open(path, std::ios::app);
        if (!fe.stream.is_open()) return;
        files_[key] = std::move(fe);
        it = files_.find(key);
    }

    auto& file = it->second.stream;
    if (!file.is_open()) return;

    if (it->second.dateStr != dateStr) {
        file.close();
        it->second.dateStr = dateStr;
        path = LogFilePath(ch, dev);    // 重新生成含新日期的路径
        file.open(path, std::ios::app);
        if (!file.is_open()) return;
    }

    std::string ts = Timestamp();
    const char* dirStr = (dir == PktDir::TX) ? "TX" : "RX";

    file << "[" << ts << "] [" << dirStr << "]"
         << " ch=" << ch << " dev=" << dev
         << " station=" << static_cast<int>(station)
         << " FC" << std::setw(2) << std::setfill('0') << static_cast<int>(func)
         << " trans=" << transId
         << " len=" << len << std::endl;

    file << "  hex: " << HexDump(data, len) << std::endl;

    if (!parse.empty()) {
        file << "  --> " << parse << std::endl;
    }

    file << std::endl;
    file.flush();
}

void PacketLogger::CleanupOldLogs()
{
    if (maxDays_ <= 0) return;
    auto cutoff = std::chrono::system_clock::now() - std::chrono::hours(24 * maxDays_);
    auto cutoff_t = std::chrono::system_clock::to_time_t(cutoff);

#ifdef _WIN32
    WIN32_FIND_DATAA ffd;
    HANDLE hFind = FindFirstFileA((logDir_ + "\\*.log").c_str(), &ffd);
    if (hFind == INVALID_HANDLE_VALUE) return;
    do {
        std::string fname = ffd.cFileName;
        if (fname == "." || fname == "..") continue;
        std::string fpath = logDir_ + "\\" + fname;
        HANDLE hFile = CreateFileA(fpath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile != INVALID_HANDLE_VALUE) {
            FILETIME ftWrite;
            if (GetFileTime(hFile, NULL, NULL, &ftWrite)) {
                ULARGE_INTEGER uli;
                uli.LowPart = ftWrite.dwLowDateTime;
                uli.HighPart = ftWrite.dwHighDateTime;
                time_t ftime = (uli.QuadPart - 116444736000000000ULL) / 10000000ULL;
                if (ftime < cutoff_t) {
                    CloseHandle(hFile);
                    std::lock_guard<std::mutex> lock(fileMtx_);
                    files_.erase(fpath);
                    DeleteFileA(fpath.c_str());
                    continue;
                }
            }
            CloseHandle(hFile);
        }
    } while (FindNextFileA(hFind, &ffd) != 0);
    FindClose(hFind);
#else
    // POSIX: use directory iteration via dirent.h
    DIR* dir = opendir(logDir_.c_str());
    if (!dir) return;
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        std::string fname = entry->d_name;
        if (fname.size() < 4 || fname.substr(fname.size()-4) != ".log") continue;
        std::string fpath = logDir_ + "/" + fname;
        struct stat st;
        if (stat(fpath.c_str(), &st) == 0 && st.st_mtime < cutoff_t) {
            std::lock_guard<std::mutex> lock(fileMtx_);
            files_.erase(fpath);
            unlink(fpath.c_str());
        }
    }
    closedir(dir);
#endif
}

std::string PacketLogger::GetStatus() const
{
    std::ostringstream oss;
    oss << "报文记录器: " << (enabled_ ? "已启用" : "已禁用")
        << " 解析=" << (parseEnabled_ ? "开" : "关")
        << " 目录=" << logDir_;

    std::lock_guard<std::mutex> lock(parserMtx_);
    if (!parsers_.empty()) {
        oss << " 解析器:";
        for (const auto& p : parsers_)
            oss << " " << p->ProtocolName();
    }
    return oss.str();
}

size_t PacketLogger::GetFileCount() const
{
    std::lock_guard<std::mutex> lock(fileMtx_);
    return files_.size();
}

// ==================== PacketLoggerModule ====================

PacketLoggerModule* PacketLoggerModule::GetInstance()
{
    if (!g_moduleManager) return nullptr;
    auto* mod = g_moduleManager->GetModule("packet_logger");
    if (!mod) return nullptr;
    return dynamic_cast<PacketLoggerModule*>(mod);
}

PacketLoggerModule::PacketLoggerModule() {}
PacketLoggerModule::~PacketLoggerModule() { Stop(); }

bool PacketLoggerModule::LoadConfig(const std::string& cfgPath)
{
    cfgPath_ = cfgPath;
    auto& pl = PacketLogger::Instance();
    IniReader ini;
    if (!ini.Load(cfgPath)) {
        std::cout << "[PacketLogger] 无配置文件，使用默认设置" << std::endl;
        pl.Init("logs/traffic");
        loaded_ = true;
        return true;
    }

    std::string logDir = ini.Get("global", "log_dir", "logs/traffic");
    int parseEn = ini.GetInt("global", "parse", 1);
    pl.Init(logDir);
    pl.SetParseEnabled(parseEn != 0);
    pl.SetMaxDays(ini.GetInt("global", "max_days", 30));

    loaded_ = true;
    std::cout << "[PacketLogger] 配置加载: dir=" << logDir
              << " parse=" << (parseEn ? "on" : "off") << std::endl;
    return true;
}

bool PacketLoggerModule::ValidateConfig(const std::string& cfgPath,
                                         std::vector<std::string>& errors)
{
    IniReader ini;
    if (!ini.Load(cfgPath)) {
        errors.push_back("Cannot load: " + cfgPath);
        return false;
    }
    return errors.empty();
}

bool PacketLoggerModule::Start()
{
    if (running_) return true;

    // ── 注册内建解析器 ──
    PacketLogger::Instance().RegisterParser(std::make_unique<ModbusTcpParser>());

    PacketLogger::Instance().SetEnabled(false);
    running_ = true;
    std::cout << "[PacketLogger] 模块已就绪（记录未启用，需通过调试控制台启动）" << std::endl;
    return true;
}

void PacketLoggerModule::Stop()
{
    running_ = false;
    PacketLogger::Instance().SetEnabled(false);
    PacketLogger::Instance().CloseAll();
}

bool PacketLoggerModule::IsRunning() const
{
    return running_;
}

REGISTER_MODULE("packet_logger", PacketLoggerModule)
