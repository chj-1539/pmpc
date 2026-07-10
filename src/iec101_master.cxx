//=============================================================================
// iec101_master.cxx - IEC 60870-5-101 master station implementation
//
// Link layer: variable frame 68 LEN LEN 68 CTRL ADDR ASDU CS 16
//             fixed frame  10 CMD ADDR CS 16
// ASDU layer reuses IEC 104 format (IecType, IecCOT)
//=============================================================================

#include "iec101_master.h"
#include "packet_logger.h"
#include "module_factory.h"
#include "ini_reader.h"
#include "str_util.h"
#include <iostream>
#include <cstring>
#include <algorithm>

constexpr size_t MAX_FRAME = 256;
constexpr int RECV_TICK_MS = 50;

int Iec101Master::SafeStoi(const std::string& s, int def) { try { return std::stoi(s); } catch (...) { return def; } }
double Iec101Master::SafeStod(const std::string& s, double def) { try { return std::stod(s); } catch (...) { return def; } }

Iec101Master::Iec101Master() {}
Iec101Master::~Iec101Master() { Stop(); }

// ==================== Config loading ====================

bool Iec101Master::LoadConfig(const std::string& path) {
    IniReader ini;
    if (!ini.Load(path)) { std::cerr << "[IEC101] Cannot open: " << path << std::endl; return false; }
    config_ = Iec101MasterConfig{};

    for (auto& k : ini.Keys("global")) {
        auto v = ini.Get("global", k, ""); auto kl = ToLower(k);
        if (kl == "timeout_ms") config_.timeoutMs = SafeStoi(v, 1000);
        else if (kl == "verbose") config_.verbose = SafeStoi(v, 0);
    }

    for (auto& sec : ini.Sections()) {
        std::string low = ToLower(sec); if (low == "global") continue;

        if (StartsWith(low, "channel_")) {
            Iec101ChannelConfig ch;
            for (auto& k : ini.Keys(sec)) {
                auto v = ini.Get(sec, k, ""); auto kl = ToLower(k);
                if (kl == "port_name") ch.portName = v;
                else if (kl == "baud") ch.baud = SafeStoi(v, 9600);
                else if (kl == "parity") ch.parity = ToLower(v);
                else if (kl == "data_bits") ch.dataBits = SafeStoi(v, 8);
                else if (kl == "stop_bits") ch.stopBits = SafeStoi(v, 1);
                else if (kl == "timeout_ms") ch.timeoutMs = SafeStoi(v, 1000);
                else if (kl == "gi_cycle_s") ch.giCycleS = SafeStoi(v, 300);
                else if (kl == "verbose") ch.verbose = SafeStoi(v, 0);
            }
            if (ch.timeoutMs <= 0) ch.timeoutMs = config_.timeoutMs;
            config_.channels.push_back(ch);
            continue;
        }

        if (StartsWith(low, "device_")) {
            if (config_.channels.empty()) continue;
            int chIdx = 0; Iec101DeviceConfig dev;
            for (auto& k : ini.Keys(sec)) {
                auto v = ini.Get(sec, k, ""); auto kl = ToLower(k);
                if (kl == "channel") chIdx = SafeStoi(v, 1) - 1;
                else if (kl == "link_addr") dev.linkAddr = (uint16_t)SafeStoi(v, 1);
                else if (kl == "common_addr") dev.coa = (uint16_t)SafeStoi(v, 1);
                else if (kl == "desc") dev.name = v;
                else if (StartsWith(kl, "ai_")) {
                    auto kv = ParseKeyValues(v); Iec101AIMapping ai{};
                    for (auto& [kk, vv] : kv) { auto l = ToLower(kk);
                        if (l == "ioa") ai.ioa = (uint32_t)SafeStoi(vv);
                        else if (l == "point") ai.point = (uint16_t)SafeStoi(vv);
                        else if (l == "scale") ai.scale = SafeStod(vv, 1.0);
                        else if (l == "offset") ai.offset = SafeStod(vv, 0.0);
                    }
                    if (ai.ioa) dev.aiList.push_back(ai);
                } else if (StartsWith(kl, "di_")) {
                    auto kv = ParseKeyValues(v); Iec101DIMapping di{};
                    for (auto& [kk, vv] : kv) { auto l = ToLower(kk);
                        if (l == "ioa") di.ioa = (uint32_t)SafeStoi(vv);
                        else if (l == "point") di.point = (uint16_t)SafeStoi(vv);
                    }
                    if (di.ioa) dev.diList.push_back(di);
                } else if (StartsWith(kl, "energy_")) {
                    auto kv = ParseKeyValues(v); Iec101EnergyMapping en{};
                    for (auto& [kk, vv] : kv) { auto l = ToLower(kk);
                        if (l == "ioa") en.ioa = (uint32_t)SafeStoi(vv);
                        else if (l == "point") en.point = (uint16_t)SafeStoi(vv);
                        else if (l == "scale") en.scale = SafeStod(vv, 1.0);
                        else if (l == "offset") en.offset = SafeStod(vv, 0.0);
                    }
                    if (en.ioa) dev.energyList.push_back(en);
                }
            }
            if (chIdx >= 0 && chIdx < (int)config_.channels.size())
                config_.channels[chIdx].devices.push_back(dev);
        }
    }

    if (config_.channels.empty()) { std::cerr << "[IEC101] No channels" << std::endl; return false; }
    int dc = 0; for (auto& c : config_.channels) dc += (int)c.devices.size();
    std::cout << "[IEC101] Loaded " << config_.channels.size() << " channels " << dc << " devices" << std::endl;
    return true;
}

