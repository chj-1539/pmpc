//=============================================================================
// iec101_slave.cxx - IEC 60870-5-101 slave implementation
//
// Serial port listening for master requests
// Supports GI response, remote control, clock sync
// DI change via EventBus upload
//=============================================================================

#include "iec101_slave.h"
#include "packet_logger.h"
#include "module_factory.h"
#include "ini_reader.h"
#include "str_util.h"
#include <iostream>
#include <cstring>

constexpr size_t MAX_FRAME = 256;
constexpr int RECV_TIMEOUT_MS = 200;
constexpr uint8_t IEC101_START_VAR = 0x68;
constexpr uint8_t IEC101_START_FIX = 0x10;
constexpr uint8_t IEC101_END = 0x16;

int Iec101Slave::SafeStoi(const std::string& s, int def) { try { return std::stoi(s); } catch (...) { return def; } }
uint8_t Iec101Slave::CalcCS(const uint8_t* data, size_t len) {
    uint8_t cs = 0; for (size_t i = 0; i < len; i++) cs = (uint8_t)(cs + data[i]); return cs;
}

Iec101Slave::Iec101Slave() {}
Iec101Slave::~Iec101Slave() { Stop(); }

bool Iec101Slave::LoadConfig(const std::string& path) {
    IniReader ini; if (!ini.Load(path)) { std::cerr << "[Iec101Slave] Cannot open: " << path << std::endl; return false; }
    config_ = Slave101Config{};
    config_.portName = ini.Get("global", "port_name", "COM1");
    config_.baud = ini.GetInt("global", "baud", 9600);
    config_.parity = ToLower(ini.Get("global", "parity", "even"));
    config_.dataBits = ini.GetInt("global", "data_bits", 8);
    config_.stopBits = ini.GetInt("global", "stop_bits", 1);
    config_.verbose = ini.GetInt("global", "verbose", 1);

    for (auto& sec : ini.Sections()) {
        std::string low = ToLower(sec);
        if (low == "global") continue;
        if (!StartsWith(low, "device_")) continue;

        Slave101DeviceConfig dev;
        dev.linkAddr = (uint16_t)ini.GetInt(sec, "link_addr", 1);
        dev.coa = (uint16_t)ini.GetInt(sec, "common_addr", 1);
        dev.desc = ini.Get(sec, "desc", "");
        for (auto& k : ini.Keys(sec)) {
            std::string kl = ToLower(k), v = ini.Get(sec, k, "");
            if (kl == "link_addr" || kl == "common_addr" || kl == "desc") continue;
            if (StartsWith(kl, "di_")) {
                auto kv = ParseKeyValues(v); Slave101DIMapping di{};
                for (auto& [kk, vv] : kv) { auto l = ToLower(kk);
                    if (l == "ioa") di.ioa = (uint32_t)SafeStoi(vv); else if (l == "ch") di.ch = (uint16_t)SafeStoi(vv);
                    else if (l == "dev") di.dev = (uint16_t)SafeStoi(vv); else if (l == "point") di.point = (uint16_t)SafeStoi(vv); }
                if (di.ioa) dev.diMap[di.ioa] = di;
            } else if (StartsWith(kl, "ai_")) {
                auto kv = ParseKeyValues(v); Slave101AIMapping ai{};
                for (auto& [kk, vv] : kv) { auto l = ToLower(kk);
                    if (l == "ioa") ai.ioa = (uint32_t)SafeStoi(vv); else if (l == "ch") ai.ch = (uint16_t)SafeStoi(vv);
                    else if (l == "dev") ai.dev = (uint16_t)SafeStoi(vv); else if (l == "point") ai.point = (uint16_t)SafeStoi(vv);
                    else if (l == "scale") ai.scale = SafeStod(vv, 1.0); else if (l == "offset") ai.offset = SafeStod(vv, 0.0); }
                if (ai.ioa) dev.aiMap[ai.ioa] = ai;
            } else if (StartsWith(kl, "do_")) {
                auto kv = ParseKeyValues(v); Slave101DOMapping do_{};
                for (auto& [kk, vv] : kv) { auto l = ToLower(kk);
                    if (l == "ioa") do_.ioa = (uint32_t)SafeStoi(vv); else if (l == "val") do_.val = SafeStoi(vv);
                    else if (l == "ch") do_.ch = (uint16_t)SafeStoi(vv); else if (l == "dev") do_.dev = (uint16_t)SafeStoi(vv);
                    else if (l == "point") do_.point = (uint16_t)SafeStoi(vv); }
                if (do_.ioa) dev.doMap[do_.ioa].push_back(do_);
            }
        }
        config_.devices.push_back(dev);
    }
    if (config_.devices.empty()) { std::cerr << "[Iec101Slave] No devices" << std::endl; return false; }
    std::cout << "[Iec101Slave] Loaded " << config_.devices.size() << " devices" << std::endl;
    return true;
}

