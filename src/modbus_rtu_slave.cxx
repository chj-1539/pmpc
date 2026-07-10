//=============================================================================
// modbus_rtu_slave.cxx — Modbus RTU 从站实现
//
// 串口监听 Modbus RTU 请求
// 支持 FC01/02/03/04/05，点对点映射
//=============================================================================

#include "modbus_rtu_slave.h"
#include "module_factory.h"
#include "ini_reader.h"
#include "str_util.h"
#include "packet_logger.h"
#include <iostream>
#include <cstring>
#include <algorithm>

constexpr size_t MAX_FRAME = 256;
constexpr uint8_t EXCEPTION_MASK = 0x80;
constexpr int RECV_TIMEOUT_MS = 500;

// ==================== CRC16 ====================

uint16_t ModbusRtuSlave::CRC16(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc = static_cast<uint16_t>(crc ^ data[i]);
        for (int j = 0; j < 8; j++) {
            if (crc & 0x0001)
                crc = static_cast<uint16_t>((crc >> 1) ^ 0xA001);
            else
                crc = static_cast<uint16_t>(crc >> 1);
        }
    }
    return crc;
}

bool ModbusRtuSlave::SendRtuFrame(CommIO& io, uint8_t stationId, const uint8_t* pdu, size_t pduLen) {
    uint8_t buf[MAX_FRAME];
    size_t pos = 0;
    buf[pos++] = stationId;
    std::memcpy(buf + pos, pdu, pduLen);
    pos += pduLen;
    uint16_t crc = CRC16(buf, pos);
    buf[pos++] = static_cast<uint8_t>(crc & 0xFF);
    buf[pos++] = static_cast<uint8_t>((crc >> 8) & 0xFF);
    try { io.write(buf, pos); return true; } catch (...) { return false; }
}

// ==================== ModbusRtuSlave ====================

ModbusRtuSlave::ModbusRtuSlave() {}
ModbusRtuSlave::~ModbusRtuSlave() { Stop(); }