bool Iec101Master::Start() {
    if (running_) return false;
    running_ = true;
    for (size_t i = 0; i < config_.channels.size(); i++)
        threads_.emplace_back(&Iec101Master::ChannelThread, this, (int)i);
    std::cout << "[IEC101] Started " << config_.channels.size() << " channels" << std::endl;
    return true;
}

void Iec101Master::Stop() { running_ = false; for (auto& t : threads_) if (t.joinable()) t.join(); threads_.clear(); }

// ==================== Frame send/receive ====================

bool Iec101Master::SendRecvVarFrame(CommIO& io, const uint8_t* asdu, size_t asduLen,
                                      uint16_t linkAddr, uint8_t* respAsdu, size_t& respLen, int timeoutMs) {
    uint8_t frame[MAX_FRAME]; size_t pos = 0;
    frame[pos++] = IEC101_START_VAR;
    uint8_t len = (uint8_t)(4 + asduLen);
    frame[pos++] = len; frame[pos++] = len;
    frame[pos++] = IEC101_START_VAR;
    frame[pos++] = 0x73;  // CTRL: DIR=1,PRM=1,FCB=1,FCV=1,SEND+REQ
    frame[pos++] = (uint8_t)(linkAddr & 0xFF);
    frame[pos++] = (uint8_t)((linkAddr >> 8) & 0xFF);
    std::memcpy(frame + pos, asdu, asduLen); pos += asduLen;
    uint8_t cs = 0; for (size_t ii = 4; ii < pos; ii++) cs = (uint8_t)(cs + frame[ii]); frame[pos++] = cs;
    frame[pos++] = IEC101_END;

    try { io.write(frame, pos); } catch (...) { return false; }

    uint8_t buf[MAX_FRAME]; size_t bp = 0;
    auto start = std::chrono::steady_clock::now();
    while (bp < MAX_FRAME) {
        uint8_t byte;
        try { size_t n = io.read(&byte, 1); if (n == 0) { if (bp > 0) break; if (std::chrono::steady_clock::now() - start > std::chrono::milliseconds(timeoutMs)) break; std::this_thread::sleep_for(std::chrono::milliseconds(RECV_TICK_MS)); continue; } }
        catch (...) { break; }
        if (bp == 0 && byte != IEC101_START_VAR && byte != IEC101_START_FIX) continue;
        buf[bp++] = byte;
        if (buf[0] == IEC101_START_FIX && bp == 5 && buf[4] == IEC101_END) return true;
        if (buf[0] == IEC101_START_VAR && bp >= 4 && bp >= (size_t)buf[1] + 2 && buf[bp-1] == IEC101_END) {
            size_t asduLen_ = buf[1] - 4;
            if (asduLen_ > 0 && asduLen_ <= MAX_FRAME) {
                std::memcpy(respAsdu, buf + 7, asduLen_);
                respLen = asduLen_;
                return true;
            }
        }
    }
    return false;
}

bool Iec101Master::SendRecvFixedFrame(CommIO& io, uint8_t cmd, uint16_t linkAddr, int timeoutMs) {
    uint8_t frame[] = { IEC101_START_FIX, cmd, (uint8_t)(linkAddr & 0xFF), 0x00, IEC101_END };
    frame[3] = (uint8_t)(frame[1] + frame[2]);
    try { io.write(frame, sizeof(frame)); } catch (...) { return false; }
    uint8_t buf[5]; size_t bp = 0;
    auto start = std::chrono::steady_clock::now();
    while (bp < 5) {
        uint8_t byte;
        try { size_t n = io.read(&byte, 1); if (n == 0) { if (std::chrono::steady_clock::now() - start > std::chrono::milliseconds(timeoutMs)) break; std::this_thread::sleep_for(std::chrono::milliseconds(RECV_TICK_MS)); continue; } }
        catch (...) { break; }
        if (bp == 0 && byte != IEC101_START_FIX) continue;
        buf[bp++] = byte;
    }
    return bp == 5 && buf[4] == IEC101_END;
}