bool Iec101Slave::Start() {
    if (running_) return false;
    running_ = true;
    portThr_ = std::thread(&Iec101Slave::PortThread, this);
    tokenDI_ = EventBus::Subscribe<DIChange>([this](const DIChange&) {});
    std::cout << "[Iec101Slave] Started" << std::endl; return true;
}

void Iec101Slave::Stop() { running_ = false; EventBus::Unsubscribe<DIChange>(tokenDI_); if (portThr_.joinable()) portThr_.join(); }

void Iec101Slave::SendACK(CommIO& io, uint16_t linkAddr) {
    uint8_t frame[] = { IEC101_START_FIX, 0x03, (uint8_t)(linkAddr & 0xFF), 0x00, IEC101_END };
    frame[3] = CalcCS(frame + 1, 2);
    try { io.write(frame, sizeof(frame)); } catch (...) {}
}

void Iec101Slave::SendGIRsp(CommIO& io, uint16_t linkAddr, uint16_t coa) {
    uint8_t asdu[512]; auto& mgr = RemoteDataMgr::Instance();
    for (auto& dev : config_.devices) {
        if (dev.linkAddr != linkAddr || dev.coa != coa) continue;
        for (auto& [ioa, di] : dev.diMap) {
            size_t pos = 0; DiPoint pt;
            asdu[pos++] = IecType::M_SP_NA_1; asdu[pos++] = 0x01;
            asdu[pos++] = IecCOT::BACKGROUND; asdu[pos++] = 0x00;
            asdu[pos++] = (uint8_t)(coa & 0xFF); asdu[pos++] = (uint8_t)((coa >> 8) & 0xFF);
            asdu[pos++] = (uint8_t)(ioa & 0xFF); asdu[pos++] = (uint8_t)((ioa >> 8) & 0xFF); asdu[pos++] = (uint8_t)((ioa >> 16) & 0xFF);
            asdu[pos++] = (mgr.GetDi(di.ch, di.dev, di.point, pt) && pt.value) ? 0x01 : 0x00;
            uint8_t frame[MAX_FRAME]; size_t fp = 0;
            // CR-6: LEN = CTRL(1) + ADDR(2) + asduLen = 3 + asduLen（不是 4）
            uint8_t len = (uint8_t)(3 + pos);
            frame[fp++] = IEC101_START_VAR; frame[fp++] = len; frame[fp++] = len; frame[fp++] = IEC101_START_VAR;
            frame[fp++] = 0x0B; frame[fp++] = (uint8_t)(linkAddr & 0xFF); frame[fp++] = (uint8_t)((linkAddr >> 8) & 0xFF);
            std::memcpy(frame + fp, asdu, pos); fp += pos;
            uint8_t cs = CalcCS(frame + 4, fp - 4); frame[fp++] = cs; frame[fp++] = IEC101_END;
            try { io.write(frame, fp); } catch (...) { return; }
        }
    }
    // activation termination
    uint8_t termAsdu[] = { IecType::C_IC_NA_1, 0x01, IecCOT::ACTIVATION_TERM, 0x00,
        (uint8_t)(coa & 0xFF), (uint8_t)((coa >> 8) & 0xFF), 0x00, 0x00, 0x00, 0x14 };
    uint8_t frame[MAX_FRAME]; size_t fp = 0;
    // CR-6: LEN = 3 + asduLen
    uint8_t len = (uint8_t)(3 + sizeof(termAsdu));
    frame[fp++] = IEC101_START_VAR; frame[fp++] = len; frame[fp++] = len; frame[fp++] = IEC101_START_VAR;
    frame[fp++] = 0x0B; frame[fp++] = (uint8_t)(linkAddr & 0xFF); frame[fp++] = (uint8_t)((linkAddr >> 8) & 0xFF);
    std::memcpy(frame + fp, termAsdu, sizeof(termAsdu)); fp += sizeof(termAsdu);
    uint8_t cs = CalcCS(frame + 4, fp - 4); frame[fp++] = cs; frame[fp++] = IEC101_END;
    try { io.write(frame, fp); } catch (...) {}
}

bool Iec101Slave::FindAndExecDO(const uint8_t* asdu, size_t len) {
    if (len < 9) return false;
    uint32_t ioa = (uint32_t)((uint32_t)asdu[6] | ((uint32_t)asdu[7] << 8) | ((uint32_t)asdu[8] << 16));
    int cmdVal = (len > 9) ? (asdu[9] & 0x01) : 0;
    for (auto& dev : config_.devices) {
        auto it = dev.doMap.find(ioa);
        if (it == dev.doMap.end()) continue;
        for (auto& do_ : it->second) {
            if (do_.val == cmdVal) {
                RemoteDataMgr::Instance().SetDoMaster(do_.ch, do_.dev, do_.point, cmdVal != 0);
                return true;
            }
        }
    }
    return false;
}

