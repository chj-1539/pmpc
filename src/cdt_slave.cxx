//=============================================================================
// cdt_slave.cxx - CDT slave (RTU, active transmit) implementation
//=============================================================================

#include "cdt_slave.h"
#include "packet_logger.h"
#include "module_factory.h"
#include "ini_reader.h"
#include "str_util.h"
#include "comm_io.h"
#include <iostream>
#include <cstring>

constexpr uint8_t  SYNC_BYTE    = 0xEB;
constexpr uint8_t  SYNC_BYTE2   = 0x90;
constexpr size_t   SYNC_LEN     = 6;
constexpr size_t   MAX_FRAME    = 256;

int CdtSlave::SafeStoi(const std::string& s, int def) { try { return std::stoi(s); } catch (...) { return def; } }
double CdtSlave::SafeStod(const std::string& s, double def) { try { return std::stod(s); } catch (...) { return def; } }

CdtSlave::CdtSlave() {} CdtSlave::~CdtSlave() { Stop(); }

bool CdtSlave::LoadConfig(const std::string& path) {
    IniReader ini; if (!ini.Load(path)) { std::cerr << "[CdtSlave] Cannot open: " << path << std::endl; return false; }
    config_ = CdtSlaveConfig{};
    config_.portName = ini.Get("global", "port_name", "COM1");
    config_.baud = ini.GetInt("global", "baud", 1200);
    config_.parity = ToLower(ini.Get("global", "parity", "even"));
    config_.dataBits = ini.GetInt("global", "data_bits", 8);
    config_.stopBits = ini.GetInt("global", "stop_bits", 1);
    config_.cycleMs = ini.GetInt("global", "cycle_ms", 1000);
    config_.verbose = ini.GetInt("global", "verbose", 0);

    for (auto& sec : ini.Sections()) {
        std::string low = ToLower(sec);
        if (low == "global") continue;
        if (!StartsWith(low, "device_")) continue;
        CdtSlaveDevice dev;
        dev.station = (uint16_t)ini.GetInt(sec, "station", 1);
        for (auto& k : ini.Keys(sec)) {
            std::string kl = ToLower(k); std::string v = ini.Get(sec, k, "");
            if (kl == "station") continue;
            if (StartsWith(kl, "yc_")) {
                auto kv = ParseKeyValues(v); CdtSlaveYC yc{};
                for (auto& [kk, vv] : kv) { auto l = ToLower(kk);
                    if (l == "addr") yc.addr = (uint16_t)SafeStoi(vv);
                    else if (l == "ch") yc.ch = (uint16_t)SafeStoi(vv);
                    else if (l == "dev") yc.dev = (uint16_t)SafeStoi(vv);
                    else if (l == "point") yc.point = (uint16_t)SafeStoi(vv);
                    else if (l == "scale") yc.scale = SafeStod(vv, 1.0);
                    else if (l == "offset") yc.offset = SafeStod(vv, 0.0);
                }
                if (yc.addr || yc.point) dev.ycList.push_back(yc);
            } else if (StartsWith(kl, "yx_")) {
                auto kv = ParseKeyValues(v); CdtSlaveYX yx{};
                for (auto& [kk, vv] : kv) { auto l = ToLower(kk);
                    if (l == "addr") yx.addr = (uint16_t)SafeStoi(vv);
                    else if (l == "bit") yx.bit = (uint8_t)SafeStoi(vv);
                    else if (l == "ch") yx.ch = (uint16_t)SafeStoi(vv);
                    else if (l == "dev") yx.dev = (uint16_t)SafeStoi(vv);
                    else if (l == "point") yx.point = (uint16_t)SafeStoi(vv);
                }
                if (yx.addr || yx.point) dev.yxList.push_back(yx);
            } else if (StartsWith(kl, "ym_")) {
                auto kv = ParseKeyValues(v); CdtSlaveYM ym{};
                for (auto& [kk, vv] : kv) { auto l = ToLower(kk);
                    if (l == "addr") ym.addr = (uint16_t)SafeStoi(vv);
                    else if (l == "ch") ym.ch = (uint16_t)SafeStoi(vv);
                    else if (l == "dev") ym.dev = (uint16_t)SafeStoi(vv);
                    else if (l == "point") ym.point = (uint16_t)SafeStoi(vv);
                    else if (l == "scale") ym.scale = SafeStod(vv, 1.0);
                    else if (l == "offset") ym.offset = SafeStod(vv, 0.0);
                }
                if (ym.addr || ym.point) dev.ymList.push_back(ym);
            }
        }
        config_.devices.push_back(dev);
    }
    if (config_.devices.empty()) { std::cerr << "[CdtSlave] No devices" << std::endl; return false; }
    std::cout << "[CdtSlave] Loaded " << config_.devices.size() << " devices" << std::endl;
    return true;
}

bool CdtSlave::Start() {
    if (running_) return false;
    running_ = true;
    thread_ = std::thread(&CdtSlave::PortThread, this);
    std::cout << "[CdtSlave] Started" << std::endl; return true;
}
void CdtSlave::Stop() { running_ = false; if (thread_.joinable()) thread_.join(); }

