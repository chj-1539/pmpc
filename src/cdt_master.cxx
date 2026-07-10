//=============================================================================
// cdt_master.cxx - CDT master (passive receive) implementation
//=============================================================================

#include "cdt_master.h"
#include "packet_logger.h"
#include "module_factory.h"
#include "ini_reader.h"
#include "str_util.h"
#include "comm_io.h"
#include <iostream>
#include <cstring>

constexpr uint8_t  SYNC_BYTE     = 0xEB;
constexpr uint8_t  SYNC_BYTE2    = 0x90;
constexpr size_t   SYNC_LEN      = 6;  // EB 90 EB 90 EB 90
constexpr size_t   CTRL_WORD_LEN = 2;
constexpr size_t   CRC_LEN       = 2;
constexpr size_t   MAX_FRAME     = 256;
constexpr int      RECV_TIMEOUT  = 200;

int CdtMaster::SafeStoi(const std::string& s, int def) { try { return std::stoi(s); } catch (...) { return def; } }
double CdtMaster::SafeStod(const std::string& s, double def) { try { return std::stod(s); } catch (...) { return def; } }

CdtMaster::CdtMaster() {}
CdtMaster::~CdtMaster() { Stop(); }

bool CdtMaster::LoadConfig(const std::string& path) {
    IniReader ini; if (!ini.Load(path)) { std::cerr << "[CdtMaster] Cannot open: " << path << std::endl; return false; }
    config_ = CdtMasterConfig{};
    auto sections = ini.Sections();
    for (auto& sec : sections) {
        std::string low = ToLower(sec);
        if (low == "global") {
            config_.timeoutMs = ini.GetInt(sec, "timeout_ms", 1000);
            config_.verbose = ini.GetInt(sec, "verbose", 0);
            config_.frameTimeoutMs = ini.GetInt(sec, "frame_timeout_ms", 5000);
        }
    }
    for (auto& sec : sections) {
        std::string low = ToLower(sec);
        if (low == "global" || !StartsWith(low, "channel_")) continue;
        CdtMasterChannel ch;
        for (auto& k : ini.Keys(sec)) {
            auto v = ini.Get(sec, k, ""); auto kl = ToLower(k);
            if (kl == "port_name") ch.portName = v;
            else if (kl == "baud") ch.baud = SafeStoi(v, 1200);
            else if (kl == "parity") ch.parity = ToLower(v);
            else if (kl == "data_bits") ch.dataBits = SafeStoi(v, 8);
            else if (kl == "stop_bits") ch.stopBits = SafeStoi(v, 1);
            else if (kl == "timeout_ms") ch.timeoutMs = SafeStoi(v, 1000);
            else if (kl == "verbose") ch.verbose = SafeStoi(v, 0);
        }
        if (ch.timeoutMs <= 0) ch.timeoutMs = config_.timeoutMs;
        config_.channels.push_back(ch);
    }
    for (auto& sec : sections) {
        std::string low = ToLower(sec);
        if (low == "global" || StartsWith(low, "channel_")) continue;
        if (!StartsWith(low, "device_")) continue;
        int chIdx = ini.GetInt(sec, "channel", 1) - 1;
        if (chIdx < 0 || chIdx >= (int)config_.channels.size()) continue;
        CdtMasterDevice dev;
        dev.station = (uint16_t)ini.GetInt(sec, "station", 1);
        for (auto& k : ini.Keys(sec)) {
            std::string kl = ToLower(k); std::string v = ini.Get(sec, k, "");
            if (kl == "channel" || kl == "station") continue;
            if (StartsWith(kl, "yc_")) {
                auto kv = ParseKeyValues(v); CdtMasterYC yc{};
                for (auto& [kk, vv] : kv) { auto l = ToLower(kk);
                    if (l == "addr") yc.addr = (uint16_t)SafeStoi(vv);
                    else if (l == "point") yc.point = (uint16_t)SafeStoi(vv);
                    else if (l == "scale") yc.scale = SafeStod(vv, 1.0);
                    else if (l == "offset") yc.offset = SafeStod(vv, 0.0);
                }
                if (yc.addr || yc.point) dev.ycList.push_back(yc);
            } else if (StartsWith(kl, "yx_")) {
                auto kv = ParseKeyValues(v); CdtMasterYX yx{};
                for (auto& [kk, vv] : kv) { auto l = ToLower(kk);
                    if (l == "addr") yx.addr = (uint16_t)SafeStoi(vv);
                    else if (l == "bit") yx.bit = (uint8_t)SafeStoi(vv);
                    else if (l == "point") yx.point = (uint16_t)SafeStoi(vv);
                }
                if (yx.addr || yx.point) dev.yxList.push_back(yx);
            } else if (StartsWith(kl, "ym_")) {
                auto kv = ParseKeyValues(v); CdtMasterYM ym{};
                for (auto& [kk, vv] : kv) { auto l = ToLower(kk);
                    if (l == "addr") ym.addr = (uint16_t)SafeStoi(vv);
                    else if (l == "point") ym.point = (uint16_t)SafeStoi(vv);
                    else if (l == "scale") ym.scale = SafeStod(vv, 1.0);
                    else if (l == "offset") ym.offset = SafeStod(vv, 0.0);
                }
                if (ym.addr || ym.point) dev.ymList.push_back(ym);
            }
        }
        config_.channels[chIdx].devices.push_back(dev);
    }
    if (config_.channels.empty()) { std::cerr << "[CdtMaster] No channels" << std::endl; return false; }
    int dc = 0; for (auto& c : config_.channels) dc += (int)c.devices.size();
    std::cout << "[CdtMaster] Loaded " << config_.channels.size() << " channels " << dc << " devices" << std::endl;
    return true;
}