void Iec101Slave::HandleFrame(CommIO& io, const uint8_t* buf, size_t len) {
    if (len < 5) return;

    // CR-5（第二轮）修复：分帧类型再取 CTRL。老代码统一 `ctrl = buf[1]`，
    // 但可变帧 `[0x68 LEN LEN 0x68 CTRL ...]` 里 buf[1] 是 LEN，不是 CTRL。
    // 如果 LEN 低 4 位刚好等于 0 → 误发 Reset ACK；等于 A/B → 误发 Class1
    // 数据；主站永远收不到 GI/遥控 confirm。
    uint16_t linkAddr = 0;
    uint8_t  ctrl = 0;
    if (buf[0] == IEC101_START_FIX) {
        // 固定帧: [0x10 CTRL ADDR_L (ADDR_H) CS END] — 单/双字节 link addr
        // 我们只支持双字节 addr（与 SendACK / SendGIRsp 一致）
        if (len < 6) return;
        ctrl = buf[1];
        linkAddr = (uint16_t)((uint16_t)buf[2] | ((uint16_t)buf[3] << 8));
    } else if (buf[0] == IEC101_START_VAR) {
        // 可变帧: [0x68 LEN LEN 0x68 CTRL ADDR_L ADDR_H ASDU... CS END]
        if (len < 9) return;
        ctrl = buf[4];
        linkAddr = (uint16_t)((uint16_t)buf[5] | ((uint16_t)buf[6] << 8));
    } else {
        return;   // 非法起始符
    }
    uint8_t fun = ctrl & 0x0F;

    // 固定帧的功能码分支只对固定帧生效
    if (buf[0] == IEC101_START_FIX) {
        if (fun == 0x00) { SendACK(io, linkAddr); return; } // reset
        if (fun == 0x0A || fun == 0x0B) { // REQ_1D / REQ_2D
            // simplified: send one DI point as class 1 data
            if (!config_.devices.empty()) {
                uint8_t asdu[32]; size_t pos = 0; auto& dev = config_.devices[0];
                asdu[pos++] = IecType::M_SP_NA_1; asdu[pos++] = 0x01;
                asdu[pos++] = IecCOT::BACKGROUND; asdu[pos++] = 0x00;
                asdu[pos++] = (uint8_t)(dev.coa & 0xFF); asdu[pos++] = (uint8_t)((dev.coa >> 8) & 0xFF);
                asdu[pos++] = 0x01; asdu[pos++] = 0x00; asdu[pos++] = 0x00; asdu[pos++] = 0x00;

                uint8_t frame[MAX_FRAME]; size_t fp = 0;
                // CR-6: LEN = CTRL(1) + ADDR(2) + asduLen = 3 + asduLen（不是 4）
                uint8_t l = (uint8_t)(3 + pos);
                frame[fp++] = IEC101_START_VAR; frame[fp++] = l; frame[fp++] = l; frame[fp++] = IEC101_START_VAR;
                frame[fp++] = 0x0B; frame[fp++] = (uint8_t)(linkAddr & 0xFF); frame[fp++] = (uint8_t)((linkAddr >> 8) & 0xFF);
                std::memcpy(frame + fp, asdu, pos); fp += pos;
                uint8_t cs = CalcCS(frame + 4, fp - 4); frame[fp++] = cs; frame[fp++] = IEC101_END;
                try { io.write(frame, fp); } catch (...) {}
            }
            return;
        }
        // 其他固定帧 fun 暂不处理
        return;
    }

    // variable frame with ASDU（buf[0] == IEC101_START_VAR，L11 已保证 LEN≥4）
    if (len >= 7) {
        const uint8_t* asdu = buf + 7;
        size_t asduLen = len - 7 - 2;
        if (asduLen < 4) return;
        uint16_t coa = (uint16_t)((uint16_t)asdu[4] | ((uint16_t)asdu[5] << 8));
        uint8_t type = asdu[0];

        if (type == IecType::C_IC_NA_1) {
            // activation confirm
            uint8_t confAsdu[] = { IecType::C_IC_NA_1, 0x01, IecCOT::ACTIVATION_CON, 0x00,
                (uint8_t)(coa & 0xFF), (uint8_t)((coa >> 8) & 0xFF), 0x00, 0x00, 0x00, 0x14 };
            uint8_t f[MAX_FRAME]; size_t fp = 0;
            // CR-6: LEN = 3 + asduLen
            uint8_t l = (uint8_t)(3 + sizeof(confAsdu));
            f[fp++] = IEC101_START_VAR; f[fp++] = l; f[fp++] = l; f[fp++] = IEC101_START_VAR;
            f[fp++] = 0x0B; f[fp++] = (uint8_t)(linkAddr & 0xFF); f[fp++] = (uint8_t)((linkAddr >> 8) & 0xFF);
            std::memcpy(f + fp, confAsdu, sizeof(confAsdu)); fp += sizeof(confAsdu);
            uint8_t cs = CalcCS(f + 4, fp - 4); f[fp++] = cs; f[fp++] = IEC101_END;
            try { io.write(f, fp); } catch (...) {}
            SendGIRsp(io, linkAddr, coa);
        } else if (type == IecType::C_SC_NA_1 || type == IecType::C_DC_NA_1) {
            // remote control
            FindAndExecDO(asdu, asduLen);
            // activation confirm
            uint8_t confAsdu[16]; size_t cp = 0;
            std::memcpy(confAsdu, asdu, asduLen < 16 ? asduLen : 16);
            confAsdu[2] = IecCOT::ACTIVATION_CON; cp = asduLen < 16 ? asduLen : 16;
            uint8_t f[MAX_FRAME]; size_t fp = 0;
            // CR-6: LEN = 3 + asduLen
            uint8_t l = (uint8_t)(3 + cp);
            f[fp++] = IEC101_START_VAR; f[fp++] = l; f[fp++] = l; f[fp++] = IEC101_START_VAR;
            f[fp++] = 0x0B; f[fp++] = (uint8_t)(linkAddr & 0xFF); f[fp++] = (uint8_t)((linkAddr >> 8) & 0xFF);
            std::memcpy(f + fp, confAsdu, cp); fp += cp;
            uint8_t cs = CalcCS(f + 4, fp - 4); f[fp++] = cs; f[fp++] = IEC101_END;
            try { io.write(f, fp); } catch (...) {}
        }
    }
}