bool ModbusRtuSlave::LoadConfig(const std::string& path) {
    IniReader ini; if (!ini.Load(path)) { std::cerr << "[ModbusRtuSlave] 无法打开: " << path << std::endl; return false; }
    config_ = RtuSlaveConfig{};
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

        RtuSlaveDeviceConfig dev;
        dev.stationId = static_cast<uint8_t>(ini.GetInt(sec, "station_id", 0));
        dev.desc = ini.Get(sec, "desc", "");
        if (dev.stationId == 0) continue;

        for (auto& k : ini.Keys(sec)) {
            std::string kl = ToLower(k), v = ini.Get(sec, k, "");
            if (kl == "station_id" || kl == "desc") continue;
            auto kv = ParseKeyValues(v);

            if (StartsWith(kl, "fc01_")) {
                RtuSlaveFC01Entry e{};
                for (auto& [kk, vv] : kv) { auto l = ToLower(kk);
                    if (l == "addr") e.addr = (uint16_t)SafeStoi(vv); else if (l == "ch") e.ch = (uint16_t)SafeStoi(vv);
                    else if (l == "dev") e.dev = (uint16_t)SafeStoi(vv); else if (l == "point") e.point = (uint16_t)SafeStoi(vv); }
                if (e.addr || e.point) dev.fc01Map[e.addr] = e;
            } else if (StartsWith(kl, "fc02_")) {
                RtuSlaveFC02Entry e{};
                for (auto& [kk, vv] : kv) { auto l = ToLower(kk);
                    if (l == "addr") e.addr = (uint16_t)SafeStoi(vv); else if (l == "ch") e.ch = (uint16_t)SafeStoi(vv);
                    else if (l == "dev") e.dev = (uint16_t)SafeStoi(vv); else if (l == "point") e.point = (uint16_t)SafeStoi(vv); }
                if (e.addr || e.point) dev.fc02Map[e.addr] = e;
            } else if (StartsWith(kl, "fc03_")) {
                auto lp = ToLower(v);
                if (lp.find("bit=") != std::string::npos) {
                    RtuSlaveFC34DIEntry e{};
                    for (auto& [kk, vv] : kv) { auto l = ToLower(kk);
                        if (l == "addr") e.addr = (uint16_t)SafeStoi(vv); else if (l == "bit") e.bit = (uint8_t)SafeStoi(vv);
                        else if (l == "ch") e.ch = (uint16_t)SafeStoi(vv); else if (l == "dev") e.dev = (uint16_t)SafeStoi(vv);
                        else if (l == "point") e.point = (uint16_t)SafeStoi(vv); }
                    if (e.addr) dev.fc03DIMap.insert({e.addr, e});
                } else {
                    RtuSlaveFC34AIEntry e{};
                    for (auto& [kk, vv] : kv) { auto l = ToLower(kk);
                        if (l == "addr") e.addr = (uint16_t)SafeStoi(vv); else if (l == "ch") e.ch = (uint16_t)SafeStoi(vv);
                        else if (l == "dev") e.dev = (uint16_t)SafeStoi(vv); else if (l == "point") e.point = (uint16_t)SafeStoi(vv); }
                    if (e.addr) dev.fc03AIMap[e.addr] = e;
                }
            } else if (StartsWith(kl, "fc04_")) {
                auto lp = ToLower(v);
                if (lp.find("bit=") != std::string::npos) {
                    RtuSlaveFC34DIEntry e{};
                    for (auto& [kk, vv] : kv) { auto l = ToLower(kk);
                        if (l == "addr") e.addr = (uint16_t)SafeStoi(vv); else if (l == "bit") e.bit = (uint8_t)SafeStoi(vv);
                        else if (l == "ch") e.ch = (uint16_t)SafeStoi(vv); else if (l == "dev") e.dev = (uint16_t)SafeStoi(vv);
                        else if (l == "point") e.point = (uint16_t)SafeStoi(vv); }
                    if (e.addr) dev.fc04DIMap.insert({e.addr, e});
                } else {
                    RtuSlaveFC34AIEntry e{};
                    for (auto& [kk, vv] : kv) { auto l = ToLower(kk);
                        if (l == "addr") e.addr = (uint16_t)SafeStoi(vv); else if (l == "ch") e.ch = (uint16_t)SafeStoi(vv);
                        else if (l == "dev") e.dev = (uint16_t)SafeStoi(vv); else if (l == "point") e.point = (uint16_t)SafeStoi(vv); }
                    if (e.addr) dev.fc04AIMap[e.addr] = e;
                }
            } else if (StartsWith(kl, "fc05_")) {
                RtuSlaveFC05Entry e{};
                for (auto& [kk, vv] : kv) { auto l = ToLower(kk);
                    if (l == "addr") e.addr = (uint16_t)SafeStoi(vv); else if (l == "ch") e.ch = (uint16_t)SafeStoi(vv);
                    else if (l == "dev") e.dev = (uint16_t)SafeStoi(vv); else if (l == "point") e.point = (uint16_t)SafeStoi(vv);
                    else if (l == "invert") e.invert = (vv == "1" || vv == "true"); }
                if (e.addr || e.point) dev.fc05Map[e.addr] = e;
            }
        }
        config_.devices.push_back(dev);
        if (config_.verbose >= 1)
            std::cout << "[ModbusRtuSlave] " << dev.desc << " station=" << (int)dev.stationId << std::endl;
    }
    if (config_.devices.empty()) { std::cerr << "[ModbusRtuSlave] 无设备" << std::endl; return false; }
    std::cout << "[ModbusRtuSlave] 加载完成: " << config_.portName << " 设备=" << config_.devices.size() << std::endl;
    return true;
}

bool ModbusRtuSlave::Start() {
    if (running_) return false;
    running_ = true;
    portThr_ = std::thread(&ModbusRtuSlave::PortThread, this);
    std::cout << "[ModbusRtuSlave] 启动: " << config_.portName << std::endl; return true;
}

void ModbusRtuSlave::Stop() { running_ = false; if (portThr_.joinable()) portThr_.join(); }

// ==================== 端口线程 ====================