bool CdtMaster::Start() {
    if (running_) return false;
    running_ = true;
    for (size_t i = 0; i < config_.channels.size(); i++)
        threads_.emplace_back(&CdtMaster::PortThread, this, (int)i);
    std::cout << "[CdtMaster] Started " << config_.channels.size() << " channels" << std::endl;
    return true;
}

void CdtMaster::Stop() { running_ = false; for (auto& t : threads_) if (t.joinable()) t.join(); threads_.clear(); }

void CdtMaster::PortThread(int chIdx) {
    if (chIdx < 0 || chIdx >= (int)config_.channels.size()) return;
    auto& ch = config_.channels[chIdx];
    std::cout << "[CdtMaster] Channel " << (chIdx+1) << " start: " << ch.portName << std::endl;

    while (running_) {
        CommIO io;
        try {
            io.open(ch.portName, ch.baud, ch.parity, ch.dataBits, ch.stopBits, ch.timeoutMs);
            std::cout << "[CdtMaster] " << ch.portName << " opened" << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "[CdtMaster] " << ch.portName << " open fail: " << e.what() << std::endl;
            for (int i = 0; i < 50 && running_; i++) std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }

        uint8_t buf[MAX_FRAME * 2]; size_t pos = 0;
        while (running_) {
            uint8_t byte;
            try {
                size_t n = io.read(&byte, 1);
                if (n == 0) continue;
            } catch (const std::exception&) { break; }

            // Sync header detection            if (pos == 0 && byte != SYNC_BYTE) continue;
            if (pos == 1 && byte != SYNC_BYTE2) { if (byte == SYNC_BYTE) { pos = 1; continue; } pos = 0; continue; }
            if (pos == 2 && byte != SYNC_BYTE) { pos = 0; continue; }
            if (pos == 3 && byte != SYNC_BYTE2) { if (byte == SYNC_BYTE) pos = 1; else pos = 0; continue; }
            if (pos == 4 && byte != SYNC_BYTE) { pos = 0; continue; }
            if (pos == 5 && byte != SYNC_BYTE2) { if (byte == SYNC_BYTE) pos = 1; else pos = 0; continue; }

            buf[pos++] = byte;

            // 同步头完成后，解析控制字获取长度
            if (pos == 8) {
                // Control word: buf[6] = function, buf[7] = info count
                uint8_t fun = buf[6];
                uint8_t infoCount = buf[7] & 0x7F;
                int dataLen = 0;
                uint8_t funHigh = static_cast<uint8_t>(fun >> 4);
                uint8_t funLow = static_cast<uint8_t>(fun & 0x0F);
                if (funHigh == 6) dataLen = infoCount * 4;  // YM 电度: 每点 4 字节
                else if (funLow <= 3) dataLen = infoCount * 2;  // YC: 每点 2 字节
                else dataLen = (infoCount + 7) / 8;  // YX: 按位打包

                int expectedTotal = static_cast<int>(SYNC_LEN + CTRL_WORD_LEN + dataLen + CRC_LEN);
                if (expectedTotal > (int)MAX_FRAME) { pos = 0; continue; }
                // 继续接收剩余数据
                while ((int)pos < expectedTotal && running_) {
                    try { size_t n = io.read(&byte, 1); if (n == 0) break; buf[pos++] = byte; } catch (...) { pos = 0; break; }
                }
                if ((int)pos == expectedTotal) {
                    PacketLogger::Instance().Log(PktDir::RX, static_cast<uint8_t>(chIdx+1), 0, 0, buf[6], 0, buf, pos);
                    uint16_t station = (buf[7] & 0x80) ? (uint16_t)(buf[7] >> 3) : 1;
                    ParseFrame(buf + SYNC_LEN, expectedTotal - SYNC_LEN, station);
                }
                pos = 0;
            }
        }
        try { io.close(); } catch (...) {}
        std::cout << "[CdtMaster] " << ch.portName << " disconnected" << std::endl;
    }
}

