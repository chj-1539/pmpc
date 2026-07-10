//=============================================================================
// iec103_master.cxx - IEC 60870-5-103 master station implementation
//
// Simplified protocol:
//   Send query, receive ASDU response, dispatch by FUN+INF to local points
//   Frame: 68 LEN LEN 68 CTRL ADDR ASDU CS 16
//=============================================================================

#include "iec103_master.h"
#include "packet_logger.h"
#include "module_factory.h"
#include "ini_reader.h"
#include "str_util.h"
#include <iostream>
#include <cstring>
#include <algorithm>
#include <cmath>

constexpr size_t MAX_FRAME = 256;
constexpr int RECV_TICK_MS = 100;

int Iec103Master::SafeStoi(const std::string& s, int def) { try { return std::stoi(s); } catch (...) { return def; } }
double Iec103Master::SafeStod(const std::string& s, double def) { try { return std::stod(s); } catch (...) { return def; } }

Iec103Master::Iec103Master() {}
Iec103Master::~Iec103Master() { Stop(); }

// ==================== 配置加载 ====================

bool Iec103Master::LoadConfig(const std::string& path) {
    IniReader ini; if (!ini.Load(path)) { std::cerr << "[IEC103] Cannot open: " << path << std::endl; return false; }
    config_ = Iec103MasterConfig{};

    for (auto& k : ini.Keys("global")) {
        auto v = ini.Get("global", k, ""); auto kl = ToLower(k);
        if (kl == "timeout_ms") config_.timeoutMs = SafeStoi(v, 1000);
        else if (kl == "verbose") config_.verbose = SafeStoi(v, 0);
    }

    for (auto& sec : ini.Sections()) {
        std::string low = ToLower(sec);
        if (low == "global") continue;

        if (StartsWith(low, "template_")) {
            std::string tplName = sec.substr(9);
            Iec103DeviceConfig tpl;
            for (auto& k : ini.Keys(sec)) {
                std::string kl = ToLower(k); std::string v = ini.Get(sec, k, "");
                auto kv = ParseKeyValues(v);
                uint8_t fun = 0, inf = 0; uint16_t pt = 0;
                for (auto& [kk, vv] : kv) { auto l = ToLower(kk);
                    if (l == "fun") fun = (uint8_t)SafeStoi(vv);
                    else if (l == "inf") inf = (uint8_t)SafeStoi(vv);
                    else if (l == "point") pt = (uint16_t)SafeStoi(vv);
                }
                if (fun == 0) continue;
                if (StartsWith(kl, "di_")) { tpl.diList.push_back({fun, inf, pt}); }
                else if (StartsWith(kl, "ai_") || StartsWith(kl, "ep_")) {
                    uint8_t type = 0; double scale = 1.0, offset = 0.0;
                    for (auto& [kk, vv] : kv) { auto l = ToLower(kk);
                        if (l == "type") type = (ToLower(vv) == "float") ? 1 : 0;
                        else if (l == "scale") scale = SafeStod(vv, 1.0);
                        else if (l == "offset") offset = SafeStod(vv, 0.0);
                    }
                    tpl.aiList.push_back({fun, inf, pt, type, scale, offset});
                }
                else if (StartsWith(kl, "do_")) { tpl.doList.push_back({fun, inf, pt}); }
            }
            templates_[tplName] = tpl;
            continue;
        }

        if (StartsWith(low, "channel_")) {
            Iec103ChannelConfig ch;
            for (auto& k : ini.Keys(sec)) {
                std::string kl = ToLower(k); std::string v = ini.Get(sec, k, "");
                if (kl == "port_name") ch.portName = v;
                else if (kl == "baud") ch.baud = SafeStoi(v, 9600);
                else if (kl == "parity") ch.parity = ToLower(v);
                else if (kl == "data_bits") ch.dataBits = SafeStoi(v, 8);
                else if (kl == "stop_bits") ch.stopBits = SafeStoi(v, 1);
                else if (kl == "scan_ms") ch.scanMs = SafeStoi(v, 3000);
                else if (kl == "timeout_ms") ch.timeoutMs = SafeStoi(v, 1000);
                else if (kl == "verbose") ch.verbose = SafeStoi(v, 0);
            }
            if (ch.timeoutMs <= 0) ch.timeoutMs = config_.timeoutMs;
            config_.channels.push_back(ch);
            continue;
        }

        if (StartsWith(low, "device_")) {
            if (config_.channels.empty()) continue;
            int chIdx = 0; Iec103DeviceConfig dev;
            std::string tplName;
            for (auto& k : ini.Keys(sec)) {
                std::string kl = ToLower(k); std::string v = ini.Get(sec, k, "");
                if (kl == "channel") chIdx = SafeStoi(v, 1) - 1;
                else if (kl == "station") dev.station = (uint16_t)SafeStoi(v, 1);
                else if (kl == "template") tplName = v;
                else if (kl == "desc") dev.name = v;
            }
            auto it = templates_.find(tplName);
            if (it != templates_.end()) {
                dev.templateName = tplName;
                dev.diList = it->second.diList;
                dev.aiList = it->second.aiList;
                dev.doList = it->second.doList;
            }
            if (chIdx >= 0 && chIdx < (int)config_.channels.size())
                config_.channels[chIdx].devices.push_back(dev);
        }
    }

    if (config_.channels.empty()) { std::cerr << "[IEC103] No channels" << std::endl; return false; }
    int dc = 0; for (auto& c : config_.channels) dc += (int)c.devices.size();
    std::cout << "[IEC103] Loaded " << config_.channels.size() << " channels " << dc << " devices templates=" << templates_.size() << std::endl;
    return true;
}