void ModbusRtuSlave::PortThread() {
    while (running_) {
        CommIO io;
        try {
            io.open(config_.portName, config_.baud, config_.parity, config_.dataBits, config_.stopBits, 100);
            std::cout << "[ModbusRtuSlave] " << config_.portName << " 已打开" << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "[ModbusRtuSlave] " << config_.portName << " 打开失败: " << e.what() << std::endl;
            for (int i = 0; i < 50 && running_; i++) std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }

        uint8_t buf[MAX_FRAME];
        size_t pos = 0;
        auto frameStart = std::chrono::steady_clock::now();

        while (running_) {
            uint8_t byte;
            try {
                size_t n = io.read(&byte, 1);
                if (n == 0) { // timeout -> check frame complete
                    if (pos > 0 && std::chrono::steady_clock::now() - frameStart > std::chrono::milliseconds(50))
                        pos = 0; // incomplete frame, discard
                    continue;
                }
            } catch (...) { break; }

            if (pos == 0) frameStart = std::chrono::steady_clock::now();
            buf[pos++] = byte;

            if (pos < 2) continue;

            // Determine expected frame length from PDU (need at least addr+func=2)
            uint8_t func = buf[1];
            size_t expectedLen = 0;

            if (func & 0x80) { // exception: addr+func+exceptionCode+CRC2 = 4
                expectedLen = 4;
            } else {
                switch (func) {
                case 0x01: case 0x02: // addr+func+addr2+qty2+CRC2 = 7
                    if (pos < 4) continue;
                    expectedLen = 7; // actually varies by qty, but min
                    // For read requests: PDU = func(1)+addr(2)+qty(2)=5, +addr(1)+CRC(2)=8
                    expectedLen = 8;
                    break;
                case 0x03: case 0x04: expectedLen = 8; break; // addr+func+addr2+qty2+CRC2 = 8
                case 0x05: expectedLen = 8; break; // addr+func+addr2+val2+CRC2 = 8
                default: expectedLen = 8; break;
                }
            }

            if (pos < expectedLen) continue;

            // Validate CRC
            uint16_t recvCrc = (uint16_t)((uint16_t)buf[pos-2] | ((uint16_t)buf[pos-1] << 8));
            uint16_t calcCrc = CRC16(buf, pos - 2);
            if (recvCrc != calcCrc) { pos = 0; continue; }

            // Process request
            uint8_t stationId = buf[0];
            const uint8_t* pdu = buf + 1;
            size_t pduLen = pos - 3; // -addr -CRC2

            HandleRequest(io, pdu, pduLen, stationId);
            pos = 0;
        }
        try { io.close(); } catch (...) {}
        std::cout << "[ModbusRtuSlave] " << config_.portName << " 断开" << std::endl;
    }
}

// ==================== 请求分发 ====================

void ModbusRtuSlave::HandleRequest(CommIO& io, const uint8_t* pdu, size_t pduLen, uint8_t stationId) {
    // 查找设备
    const RtuSlaveDeviceConfig* dev = nullptr;
    for (auto& d : config_.devices) { if (d.stationId == stationId) { dev = &d; break; } }

    // 记录收到的报文
    uint8_t rtuFrame[MAX_FRAME]; rtuFrame[0] = stationId;
    std::memcpy(rtuFrame + 1, pdu, pduLen);
    auto& pl = PacketLogger::Instance();
    pl.Log(PktDir::RX, 0, 0, stationId, pdu[0], 0, rtuFrame, pduLen + 1);

    if (!dev) {
        uint8_t errPdu[] = { static_cast<uint8_t>(pdu[0] | EXCEPTION_MASK), 0x02 };
        SendRtuFrame(io, stationId, errPdu, sizeof(errPdu));
        pl.Log(PktDir::TX, 0, 0, stationId, pdu[0] | EXCEPTION_MASK, 0, errPdu, sizeof(errPdu));
        return;
    }

    uint8_t func = pdu[0];
    std::vector<uint8_t> response;
    bool ok = false;

    switch (func) {
    case 0x01: ok = HandleFC01(*dev, pdu, pduLen, response); break;
    case 0x02: ok = HandleFC02(*dev, pdu, pduLen, response); break;
    case 0x03: ok = HandleFC03(*dev, pdu, pduLen, response); break;
    case 0x04: ok = HandleFC04(*dev, pdu, pduLen, response); break;
    case 0x05: ok = HandleFC05(*dev, pdu, pduLen, response); break;
    default: {
        uint8_t errPdu[] = { static_cast<uint8_t>(func | EXCEPTION_MASK), 0x01 };
        SendRtuFrame(io, stationId, errPdu, sizeof(errPdu));
        pl.Log(PktDir::TX, 0, 0, stationId, func | EXCEPTION_MASK, 0, errPdu, sizeof(errPdu));
        return;
    }
    }

    if (ok) {
        SendRtuFrame(io, stationId, response.data(), response.size());
        pl.Log(PktDir::TX, 0, 0, stationId, func, 0, response.data(), response.size());
    } else if (func == 0x05) {
        uint8_t errPdu[] = { static_cast<uint8_t>(func | EXCEPTION_MASK), 0x02 };
        SendRtuFrame(io, stationId, errPdu, sizeof(errPdu));
        pl.Log(PktDir::TX, 0, 0, stationId, func | EXCEPTION_MASK, 0, errPdu, sizeof(errPdu));
    }
}