void CdtMaster::ParseFrame(const uint8_t* data, size_t len, uint16_t station) {
    if (len < 4) return;
    uint8_t fun = data[0];
    uint8_t infoCount = data[1] & 0x7F;
    const uint8_t* payload = data + 2;
    size_t dataLen = len - 4; // subtract ctrl(2) + CRC(2)
    if (dataLen == 0) return;

    uint64_t now = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    auto& mgr = RemoteDataMgr::Instance();

    // 查找设备
    for (auto& ch : config_.channels)
        for (auto& dev : ch.devices)
            if (dev.station == station) {
                uint8_t funHigh = static_cast<uint8_t>(fun >> 4);
                uint8_t funLow = static_cast<uint8_t>(fun & 0x0F);

                if (funHigh == 6) {
                    for (uint16_t i = 0; i < infoCount && (i+1)*4 <= (int)dataLen; i++) {
                        int32_t raw = (int32_t)((uint32_t)payload[i*4] |
                            ((uint32_t)payload[i*4+1] << 8) |
                            ((uint32_t)payload[i*4+2] << 16) |
                            ((uint32_t)payload[i*4+3] << 24));
                        double val = (double)raw;
                        for (auto& ym : dev.ymList)
                            if (ym.addr == i) { mgr.SetAi(1, station, ym.point, val * ym.scale + ym.offset); break; }
                    }
                } else if (funLow <= 3) {
                    for (uint16_t i = 0; i < infoCount && (i+1)*2 <= (int)dataLen; i++) {
                        int16_t raw = (int16_t)((uint16_t)payload[i*2] | ((uint16_t)payload[i*2+1] << 8));
                        double val = (double)raw;
                        for (auto& yc : dev.ycList)
                            if (yc.addr == i) { mgr.SetAi(1, station, yc.point, val * yc.scale + yc.offset); break; }
                    }
                } else {
                    // YX digital: bit-packed
                    for (uint16_t i = 0; i < infoCount; i++) {
                        uint16_t byteIdx = i / 8;
                        if (byteIdx >= dataLen) break;
                        bool val = (payload[byteIdx] >> (i % 8)) & 1;
                        for (auto& yx : dev.yxList)
                            if (yx.addr == i / 8 && yx.bit == (i % 8)) { mgr.SetDi(1, station, yx.point, val, now, true); break; }
                    }
                }
                return;
            }
}

REGISTER_MODULE("cdt_master", CdtMasterModule)

struct CdtMasterModule::Impl { CdtMaster master; std::string cfgPath; bool loaded = false; bool running = false; };
CdtMasterModule::CdtMasterModule() : impl_(std::make_unique<Impl>()) {}
CdtMasterModule::~CdtMasterModule() { Stop(); }
bool CdtMasterModule::LoadConfig(const std::string& cfgPath) { impl_->cfgPath = cfgPath; impl_->loaded = impl_->master.LoadConfig(cfgPath); return impl_->loaded; }
bool CdtMasterModule::ValidateConfig(const std::string& cfgPath, std::vector<std::string>& errors) { CdtMaster m; if (!m.LoadConfig(cfgPath)) { errors.push_back("Cannot load: " + cfgPath); return false; } return true; }
bool CdtMasterModule::Start() { if (impl_->running) return true; impl_->running = impl_->master.Start(); return impl_->running; }
void CdtMasterModule::Stop() { if (!impl_->running) return; impl_->master.Stop(); impl_->running = false; }
bool CdtMasterModule::IsRunning() const { return impl_->running; }
