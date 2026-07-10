//=============================================================================
// modbus_rtu_master.cxx — Modbus RTU 主站采集实现
//
// 串口通信 + Modbus RTU 协议 (address + function + data + CRC16)
// 模板继承 + 地址合并优化
//=============================================================================

#include "modbus_rtu_master.h"
#include "module_factory.h"
#include "str_util.h"
#include "ini_reader.h"
#include "comm_io.h"
#include "packet_logger.h"
#include <iostream>
#include <sstream>
#include <cstring>
#include <algorithm>
#include <chrono>
#include <cmath>

constexpr size_t MAX_FRAME = 256;
constexpr uint8_t EXCEPTION_MASK = 0x80;

// ==================== CRC16 (Modbus) ====================

uint16_t ModbusRtuMaster::CRC16(const uint8_t* data, size_t len) {
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

// ==================== 帧构建/解析 ====================

bool ModbusRtuMaster::BuildRtuFrame(uint8_t station, const uint8_t* pdu, size_t pduLen,
                                      uint8_t* frame, size_t& frameLen) {
    if (pduLen + 3 > MAX_FRAME) return false;
    frame[0] = station;
    std::memcpy(frame + 1, pdu, pduLen);
    size_t crcPos = 1 + pduLen;
    uint16_t crc = CRC16(frame, crcPos);
    frame[crcPos] = static_cast<uint8_t>(crc & 0xFF);
    frame[crcPos + 1] = static_cast<uint8_t>((crc >> 8) & 0xFF);
    frameLen = crcPos + 2;
    return true;
}

bool ModbusRtuMaster::ParseRtuResponse(const uint8_t* frame, size_t frameLen,
                                         uint8_t& func, uint8_t* data, size_t& dataLen) {
    if (frameLen < 4) return false;
    uint16_t recvCrc = static_cast<uint16_t>(static_cast<uint16_t>(frame[frameLen - 2])
                     | (static_cast<uint16_t>(frame[frameLen - 1]) << 8));
    uint16_t calcCrc = CRC16(frame, frameLen - 2);
    if (recvCrc != calcCrc) return false;
    func = frame[1];
    size_t payloadLen = frameLen - 3;
    std::memcpy(data, frame + 1, payloadLen);
    dataLen = payloadLen;
    return true;
}

// ==================== ModbusRtuMaster ====================

ModbusRtuMaster::ModbusRtuMaster() {}
ModbusRtuMaster::~ModbusRtuMaster() { Stop(); }

// ==================== 配置加载 ====================

bool ModbusRtuMaster::LoadConfig(const std::string& path) {
    IniReader ini;
    if (!ini.Load(path)) { std::cerr << "[ModbusRtuMaster] Cannot open: " << path << std::endl; return false; }
    config_ = RtuMasterConfig{};
    templates_.clear();

    auto sections = ini.Sections();
    std::string curSectionType;
    RtuPortConfig currentPort;
    DeviceConfig currentTemplate;
    bool inPort = false;

    for (const auto& sec : sections) {
        std::string secLower = ToLower(sec);
        if (secLower == "global") {
            for (auto& k : ini.Keys(sec)) ParseGlobal(k, ini.Get(sec, k, ""));
            continue;
        }
        if (StartsWith(secLower, "port_")) {
            if (inPort && !currentPort.portName.empty()) config_.ports.push_back(currentPort);
            currentPort = RtuPortConfig{}; inPort = true;
            for (auto& k : ini.Keys(sec)) ParsePort(k, ini.Get(sec, k, ""), currentPort);
            continue;
        }
        if (StartsWith(secLower, "template_")) {
            currentTemplate = DeviceConfig{}; inPort = false;
            std::string tplName = sec.substr(9);
            for (auto& k : ini.Keys(sec)) {
                auto v = ini.Get(sec, k, "");
                if (StartsWith(k, "di_") || StartsWith(k, "ai_") || StartsWith(k, "do_") || StartsWith(k, "ao_"))
                    ParseTemplateEntry(k, v, currentTemplate);
            }
            templates_[tplName] = currentTemplate;
            continue;
        }
        if (StartsWith(secLower, "device_") && inPort) {
            DeviceConfig dev; int portIdx = static_cast<int>(config_.ports.size());
            std::string templateName;
            for (auto& k : ini.Keys(sec)) {
                auto v = ini.Get(sec, k, ""); auto kl = ToLower(k);
                if (kl == "port") portIdx = std::stoi(v) - 1;
                else if (kl == "station_id") dev.stationId = static_cast<uint8_t>(std::stoi(v));
                else if (kl == "template") templateName = v;
                else if (kl == "desc") dev.name = v;
                else if (StartsWith(k, "di_") || StartsWith(k, "ai_") || StartsWith(k, "do_") || StartsWith(k, "ao_"))
                    ParseDeviceEntry(k, v, dev);
            }
            auto it = templates_.find(templateName);
            if (it != templates_.end()) {
                dev.templateName = templateName;
                dev.diList.insert(dev.diList.begin(), it->second.diList.begin(), it->second.diList.end());
                dev.aiList.insert(dev.aiList.begin(), it->second.aiList.begin(), it->second.aiList.end());
                dev.doList.insert(dev.doList.begin(), it->second.doList.begin(), it->second.doList.end());
                dev.aoList.insert(dev.aoList.begin(), it->second.aoList.begin(), it->second.aoList.end());
            }
            dev.channel = static_cast<uint8_t>(portIdx + 1);
            if (portIdx >= 0 && portIdx < static_cast<int>(config_.ports.size()))
                config_.ports[portIdx].devices.push_back(dev);
            continue;
        }
    }
    if (inPort && !currentPort.portName.empty()) config_.ports.push_back(currentPort);
    if (config_.ports.empty()) { std::cerr << "[ModbusRtuMaster] No serial ports" << std::endl; return false; }
    int devCount = 0;
    for (auto& p : config_.ports) devCount += static_cast<int>(p.devices.size());
    std::cout << "[ModbusRtuMaster] Loaded " << config_.ports.size() << " ports " << devCount << " devices" << std::endl;
    return true;
}

bool ModbusRtuMaster::ParseGlobal(const std::string& key, const std::string& val) {
    auto kl = ToLower(key);
    if (kl == "timeout_ms") config_.timeoutMs = std::stoi(val);
    else if (kl == "retry_count") config_.retryCount = std::stoi(val);
    else if (kl == "retry_sleep_ms") config_.retrySleepMs = std::stoi(val);
    else if (kl == "verbose") config_.verbose = std::stoi(val);
    else if (kl == "max_di_read") config_.maxDiRead = std::stoi(val);
    else if (kl == "max_ai_read") config_.maxAiRead = std::stoi(val);
    return true;
}

bool ModbusRtuMaster::ParsePort(const std::string& key, const std::string& val, RtuPortConfig& pc) {
    auto kl = ToLower(key);
    if (kl == "port_name") pc.portName = val;
    else if (kl == "baud") pc.baud = std::stoi(val);
    else if (kl == "parity") pc.parity = ToLower(val);
    else if (kl == "data_bits") pc.dataBits = std::stoi(val);
    else if (kl == "stop_bits") pc.stopBits = std::stoi(val);
    else if (kl == "scan_ms") pc.scanMs = std::stoi(val);
    else if (kl == "timeout_ms") pc.timeoutMs = std::stoi(val);
    else if (kl == "retry_count") pc.retryCount = std::stoi(val);
    else if (kl == "retry_sleep_ms") pc.retrySleepMs = std::stoi(val);
    else if (kl == "verbose") pc.verbose = std::stoi(val);
    else if (kl == "max_di_read") pc.maxDiRead = std::stoi(val);
    else if (kl == "max_ai_read") pc.maxAiRead = std::stoi(val);
    else if (kl == "keep_alive_addr") pc.keepAliveAddr = std::stoi(val);
    if (pc.timeoutMs <= 0) pc.timeoutMs = config_.timeoutMs;
    if (pc.retryCount <= 0) pc.retryCount = config_.retryCount;
    if (pc.retrySleepMs <= 0) pc.retrySleepMs = config_.retrySleepMs;
    if (pc.verbose == 0 && config_.verbose > 0) pc.verbose = config_.verbose;
    if (pc.maxDiRead <= 0) pc.maxDiRead = config_.maxDiRead;
    if (pc.maxAiRead <= 0) pc.maxAiRead = config_.maxAiRead;
    return true;
}

bool ModbusRtuMaster::ParseTemplateEntry(const std::string& prefix, const std::string& params, DeviceConfig& tmpl) {
    auto kv = ParseKeyValues(params);
    if (kv.empty()) return false;
    if (StartsWith(prefix, "di_")) {
        DIMapping di{};
        for (auto& [k, v] : kv) { auto lk = ToLower(k);
            if (lk == "addr") di.addr = static_cast<uint16_t>(std::stoi(v));
            else if (lk == "func") di.func = static_cast<uint8_t>(std::stoi(v));
            else if (lk == "qty") di.qty = static_cast<uint16_t>(std::stoi(v));
            else if (lk == "bit") di.bit = std::stoi(v);
            else if (lk == "point") di.point = static_cast<uint16_t>(std::stoi(v));
            else if (lk == "invert") di.invert = (v == "1" || v == "true" || v == "yes");
        }
        tmpl.diList.push_back(di);
    } else if (StartsWith(prefix, "ai_")) {
        AIMapping ai{};
        for (auto& [k, v] : kv) { auto lk = ToLower(k);
            if (lk == "addr") ai.addr = static_cast<uint16_t>(std::stoi(v));
            else if (lk == "func") ai.func = static_cast<uint8_t>(std::stoi(v));
            else if (lk == "point") ai.point = static_cast<uint16_t>(std::stoi(v));
            else if (lk == "scale") ai.scale = std::stod(v);
            else if (lk == "offset") ai.offset = std::stod(v);
        }
        tmpl.aiList.push_back(ai);
    } else if (StartsWith(prefix, "do_")) {
        DOMapping do_{};
        for (auto& [k, v] : kv) { auto lk = ToLower(k);
            if (lk == "addr") do_.addr = static_cast<uint16_t>(std::stoi(v));
            else if (lk == "func") do_.func = static_cast<uint8_t>(std::stoi(v));
            else if (lk == "point") do_.point = static_cast<uint16_t>(std::stoi(v));
            else if (lk == "invert") do_.invert = (v == "1" || v == "true" || v == "yes");
            else if (lk == "pulse_ms") do_.pulseMs = std::stoi(v);
        }
        tmpl.doList.push_back(do_);
    } else if (StartsWith(prefix, "ao_")) {
        AOMapping ao{};
        for (auto& [k, v] : kv) { auto lk = ToLower(k);
            if (lk == "addr") ao.addr = static_cast<uint16_t>(std::stoi(v));
            else if (lk == "func") ao.func = static_cast<uint8_t>(std::stoi(v));
            else if (lk == "point") ao.point = static_cast<uint16_t>(std::stoi(v));
            else if (lk == "scale") ao.scale = std::stod(v);
            else if (lk == "offset") ao.offset = std::stod(v);
            else if (lk == "pulse_ms") ao.pulseMs = std::stoi(v);
        }
        tmpl.aoList.push_back(ao);
    }
    return true;
}

bool ModbusRtuMaster::ParseDeviceEntry(const std::string& key, const std::string& params, DeviceConfig& dev) {
    // 从 key 提取前缀 (e.g. "do_1" → "do_")
    size_t uscore = key.find('_');
    std::string prefix = (uscore != std::string::npos) ? key.substr(0, uscore + 1) : "di_";
    return ParseTemplateEntry(prefix, params, dev);
}

// ==================== 启停 ====================

bool ModbusRtuMaster::Start() {
    if (running_) return false;
    if (config_.ports.empty()) { std::cerr << "[ModbusRtuMaster] No ports" << std::endl; return false; }
    running_ = true;
    threads_.reserve(config_.ports.size());
    for (size_t i = 0; i < config_.ports.size(); i++) {
        threads_.emplace_back(&ModbusRtuMaster::PortThread, this, static_cast<int>(i));
        std::cout << "[ModbusRtuMaster] Start port " << config_.ports[i].portName << std::endl;
    }
    return true;
}

void ModbusRtuMaster::Stop() {
    running_ = false;
    for (auto& t : threads_) if (t.joinable()) t.join();
    threads_.clear();

    // 清除发送跟踪和脉冲队列，确保热重载后重新计算
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

// ==================== 端口线程 ====================

void ModbusRtuMaster::PortThread(int portIdx) {
    if (portIdx < 0 || portIdx >= static_cast<int>(config_.ports.size())) return;
    RtuPortConfig& pc = config_.ports[portIdx];
    int scanMs = pc.scanMs > 0 ? pc.scanMs : 3000;

    std::cout << "[ModbusRtuMaster] Thread start: " << pc.portName
              << " scan=" << scanMs << "ms devices=" << pc.devices.size() << std::endl;

    while (running_) {
        CommIO io;
        bool opened = false;

        try {
            io.open(pc.portName, pc.baud, pc.parity, pc.dataBits, pc.stopBits, pc.timeoutMs);
            opened = true;
            std::cout << "[ModbusRtuMaster] " << pc.portName << " opened" << std::endl;

            for (auto& dev : pc.devices) {
                std::string key = std::to_string(dev.channel) + "_" + std::to_string(dev.stationId);
                { std::lock_guard<std::mutex> lock(commMtx_); commStatus_[key] = true; }
                RemoteDataMgr::Instance().SetDi(dev.channel, dev.stationId, 1, true, 0, false);

                // 预填充 doSent_/aoSent_，避免首次连接时发出意外初始写回
                // 注意：使用 emplace 不覆盖已有条目，确保断线重连期间变化的 DO/AO 能被重新发送
                for (auto& do_ : dev.doList) {
                    DoPoint pt;
                    if (RemoteDataMgr::Instance().GetDo(dev.channel, dev.stationId, do_.point, pt)) {
                        bool val = (do_.invert) ? !pt.masterVal : pt.masterVal;
                        std::string sk = key + "_" + std::to_string(do_.point);
                        { std::lock_guard<std::mutex> lock(sentMtx_); doSent_.emplace(sk, val); }
                    }
                }
                for (auto& ao : dev.aoList) {
                    AoPoint pt;
                    if (RemoteDataMgr::Instance().GetAo(dev.channel, dev.stationId, ao.point, pt)) {
                        std::string sk = key + "_" + std::to_string(ao.point);
                        { std::lock_guard<std::mutex> lock(sentMtx_); aoSent_.emplace(sk, pt.value); }
                    }
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "[ModbusRtuMaster] " << pc.portName << " open failed: " << e.what() << std::endl;
            for (int i = 0; i < 50 && running_; i++) std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }

        while (running_ && opened) {
            try {
                for (auto& dev : pc.devices) {
                    if (!running_) break;
                    PollDevice(io, dev, pc.timeoutMs, pc.maxDiRead, pc.maxAiRead, pc.keepAliveAddr);
                }
                // DO/AO write-back after read
                for (auto& dev : pc.devices) {
                    if (!running_) break;
                    WriteDOChanges(io, dev, pc.timeoutMs);
                    WriteAOChanges(io, dev, pc.timeoutMs);
                }
                // DO/AO 脉冲复位（到期自动发送复位报文）
                ProcessPulseQueue(io, pc.timeoutMs);
                for (auto& dev : pc.devices) {
                    std::string key = std::to_string(dev.channel) + "_" + std::to_string(dev.stationId);
                    bool wasOffline = false;
                    { std::lock_guard<std::mutex> lock(commMtx_);
                        auto it = commStatus_.find(key);
                        if (it != commStatus_.end() && !it->second) { it->second = true; wasOffline = true; }
                    }
                    if (wasOffline) {
                        RemoteDataMgr::Instance().SetDi(dev.channel, dev.stationId, 1, true, 0, false);
                        std::cout << "[ModbusRtuMaster] Device " << static_cast<int>(dev.stationId) << " reconnected" << std::endl;
                    }
                }
            } catch (const std::exception& e) {
                std::cerr << "[ModbusRtuMaster] Error: " << e.what() << std::endl;
                opened = false;
                for (auto& dev : pc.devices) {
                    std::string key = std::to_string(dev.channel) + "_" + std::to_string(dev.stationId);
                    { std::lock_guard<std::mutex> lock(commMtx_); commStatus_[key] = false; }
                    RemoteDataMgr::Instance().SetDi(dev.channel, dev.stationId, 1, false, 0, false);
                }
                break;
            }
            if (running_ && opened) {
                for (int i = 0; i < scanMs / 100 && running_; i++)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
        try { io.close(); } catch (...) {}
        std::cout << "[ModbusRtuMaster] " << pc.portName << " disconnected" << std::endl;
    }
    std::cout << "[ModbusRtuMaster] Port thread exit: " << pc.portName << std::endl;
}

// ==================== 设备采集 ====================

bool ModbusRtuMaster::PollDevice(CommIO& io, const DeviceConfig& dev,
                                 int timeoutMs, int maxDiRead, int maxAiRead, int keepAliveAddr) {
    std::map<uint8_t, std::vector<size_t>> diByFunc;
    for (size_t i = 0; i < dev.diList.size(); i++) diByFunc[dev.diList[i].func].push_back(i);
    std::map<uint8_t, std::vector<size_t>> aiByFunc;
    for (size_t i = 0; i < dev.aiList.size(); i++) aiByFunc[dev.aiList[i].func].push_back(i);

    // ── FC01/02 读取 ──
    // 不检查地址连续性；在 maxDiRead 范围内合并为一包
    for (auto& [func, indices] : diByFunc) {
        if (func != 1 && func != 2) continue;
        if (indices.empty()) continue;
        std::sort(indices.begin(), indices.end(),
                  [&](size_t a, size_t b) { return dev.diList[a].addr < dev.diList[b].addr; });

        size_t start = 0;
        while (start < indices.size()) {
            uint16_t groupStart = dev.diList[indices[start]].addr;
            uint16_t groupEnd   = static_cast<uint16_t>(groupStart + dev.diList[indices[start]].qty - 1);
            size_t end = start;

            for (size_t i = start + 1; i < indices.size(); i++) {
                auto& di = dev.diList[indices[i]];
                uint16_t diEnd = static_cast<uint16_t>(di.addr + di.qty - 1);
                uint16_t newEnd = (diEnd > groupEnd) ? diEnd : groupEnd;
                if (static_cast<uint16_t>(newEnd - groupStart + 1) > static_cast<uint16_t>(maxDiRead))
                    break;
                groupEnd = newEnd;
                end = i;
            }

            uint16_t qty = static_cast<uint16_t>(groupEnd - groupStart + 1);
            uint8_t pdu[] = {func,
                static_cast<uint8_t>((groupStart >> 8) & 0xFF),
                static_cast<uint8_t>(groupStart & 0xFF),
                static_cast<uint8_t>((qty >> 8) & 0xFF),
                static_cast<uint8_t>(qty & 0xFF) };

            uint8_t resp[MAX_FRAME]; size_t respLen = 0;
            if (!SendAndReceive(io, dev.stationId, pdu, sizeof(pdu), resp, respLen, timeoutMs)) { start = end + 1; continue; }
            if (respLen > 1 && (resp[0] & EXCEPTION_MASK)) { start = end + 1; continue; }
            if (respLen >= 2) DispatchFC01or02(func, resp + 1, respLen - 1, dev, groupStart, qty);
            start = end + 1;
        }
    }

    // ── FC03/04 读取 ──
    for (auto func : {3, 4}) {
        struct Entry { uint16_t addr; int regs; bool isDI; size_t idx; };
        std::vector<Entry> entries;
        auto aiIt = aiByFunc.find(static_cast<uint8_t>(func));
        if (aiIt != aiByFunc.end())
            for (auto idx : aiIt->second) entries.push_back({dev.aiList[idx].addr, dev.aiList[idx].regCount(), false, idx});
        auto diIt = diByFunc.find(static_cast<uint8_t>(func));
        if (diIt != diByFunc.end())
            for (auto idx : diIt->second) entries.push_back({dev.diList[idx].addr, 1, true, idx});
        if (entries.empty()) continue;
        std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.addr < b.addr; });

        // 按 maxAiRead 长度合并，不检查地址连续性
        size_t start = 0;
        while (start < entries.size()) {
            uint16_t blockStart = entries[start].addr;
            uint16_t blockEnd   = static_cast<uint16_t>(entries[start].addr + entries[start].regs - 1);
            size_t end = start;

            for (size_t i = start + 1; i < entries.size(); i++) {
                uint16_t eAddr  = entries[i].addr;
                uint16_t eEnd   = static_cast<uint16_t>(eAddr + entries[i].regs - 1);
                uint16_t newEnd = (eEnd > blockEnd) ? eEnd : blockEnd;
                if (static_cast<uint16_t>(newEnd - blockStart + 1) > static_cast<uint16_t>(maxAiRead))
                    break;
                blockEnd = newEnd;
                end = i;
            }

            uint16_t qty = static_cast<uint16_t>(blockEnd - blockStart + 1);
            std::vector<size_t> diIdxs, aiIdxs;
            for (size_t i = start; i <= end; i++) {
                if (entries[i].isDI) diIdxs.push_back(entries[i].idx);
                else aiIdxs.push_back(entries[i].idx);
            }

            uint8_t pdu[] = {static_cast<uint8_t>(func),
                static_cast<uint8_t>((blockStart >> 8) & 0xFF),
                static_cast<uint8_t>(blockStart & 0xFF),
                static_cast<uint8_t>((qty >> 8) & 0xFF),
                static_cast<uint8_t>(qty & 0xFF) };

            uint8_t resp[MAX_FRAME]; size_t respLen = 0;
            if (!SendAndReceive(io, dev.stationId, pdu, sizeof(pdu), resp, respLen, timeoutMs)) { start = end + 1; continue; }
            if (respLen > 1 && (resp[0] & EXCEPTION_MASK)) { start = end + 1; continue; }
            if (respLen >= 2) DispatchFC03or04(static_cast<uint8_t>(func), resp + 1, respLen - 1, dev, diIdxs, aiIdxs, blockStart, qty);
            start = end + 1;
        }
    }

    // ── Keep-alive：仅 DO 无 DI/AI 时发送连接测试帧 ──
    if (keepAliveAddr >= 0 && dev.diList.empty() && dev.aiList.empty())
    {
        uint8_t pdu[] = {0x03,
            static_cast<uint8_t>((keepAliveAddr >> 8) & 0xFF),
            static_cast<uint8_t>(keepAliveAddr & 0xFF),
            0x00, 0x01};
        uint8_t resp[MAX_FRAME];
        size_t respLen = 0;
        SendAndReceive(io, dev.stationId, pdu, sizeof(pdu), resp, respLen, timeoutMs);
    }
    return true;
}

// ==================== 帧收发 ====================

bool ModbusRtuMaster::SendAndReceive(CommIO& io, uint8_t station,
                                      const uint8_t* pdu, size_t pduLen,
                                      uint8_t* resp, size_t& respLen, int timeoutMs) {
    uint8_t frame[MAX_FRAME]; size_t frameLen = 0;
    if (!BuildRtuFrame(station, pdu, pduLen, frame, frameLen)) return false;

    // Log TX
    PacketLogger::Instance().Log(PktDir::TX, 0, 0, station, pdu[0], 0, frame, frameLen);

    try { io.write(frame, frameLen); } catch (const std::exception&) { return false; }

    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    uint8_t buf[MAX_FRAME]; size_t pos = 0;
    auto startTime = std::chrono::steady_clock::now();
    bool gotByte = false;

    while (pos < MAX_FRAME) {
        uint8_t byte;
        try {
            size_t n = io.read(&byte, 1);
            if (n == 0) {
                if (gotByte) break;
                if (std::chrono::steady_clock::now() - startTime > std::chrono::milliseconds(timeoutMs)) break;
                continue;
            }
        } catch (const std::exception&) { break; }
        buf[pos++] = byte; gotByte = true;
        startTime = std::chrono::steady_clock::now();
    }
    if (pos < 3) return false;

    uint8_t func; size_t dataLen = 0;
    if (!ParseRtuResponse(buf, pos, func, resp, dataLen)) return false;
    respLen = dataLen;

    // Log RX
    PacketLogger::Instance().Log(PktDir::RX, 0, 0, station, func, 0, buf, pos);
    return true;
}

// ==================== 数据分发 ====================

bool ModbusRtuMaster::DispatchFC01or02(uint8_t /*devFunc*/, const uint8_t* respData, size_t respLen,
                                        const DeviceConfig& dev, uint16_t minAddr, uint16_t /*qty*/) {
    if (respLen < 1) return false;
    uint8_t byteCount = respData[0];
    const uint8_t* rawData = respData + 1;
    if (respLen - 1 < byteCount) byteCount = static_cast<uint8_t>(respLen - 1);

    auto& mgr = RemoteDataMgr::Instance();
    uint64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    for (auto& di : dev.diList) {
        uint16_t relAddr = static_cast<uint16_t>(di.addr - minAddr);
        for (uint16_t j = 0; j < di.qty; j++) {
            uint16_t byteIdx = static_cast<uint16_t>((relAddr + j) / 8);
            if (byteIdx >= byteCount) continue;
            bool val = (rawData[byteIdx] >> ((relAddr + j) % 8)) & 1;
            if (di.invert) val = !val;
            mgr.SetDi(dev.channel, dev.stationId, static_cast<uint16_t>(di.point + j), val, now, true);
        }
    }
    return true;
}

bool ModbusRtuMaster::DispatchFC03or04(uint8_t /*devFunc*/, const uint8_t* respData, size_t respLen,
                                        const DeviceConfig& dev,
                                        const std::vector<size_t>& diIdxs,
                                        const std::vector<size_t>& aiIdxs,
                                        uint16_t minAddr, uint16_t /*qty*/) {
    if (respLen < 1) return false;
    uint8_t byteCount = respData[0];
    const uint8_t* rawData = respData + 1;
    if (respLen - 1 < byteCount) byteCount = static_cast<uint8_t>(respLen - 1);

    auto& mgr = RemoteDataMgr::Instance();
    uint64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    for (auto idx : diIdxs) {
        auto& di = dev.diList[idx];
        if (di.bit < 0 || di.bit > 15) continue;
        uint16_t byteOff = static_cast<uint16_t>((di.addr - minAddr) * 2);
        if (byteOff + 1 >= byteCount) continue;
        uint16_t reg = static_cast<uint16_t>((rawData[byteOff] << 8) | rawData[byteOff + 1]);
        bool val = (reg >> di.bit) & 1;
        if (di.invert) val = !val;
        mgr.SetDi(dev.channel, dev.stationId, di.point, val, now, true);
    }
    for (auto idx : aiIdxs) {
        auto& ai = dev.aiList[idx];
        uint16_t byteOff = static_cast<uint16_t>((ai.addr - minAddr) * 2);
        if (byteOff + 2 > byteCount) continue;
        double rawVal = static_cast<double>(static_cast<uint16_t>((rawData[byteOff] << 8) | rawData[byteOff + 1]));
        double engVal = rawVal * ai.scale + ai.offset;
        mgr.SetAi(dev.channel, dev.stationId, ai.point, engVal);
    }
    return true;
}


// ==================== DO/AO 回写 ====================

bool ModbusRtuMaster::WriteDOChanges(CommIO& io, const DeviceConfig& dev, int timeoutMs)
{
    if (dev.doList.empty()) return true;
    auto& mgr = RemoteDataMgr::Instance();

    for (auto& do_ : dev.doList)
    {
        DoPoint pt;
        if (!mgr.GetDo(dev.channel, dev.stationId, do_.point, pt))
            continue;

        bool current = (do_.invert) ? !pt.masterVal : pt.masterVal;

        // 用模块级 doSent_ 避免与 CheckAllPointChange 的 lastMaster 同步冲突
        std::string key = std::to_string(dev.channel) + "_"
                        + std::to_string(dev.stationId) + "_"
                        + std::to_string(do_.point);
        {
            std::lock_guard<std::mutex> lock(sentMtx_);
            auto it = doSent_.find(key);
            if (it != doSent_.end() && it->second == current)
                continue;
        }

        // FC05: Write Single Coil
        if (do_.func == 5)
        {
            uint8_t pdu[] = {
                static_cast<uint8_t>(0x05),
                static_cast<uint8_t>((do_.addr >> 8) & 0xFF),
                static_cast<uint8_t>(do_.addr & 0xFF),
                current ? static_cast<uint8_t>(0xFF) : static_cast<uint8_t>(0x00),
                0x00
            };

            uint8_t resp[32]; size_t respLen = 0;
            if (!SendAndReceive(io, dev.stationId, pdu, sizeof(pdu), resp, respLen, timeoutMs))
                continue;

            if (respLen > 0 && (resp[0] & 0x80)) continue; // exception

            std::cout << "[ModbusRtuMaster] FC05 station=" << static_cast<int>(dev.stationId)
                      << " addr=" << do_.addr << " val=" << current << std::endl;

            // 记录已发送值
            {
                std::lock_guard<std::mutex> lock(sentMtx_);
                doSent_[key] = current;
            }

            // 遥控 ON 执行完成后立即复位 masterVal，使下一次遥控可再次触发
            if (current)
            {
                RemoteDataMgr::Instance().SetDoMaster(
                    dev.channel, dev.stationId, do_.point, false);
                {
                    std::lock_guard<std::mutex> lock(sentMtx_);
                    doSent_[key] = false;
                }
            }

            // 脉冲复位
            if (do_.pulseMs > 0)
            {
                uint64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                PulseEntry pe;
                pe.channel   = dev.channel;
                pe.stationId = dev.stationId;
                pe.func      = 5;
                pe.addr      = do_.addr;
                pe.valHi     = current ? 0x00 : 0xFF;
                pe.valLo     = 0x00;
                pe.readyAtMs = now + do_.pulseMs;
                { std::lock_guard<std::mutex> lock(pulseMtx_); pulseQueue_.push_back(pe); }
            }
        }
        // FC15 多线圈回写暂简化为 FC05
    }
    return true;
}

bool ModbusRtuMaster::WriteAOChanges(CommIO& io, const DeviceConfig& dev, int timeoutMs)
{
    if (dev.aoList.empty()) return true;
    auto& mgr = RemoteDataMgr::Instance();

    for (auto& ao : dev.aoList)
    {
        AoPoint pt;
        if (!mgr.GetAo(dev.channel, dev.stationId, ao.point, pt))
            continue;

        // 用模块级 aoSent_ 避免与 CheckAllPointChange 的 lastVal 同步冲突
        std::string key = std::to_string(dev.channel) + "_"
                        + std::to_string(dev.stationId) + "_"
                        + std::to_string(ao.point);
        {
            std::lock_guard<std::mutex> lock(sentMtx_);
            auto it = aoSent_.find(key);
            if (it != aoSent_.end() && std::abs(it->second - pt.value) < 0.001)
                continue;
        }

        // FC06: Write Single Register (UInt16)
        if (ao.func == 6)
        {
            uint16_t regVal = static_cast<uint16_t>(std::round(pt.value));
            uint8_t pdu[] = {
                static_cast<uint8_t>(0x06),
                static_cast<uint8_t>((ao.addr >> 8) & 0xFF),
                static_cast<uint8_t>(ao.addr & 0xFF),
                static_cast<uint8_t>((regVal >> 8) & 0xFF),
                static_cast<uint8_t>(regVal & 0xFF)
            };

            uint8_t resp[32]; size_t respLen = 0;
            if (!SendAndReceive(io, dev.stationId, pdu, sizeof(pdu), resp, respLen, timeoutMs))
                continue;

            if (respLen > 0 && (resp[0] & 0x80)) continue; // exception

            std::cout << "[ModbusRtuMaster] FC06 station=" << static_cast<int>(dev.stationId)
                      << " addr=" << ao.addr << " val=" << regVal << std::endl;

            // 记录已发送值
            {
                std::lock_guard<std::mutex> lock(sentMtx_);
                aoSent_[key] = pt.value;
            }

            // 脉冲复位
            if (ao.pulseMs > 0)
            {
                uint64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                PulseEntry pe;
                pe.channel   = dev.channel;
                pe.stationId = dev.stationId;
                pe.func      = 6;
                pe.addr      = ao.addr;
                pe.valHi     = 0x00;
                pe.valLo     = 0x00;
                pe.readyAtMs = now + ao.pulseMs;
                { std::lock_guard<std::mutex> lock(pulseMtx_); pulseQueue_.push_back(pe); }
            }
        }
        // FC16 多寄存器回写可在后续扩展
    }
    return true;
}
// ==================== DO/AO 脉冲复位 ====================

void ModbusRtuMaster::ProcessPulseQueue(CommIO& io, int timeoutMs)
{
    if (pulseQueue_.empty()) return;
    uint64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    std::lock_guard<std::mutex> lock(pulseMtx_);
    for (size_t i = 0; i < pulseQueue_.size(); )
    {
        auto& pe = pulseQueue_[i];
        if (now < pe.readyAtMs) { i++; continue; }

        uint8_t pdu[5];
        pdu[0] = pe.func;
        pdu[1] = static_cast<uint8_t>((pe.addr >> 8) & 0xFF);
        pdu[2] = static_cast<uint8_t>(pe.addr & 0xFF);
        pdu[3] = pe.valHi;
        pdu[4] = pe.valLo;

        uint8_t resp[32]; size_t respLen = 0;
        if (SendAndReceive(io, pe.stationId, pdu, sizeof(pdu), resp, respLen, timeoutMs) &&
            !(respLen > 0 && (resp[0] & 0x80)))
        {
            std::cout << "[ModbusRtuMaster] Pulse reset FC" << static_cast<int>(pe.func)
                      << " station=" << static_cast<int>(pe.stationId)
                      << " addr=" << pe.addr << std::endl;
        }
        pulseQueue_.erase(pulseQueue_.begin() + i);
    }
}

// ==================== ModbusRtuMasterModule ====================

struct ModbusRtuMasterModule::Impl { ModbusRtuMaster master; std::string cfgPath; bool loaded = false; bool running = false; };
ModbusRtuMasterModule::ModbusRtuMasterModule() : impl_(std::make_unique<Impl>()) {}
ModbusRtuMasterModule::~ModbusRtuMasterModule() { Stop(); }
bool ModbusRtuMasterModule::LoadConfig(const std::string& cfgPath) { impl_->cfgPath = cfgPath; impl_->loaded = impl_->master.LoadConfig(cfgPath); return impl_->loaded; }
bool ModbusRtuMasterModule::ValidateConfig(const std::string& cfgPath, std::vector<std::string>& errors) { ModbusRtuMaster temp; if (!temp.LoadConfig(cfgPath)) { errors.push_back("Cannot load: " + cfgPath); return false; } return true; }
bool ModbusRtuMasterModule::Start() { if (impl_->running) return true; impl_->running = impl_->master.Start(); return impl_->running; }
void ModbusRtuMasterModule::Stop() { if (!impl_->running) return; impl_->master.Stop(); impl_->running = false; }
bool ModbusRtuMasterModule::IsRunning() const { return impl_->running; }
REGISTER_MODULE("modbus_rtu_master", ModbusRtuMasterModule)