// ==================== 功能码处理 (复用 modbus_tcp_slave 逻辑) ====================

bool ModbusRtuSlave::HandleFC01(const RtuSlaveDeviceConfig& dev, const uint8_t* pdu, size_t, std::vector<uint8_t>& resp) {
    uint16_t startAddr = static_cast<uint16_t>((static_cast<uint16_t>(pdu[1]) << 8) | pdu[2]);
    uint16_t qty = static_cast<uint16_t>((static_cast<uint16_t>(pdu[3]) << 8) | pdu[4]);
    if (qty == 0 || qty > 2000) return false;
    uint16_t byteCount = (uint16_t)((qty + 7) / 8);
    std::vector<uint8_t> data(byteCount, 0);
    auto& mgr = RemoteDataMgr::Instance();
    for (uint16_t i = 0; i < qty; i++) {
        auto it = dev.fc01Map.find((uint16_t)(startAddr + i));
        if (it == dev.fc01Map.end()) continue;
        DiPoint pt; if (!mgr.GetDi(it->second.ch, it->second.dev, it->second.point, pt)) continue;
        if (pt.value) data[i / 8] |= (uint8_t)(1 << (i % 8));
    }
    resp.clear(); resp.push_back(0x01); resp.push_back((uint8_t)byteCount);
    resp.insert(resp.end(), data.begin(), data.end());
    return true;
}

bool ModbusRtuSlave::HandleFC02(const RtuSlaveDeviceConfig& dev, const uint8_t* pdu, size_t, std::vector<uint8_t>& resp) {
    uint16_t startAddr = static_cast<uint16_t>((static_cast<uint16_t>(pdu[1]) << 8) | pdu[2]);
    uint16_t qty = static_cast<uint16_t>((static_cast<uint16_t>(pdu[3]) << 8) | pdu[4]);
    if (qty == 0 || qty > 2000) return false;
    uint16_t byteCount = (uint16_t)((qty + 7) / 8);
    std::vector<uint8_t> data(byteCount, 0);
    auto& mgr = RemoteDataMgr::Instance();
    for (uint16_t i = 0; i < qty; i++) {
        auto it = dev.fc02Map.find((uint16_t)(startAddr + i));
        if (it == dev.fc02Map.end()) continue;
        DiPoint pt; if (!mgr.GetDi(it->second.ch, it->second.dev, it->second.point, pt)) continue;
        if (pt.value) data[i / 8] |= (uint8_t)(1 << (i % 8));
    }
    resp.clear(); resp.push_back(0x02); resp.push_back((uint8_t)byteCount);
    resp.insert(resp.end(), data.begin(), data.end());
    return true;
}

bool ModbusRtuSlave::HandleFC03(const RtuSlaveDeviceConfig& dev, const uint8_t* pdu, size_t pduLen, std::vector<uint8_t>& resp) {
    return HandleFCReadRegs(pdu, pduLen, resp, 0x03, dev.fc03AIMap, dev.fc03DIMap);
}

bool ModbusRtuSlave::HandleFC04(const RtuSlaveDeviceConfig& dev, const uint8_t* pdu, size_t pduLen, std::vector<uint8_t>& resp) {
    return HandleFCReadRegs(pdu, pduLen, resp, 0x04, dev.fc04AIMap, dev.fc04DIMap);
}