void Iec101Slave::PortThread() {
    std::cout << "[Iec101Slave] Port thread start: " << config_.portName << std::endl;
    while (running_) {
        CommIO io;
        try {
            io.open(config_.portName, config_.baud, config_.parity, config_.dataBits, config_.stopBits, RECV_TIMEOUT_MS);
            std::cout << "[Iec101Slave] " << config_.portName << " opened" << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "[Iec101Slave] " << config_.portName << " open fail: " << e.what() << std::endl;
            for (int i = 0; i < 50 && running_; i++) std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }

        uint8_t buf[MAX_FRAME]; size_t pos = 0;
        while (running_) {
            uint8_t byte;
            try {
                size_t n = io.read(&byte, 1);
                if (n == 0) { if (pos > 0) pos = 0; continue; }
            } catch (...) { break; }
            if (pos == 0 && byte != IEC101_START_VAR && byte != IEC101_START_FIX) continue;
            buf[pos++] = byte;
            if      (IsCompleteFixedFrame(buf, pos))    { HandleFrame(io, buf, pos); pos = 0; }
            else if (IsCompleteVariableFrame(buf, pos)) { HandleFrame(io, buf, pos); pos = 0; }
            else if (pos >= MAX_FRAME)                  { pos = 0; }   // 缓冲区溢出保护
        }
        try { io.close(); } catch (...) {}
        std::cout << "[Iec101Slave] " << config_.portName << " disconnected" << std::endl;
    }
}

REGISTER_MODULE("iec101_slave", Iec101SlaveModule)

struct Iec101SlaveModule::Impl { Iec101Slave slave; std::string cfgPath; bool loaded = false; bool running = false; };
Iec101SlaveModule::Iec101SlaveModule() : impl_(std::make_unique<Impl>()) {}
Iec101SlaveModule::~Iec101SlaveModule() { Stop(); }
bool Iec101SlaveModule::LoadConfig(const std::string& cfgPath) { impl_->cfgPath = cfgPath; impl_->loaded = impl_->slave.LoadConfig(cfgPath); return impl_->loaded; }
bool Iec101SlaveModule::ValidateConfig(const std::string& cfgPath, std::vector<std::string>& errors) { Iec101Slave s; if (!s.LoadConfig(cfgPath)) { errors.push_back("Cannot load: " + cfgPath); return false; } return true; }
bool Iec101SlaveModule::Start() { if (impl_->running) return true; impl_->running = impl_->slave.Start(); return impl_->running; }
void Iec101SlaveModule::Stop() { if (!impl_->running) return; impl_->slave.Stop(); impl_->running = false; }
bool Iec101SlaveModule::IsRunning() const { return impl_->running; }