// ==================== 启停 ====================

bool Iec103Master::Start() {
    if (running_) return false;
    running_ = true;
    for (size_t i = 0; i < config_.channels.size(); i++)
        threads_.emplace_back(&Iec103Master::ChannelThread, this, (int)i);
    std::cout << "[IEC103] Started " << config_.channels.size() << " channels" << std::endl;
    return true;
}

void Iec103Master::Stop() { running_ = false; for (auto& t : threads_) if (t.joinable()) t.join(); threads_.clear(); }

// ==================== Frame send/receive ====================

bool Iec103Master::SendAndRecv(CommIO& io, const uint8_t* req, size_t reqLen, uint8_t* resp, size_t& respLen, int timeoutMs) {
    try { io.write(req, reqLen); } catch (...) { return false; }
    auto start = std::chrono::steady_clock::now();
    size_t pos = 0;
    while (pos < MAX_FRAME) {
        uint8_t byte;
        try {
            size_t n = io.read(&byte, 1);
            if (n == 0) {
                if (pos > 0) break;
                if (std::chrono::steady_clock::now() - start > std::chrono::milliseconds(timeoutMs)) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
        } catch (...) { break; }
        if (pos == 0 && byte != FRAME_START_103) continue;
        resp[pos++] = byte;
        // IEC 103: 68 LEN LEN 68 CTRL ADDR ... CS 16
        if (pos >= 2 && resp[0] == FRAME_START_103) {
            if (pos == 2) {
                if (byte < 4) { pos = 0; continue; } // invalid length
            }
            // Simplified: wait for end byte 0x16
            if (byte == 0x16 && pos >= 6) { respLen = pos; return true; }
        }
    }
    respLen = pos;
    return pos > 0;
}

// ==================== 通道线程 ====================

void Iec103Master::ChannelThread(int chIdx) {
    if (chIdx < 0 || chIdx >= (int)config_.channels.size()) return;
    auto& ch = config_.channels[chIdx];
    int scanMs = ch.scanMs > 0 ? ch.scanMs : 3000;

    std::cout << "[IEC103] Channel " << (chIdx+1) << " start: " << ch.portName << std::endl;

    while (running_) {
        CommIO io;
        try {
            io.open(ch.portName, ch.baud, ch.parity, ch.dataBits, ch.stopBits, ch.timeoutMs);
            std::cout << "[IEC103] " << ch.portName << " opened" << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "[IEC103] " << ch.portName << " open fail: " << e.what() << std::endl;
            for (int i = 0; i < 50 && running_; i++) std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }

        while (running_) {
            try {
                for (size_t di = 0; di < ch.devices.size(); di++) {
                    if (!running_) break;
                    PollDevice(io, ch.devices[di], ch, chIdx, (int)di);
                }
            } catch (const std::exception& e) {
                std::cerr << "[IEC103] comm error: " << e.what() << std::endl;
                break;
            }
            if (running_) {
                for (int i = 0; i < scanMs / 100 && running_; i++)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }

        try { io.close(); } catch (...) {}
        std::cout << "[IEC103] " << ch.portName << " disconnected" << std::endl;
    }
    std::cout << "[IEC103] Channel " << (chIdx+1) << " thread exit" << std::endl;
}

// ==================== 设备轮询 ====================

bool Iec103Master::PollDevice(CommIO& io, const Iec103DeviceConfig& dev, const Iec103ChannelConfig& ch, int chIdx, int devIdx) {
    auto& mgr = RemoteDataMgr::Instance();
    for (auto& ai : dev.aiList) {
        if (!running_) return false;

        // Build query command: simplified FUN+INF request
        uint8_t req[] = {
            FRAME_START_103, 0x09, 0x09, FRAME_START_103, // 68 09 09 68
            0x7B,  // CTRL: request
            (uint8_t)(dev.station & 0xFF), // Address
            0x64,  // ASDU type: C_IC_NA_1
            0x01,  // VSQ
            0x06,  // COT: activation            0x00,
            (uint8_t)(dev.station & 0xFF), // Common address
            (uint8_t)((dev.station >> 8) & 0xFF),
            ai.fun, // FUN
            ai.inf, // INF
            0x00,  // CS
            0x16   // End
        };
        // Fix CS
        uint8_t cs = 0;
        for (size_t i = 0; i < sizeof(req) - 2; i++) cs = (uint8_t)(cs + req[i]);
        req[sizeof(req) - 2] = cs;

        uint8_t resp[MAX_FRAME];
        size_t respLen = 0;
        if (!SendAndRecv(io, req, sizeof(req), resp, respLen, ch.timeoutMs))
            continue;

        if (respLen < 6) continue;

        // 从响应中提取值（简化：假设响应包含原始值）
        double rawVal = 0.0;
        for (size_t i = 4; i < respLen; i++) {
            if (resp[i] == 0x16) break;
            if (resp[i] > 0 && resp[i] < 0xFF)
                rawVal = (double)resp[i];
        }

        double engVal = rawVal * ai.scale + ai.offset;
        mgr.SetAi((uint16_t)(chIdx + 1), (uint16_t)(devIdx + 1), ai.point, engVal);

        if (config_.verbose >= 1)
            std::cout << "[IEC103] AI ch=" << (chIdx+1) << " dev=" << (devIdx+1)
                     << " pt=" << ai.point << " val=" << engVal << std::endl;
    }

    return true;
}

// ==================== Iec103MasterModule ====================

struct Iec103MasterModule::Impl { Iec103Master master; std::string cfgPath; bool loaded = false; bool running = false; };
Iec103MasterModule::Iec103MasterModule() : impl_(std::make_unique<Impl>()) {}
Iec103MasterModule::~Iec103MasterModule() { Stop(); }
bool Iec103MasterModule::LoadConfig(const std::string& cfgPath) { impl_->cfgPath = cfgPath; impl_->loaded = impl_->master.LoadConfig(cfgPath); return impl_->loaded; }
bool Iec103MasterModule::ValidateConfig(const std::string& cfgPath, std::vector<std::string>& errors) { Iec103Master m; if (!m.LoadConfig(cfgPath)) { errors.push_back("Cannot load: " + cfgPath); return false; } return true; }
bool Iec103MasterModule::Start() { if (impl_->running) return true; impl_->running = impl_->master.Start(); return impl_->running; }
void Iec103MasterModule::Stop() { if (!impl_->running) return; impl_->master.Stop(); impl_->running = false; }
bool Iec103MasterModule::IsRunning() const { return impl_->running; }
REGISTER_MODULE("iec103_master", Iec103MasterModule)