bool ModbusRtuSlave::HandleFCReadRegs(const uint8_t* pdu, size_t, std::vector<uint8_t>& resp, uint8_t func,
                                        const std::map<uint16_t, RtuSlaveFC34AIEntry>& aiMap,
                                        const std::multimap<uint16_t, RtuSlaveFC34DIEntry>& diMap) {
    uint16_t startAddr = static_cast<uint16_t>((static_cast<uint16_t>(pdu[1]) << 8) | pdu[2]);
    uint16_t qty = static_cast<uint16_t>((static_cast<uint16_t>(pdu[3]) << 8) | pdu[4]);
    if (qty == 0 || qty > 125) return false;
    uint16_t byteCount = (uint16_t)(qty * 2);
    std::vector<uint8_t> data(byteCount, 0);
    auto& mgr = RemoteDataMgr::Instance();

    // AI
    for (auto it = aiMap.lower_bound(startAddr); it != aiMap.end(); ++it) {
        if (it->first >= startAddr + qty) break;
        const auto& ai = it->second;
        int regCnt = DataConvert::GetRegCount(ai.dtype);
        AiPoint pt; if (!mgr.GetAi(ai.ch, ai.dev, ai.point, pt)) continue;
        double rawVal = pt.value;
        double unused;
        // Store as uint16 big-endian (simplified: UInt16 only)
        (void)unused;
        uint16_t regVal = (uint16_t)rawVal;
        for (int r = 0; r < regCnt; r++) {
            int off = (int)(it->first - startAddr + r) * 2;
            if (off + 1 >= (int)byteCount) break;
            data[off] = (uint8_t)((regVal >> 8) & 0xFF);
            data[off + 1] = (uint8_t)(regVal & 0xFF);
        }
    }

    // DI (bit)
    for (uint16_t i = 0; i < qty; i++) {
        uint16_t addr = (uint16_t)(startAddr + i);
        auto [rb, re] = diMap.equal_range(addr);
        if (rb == re) continue;
        uint16_t regVal = static_cast<uint16_t>((static_cast<uint16_t>(data[i * 2]) << 8) | data[i * 2 + 1]);
        for (auto it = rb; it != re; ++it) {
            DiPoint pt; if (!mgr.GetDi(it->second.ch, it->second.dev, it->second.point, pt)) continue;
            uint16_t m = static_cast<uint16_t>(1 << it->second.bit);
            if (pt.value) regVal = static_cast<uint16_t>(regVal | m);
            else regVal = static_cast<uint16_t>(regVal & static_cast<uint16_t>(~m));
        }
        data[i * 2] = (uint8_t)((regVal >> 8) & 0xFF);
        data[i * 2 + 1] = (uint8_t)(regVal & 0xFF);
    }

    resp.clear(); resp.push_back(func); resp.push_back((uint8_t)byteCount);
    resp.insert(resp.end(), data.begin(), data.end());
    return true;
}

bool ModbusRtuSlave::HandleFC05(const RtuSlaveDeviceConfig& dev, const uint8_t* pdu, size_t, std::vector<uint8_t>& resp) {
    uint16_t addr = static_cast<uint16_t>((static_cast<uint16_t>(pdu[1]) << 8) | pdu[2]);
    auto it = dev.fc05Map.find(addr);
    if (it == dev.fc05Map.end()) return false;
    const auto& e = it->second;
    bool coilVal = (pdu[3] == 0xFF);
    if (e.invert) coilVal = !coilVal;
    RemoteDataMgr::Instance().SetDoMaster(e.ch, e.dev, e.point, coilVal);
    resp.assign(pdu, pdu + 5);
    return true;
}

// ==================== ModbusRtuSlaveModule ====================

struct ModbusRtuSlaveModule::Impl { ModbusRtuSlave slave; std::string cfgPath; bool loaded = false; bool running = false; };
ModbusRtuSlaveModule::ModbusRtuSlaveModule() : impl_(std::make_unique<Impl>()) {}
ModbusRtuSlaveModule::~ModbusRtuSlaveModule() { Stop(); }
bool ModbusRtuSlaveModule::LoadConfig(const std::string& cfgPath) { impl_->cfgPath = cfgPath; impl_->loaded = impl_->slave.LoadConfig(cfgPath); return impl_->loaded; }
bool ModbusRtuSlaveModule::ValidateConfig(const std::string& cfgPath, std::vector<std::string>& errors) { ModbusRtuSlave s; if (!s.LoadConfig(cfgPath)) { errors.push_back("Cannot load: " + cfgPath); return false; } return true; }
bool ModbusRtuSlaveModule::Start() { if (impl_->running) return true; impl_->running = impl_->slave.Start(); return impl_->running; }
void ModbusRtuSlaveModule::Stop() { if (!impl_->running) return; impl_->slave.Stop(); impl_->running = false; }
bool ModbusRtuSlaveModule::IsRunning() const { return impl_->running; }
REGISTER_MODULE("modbus_rtu_slave", ModbusRtuSlaveModule)