// ==================== ASDU handling ====================

static uint32_t ReadIOA(const uint8_t* data) {
    return (uint32_t)((uint32_t)data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16));
}

void Iec101Master::HandleGIResponse(const uint8_t* asdu, size_t len, uint16_t coa) {
    if (len < 6) return;
    auto& mgr = RemoteDataMgr::Instance();
    uint64_t now = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    // simplified: match DI by IOA
    for (auto& ch : config_.channels)
        for (auto& dev : ch.devices)
            if (dev.coa == coa)
                for (auto& di : dev.diList) {
                    // find IOA at offset 6
                    for (size_t i = 6; i + 4 <= len; i += 4) {
                        uint32_t ioa = ReadIOA(asdu + i);
                        if (ioa == di.ioa) {
                            bool val = (asdu[i+3] & 0x01) != 0;
                            mgr.SetDi(1, dev.coa, di.point, val, now, true);
                        }
                    }
                }
}

void Iec101Master::PollDevice(CommIO& io, const Iec101DeviceConfig& dev, int chIdx, int /*devIdx*/, int timeoutMs) {
    uint8_t respAsdu[MAX_FRAME]; size_t respLen = 0;

    // Send GI command
    uint8_t giAsdu[] = {
        IecType::C_IC_NA_1, 0x01, IecCOT::ACTIVATION, 0x00,
        (uint8_t)(dev.coa & 0xFF), (uint8_t)((dev.coa >> 8) & 0xFF),
        0x00, 0x00, 0x00, 0x14
    };
    if (SendRecvVarFrame(io, giAsdu, sizeof(giAsdu), dev.linkAddr, respAsdu, respLen, timeoutMs)) {
        HandleGIResponse(respAsdu, respLen, dev.coa);
    }

    // Request class 1 data
    for (int i = 0; i < 3; i++) {
        if (!running_) break;
        SendRecvFixedFrame(io, 0x5B, dev.linkAddr, timeoutMs);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    if (config_.verbose >= 1)
        std::cout << "[IEC101] Poll link=" << dev.linkAddr << " coa=" << dev.coa << std::endl;
    (void)chIdx;
}

void Iec101Master::ChannelThread(int chIdx) {
    if (chIdx < 0 || chIdx >= (int)config_.channels.size()) return;
    auto& ch = config_.channels[chIdx];
    std::cout << "[IEC101] Channel" << (chIdx+1) << " start: " << ch.portName << std::endl;

    while (running_) {
        CommIO io;
        try {
            io.open(ch.portName, ch.baud, ch.parity, ch.dataBits, ch.stopBits, ch.timeoutMs);
            std::cout << "[IEC101] " << ch.portName << " opened" << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "[IEC101] " << ch.portName << " open fail: " << e.what() << std::endl;
            for (int i = 0; i < 50 && running_; i++) std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }

        SendRecvFixedFrame(io, 0x00, 1, ch.timeoutMs); // reset

        while (running_) {
            try {
                for (size_t di = 0; di < ch.devices.size(); di++) {
                    if (!running_) break;
                    PollDevice(io, ch.devices[di], chIdx, (int)di, ch.timeoutMs);
                }
            } catch (const std::exception& e) { std::cerr << "[IEC101] Error: " << e.what() << std::endl; break; }
            if (running_) for (int i = 0; i < 50 && running_; i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        try { io.close(); } catch (...) {}
        std::cout << "[IEC101] " << ch.portName << " disconnected" << std::endl;
    }
}

REGISTER_MODULE("iec101_master", Iec101MasterModule)

struct Iec101MasterModule::Impl { Iec101Master master; std::string cfgPath; bool loaded = false; bool running = false; };
Iec101MasterModule::Iec101MasterModule() : impl_(std::make_unique<Impl>()) {}
Iec101MasterModule::~Iec101MasterModule() { Stop(); }
bool Iec101MasterModule::LoadConfig(const std::string& cfgPath) { impl_->cfgPath = cfgPath; impl_->loaded = impl_->master.LoadConfig(cfgPath); return impl_->loaded; }
bool Iec101MasterModule::ValidateConfig(const std::string& cfgPath, std::vector<std::string>& errors) { Iec101Master m; if (!m.LoadConfig(cfgPath)) { errors.push_back("Cannot load: " + cfgPath); return false; } return true; }
bool Iec101MasterModule::Start() { if (impl_->running) return true; impl_->running = impl_->master.Start(); return impl_->running; }
void Iec101MasterModule::Stop() { if (!impl_->running) return; impl_->master.Stop(); impl_->running = false; }
bool Iec101MasterModule::IsRunning() const { return impl_->running; }