void CdtSlave::PortThread() {
    std::cout << "[CdtSlave] Port thread start: " << config_.portName << std::endl;
    while (running_) {
        CommIO io;
        try {
            io.open(config_.portName, config_.baud, config_.parity, config_.dataBits, config_.stopBits, 1000);
            std::cout << "[CdtSlave] " << config_.portName << " opened" << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "[CdtSlave] " << config_.portName << " open fail: " << e.what() << std::endl;
            for (int i = 0; i < 50 && running_; i++) std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }

        while (running_) {
            BuildAndSend(io);
            for (int i = 0; i < config_.cycleMs / 100 && running_; i++)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        try { io.close(); } catch (...) {}
    }
}

void CdtSlave::BuildAndSend(CommIO& io) {
    auto& mgr = RemoteDataMgr::Instance();
    uint8_t frame[MAX_FRAME];

    for (auto& dev : config_.devices) {
        //  YC 遥测
        if (!dev.ycList.empty()) {
            size_t pos = 0;
            for (int i = 0; i < 3; i++) { frame[pos++] = SYNC_BYTE; frame[pos++] = SYNC_BYTE2; }
            // Control word: func 0x01 = YC + info count
            uint8_t count = (uint8_t)(dev.ycList.size() > 127 ? 127 : dev.ycList.size());
            frame[pos++] = 0x01;  // 功能YC
            frame[pos++] = (uint8_t)(count | (dev.station << 3));  // 信息字数 + 站地址
            for (uint16_t i = 0; i < count; i++) {
                auto& yc = dev.ycList[i]; AiPoint pt;
                double raw = 0;
                if (mgr.GetAi(yc.ch, yc.dev, yc.point, pt))
                    raw = (pt.value - yc.offset) / yc.scale;
                int16_t rawInt = (int16_t)(raw);
                frame[pos++] = (uint8_t)(rawInt & 0xFF);
                frame[pos++] = (uint8_t)((rawInt >> 8) & 0xFF);
            }
            // CRC（简化：和校验）
            uint8_t cs = 0; for (size_t i = 6; i < pos; i++) cs = (uint8_t)(cs + frame[i]);
            frame[pos++] = cs;
            frame[pos++] = 0x00;

            try { io.write(frame, pos); } catch (const std::exception&) { return; }
            PacketLogger::Instance().Log(PktDir::TX, 0, 0, static_cast<uint8_t>(dev.station), frame[6], 0, frame, pos);
        }

        // YX digital (DI)
        if (!dev.yxList.empty()) {
            size_t pos = 0;
            for (int i = 0; i < 3; i++) { frame[pos++] = SYNC_BYTE; frame[pos++] = SYNC_BYTE2; }

            // Group by addr, 8 DI per byte
            int maxAddr = 0; for (auto& yx : dev.yxList) if ((int)yx.addr > maxAddr) maxAddr = yx.addr;
            int byteCount = maxAddr + 1;
            int bitCount = byteCount * 8;

            frame[pos++] = 0x10;  // Func: YX
            uint8_t bcLow = (uint8_t)(bitCount > 127 ? 127 : bitCount);
            frame[pos++] = (uint8_t)(bcLow | (dev.station << 3));

            uint8_t yxData[128]; memset(yxData, 0, byteCount);
            for (auto& yx : dev.yxList) {
                DiPoint pt;
                if (mgr.GetDi(yx.ch, yx.dev, yx.point, pt) && pt.value)
                    yxData[yx.addr] |= (uint8_t)(1 << yx.bit);
            }
            for (int i = 0; i < byteCount; i++) frame[pos++] = yxData[i];

            uint8_t cs = 0; for (size_t i = 6; i < pos; i++) cs = (uint8_t)(cs + frame[i]);
            frame[pos++] = cs; frame[pos++] = 0x00;
            try { io.write(frame, pos); } catch (...) { return; }
        }

        // YM energy (0x60)
        if (!dev.ymList.empty()) {
            size_t pos = 0;
            for (int i = 0; i < 3; i++) { frame[pos++] = SYNC_BYTE; frame[pos++] = SYNC_BYTE2; }
            uint8_t count = (uint8_t)(dev.ymList.size() > 127 ? 127 : dev.ymList.size());
            frame[pos++] = 0x60;  // Func: YM
            for (uint16_t i = 0; i < count; i++) {
                auto& ym = dev.ymList[i]; AiPoint pt;
                double raw = 0;
                if (mgr.GetAi(ym.ch, ym.dev, ym.point, pt))
                    raw = (pt.value - ym.offset) / ym.scale;
                int32_t rawInt = (int32_t)raw;
                frame[pos++] = (uint8_t)(rawInt & 0xFF);
                frame[pos++] = (uint8_t)((rawInt >> 8) & 0xFF);
                frame[pos++] = (uint8_t)((rawInt >> 16) & 0xFF);
                frame[pos++] = (uint8_t)((rawInt >> 24) & 0xFF);
            }
            uint8_t cs2 = 0; for (size_t i = 6; i < pos; i++) cs2 = (uint8_t)(cs2 + frame[i]);
            frame[pos++] = cs2; frame[pos++] = 0x00;
            try { io.write(frame, pos); } catch (...) { return; }
        }
    }
}

REGISTER_MODULE("cdt_slave", CdtSlaveModule)

struct CdtSlaveModule::Impl { CdtSlave rtu; std::string cfgPath; bool loaded = false; bool running = false; };
CdtSlaveModule::CdtSlaveModule() : impl_(std::make_unique<Impl>()) {}
CdtSlaveModule::~CdtSlaveModule() { Stop(); }
bool CdtSlaveModule::LoadConfig(const std::string& cfgPath) { impl_->cfgPath = cfgPath; impl_->loaded = impl_->rtu.LoadConfig(cfgPath); return impl_->loaded; }
bool CdtSlaveModule::ValidateConfig(const std::string& cfgPath, std::vector<std::string>& errors) { CdtSlave r; if (!r.LoadConfig(cfgPath)) { errors.push_back("Cannot load: " + cfgPath); return false; } return true; }
bool CdtSlaveModule::Start() { if (impl_->running) return true; impl_->running = impl_->rtu.Start(); return impl_->running; }
void CdtSlaveModule::Stop() { if (!impl_->running) return; impl_->rtu.Stop(); impl_->running = false; }
bool CdtSlaveModule::IsRunning() const { return impl_->running; }
