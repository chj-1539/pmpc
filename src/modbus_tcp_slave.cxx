//=============================================================================
// modbus_tcp_slave.cxx — Modbus TCP 从站实现
//
// 协议：Modbus TCP (MBAP + PDU)
// 功能码：01 读线圈 / 02 读离散输入 / 03 读保持寄存器
//         04 读输入寄存器 / 05 写单个线圈
// 映射：点对点，地址可不连续，跨通道/设备
//=============================================================================

#include "modbus_tcp_slave.h"
#include "module_factory.h"
#include "str_util.h"
#include "packet_logger.h"
#include "ini_reader.h"
#include <iostream>
#include <sstream>
#include <cstring>
#include <algorithm>

// ─── Modbus TCP 常量 ───────────────────────────────────────────────────────
constexpr size_t MBAP_LEN    = 7;
constexpr size_t MAX_PDU     = 256;
constexpr size_t RECV_BUF    = MBAP_LEN + MAX_PDU;
constexpr uint8_t EXCEPTION_MASK = 0x80;
constexpr int    MAX_FC01_02_QTY = 2000;
constexpr int    MAX_FC03_04_QTY = 125;

// ==================== DataType/Endian 解析 ====================

MDataType ModbusTcpSlave::ParseDataType(const std::string& s)
{
    auto l = ToLower(s);
    if (l == "int16"   || l == "int16_t") return MDataType::Int16;
    if (l == "uint16"  || l == "uint16_t" || l == "word") return MDataType::UInt16;
    if (l == "int32"   || l == "int32_t" || l == "dint")   return MDataType::Int32;
    if (l == "uint32"  || l == "uint32_t" || l == "dword") return MDataType::UInt32;
    if (l == "int64"   || l == "int64_t" || l == "lint")   return MDataType::Int64;
    if (l == "uint64"  || l == "uint64_t" || l == "ulint") return MDataType::UInt64;
    if (l == "float"   || l == "real")   return MDataType::Float;
    if (l == "double"  || l == "lreal")  return MDataType::Double;
    return MDataType::UInt16;
}

MEndian ModbusTcpSlave::ParseEndian(const std::string& s)
{
    auto l = ToLower(s);
    if (l == "ab")        return MEndian::AB;
    if (l == "ba")        return MEndian::BA;
    if (l == "abcd")      return MEndian::ABCD;
    if (l == "cdab")      return MEndian::CDAB;
    if (l == "badc")      return MEndian::BADC;
    if (l == "dcba")      return MEndian::DCBA;
    if (l == "abcdefgh")  return MEndian::ABCDEFGH;
    if (l == "ghefcdab")  return MEndian::GHEFCDAB;
    if (l == "hgfedcba")  return MEndian::HGFEDCBA;
    return MEndian::AB;
}

// ==================== 数据转换工具 ====================

uint64_t ModbusTcpSlave::DoubleToRawValue(double value, MDataType dtype)
{
    switch (dtype) {
    case MDataType::UInt16:
        return static_cast<uint64_t>(static_cast<uint16_t>(value));
    case MDataType::Int16:
        return static_cast<uint64_t>(
            static_cast<uint16_t>(static_cast<int16_t>(value)));
    case MDataType::UInt32:
        return static_cast<uint64_t>(static_cast<uint32_t>(value));
    case MDataType::Int32:
        return static_cast<uint64_t>(
            static_cast<uint32_t>(static_cast<int32_t>(value)));
    case MDataType::UInt64:
        return static_cast<uint64_t>(value);
    case MDataType::Int64:
        return static_cast<uint64_t>(static_cast<int64_t>(value));
    case MDataType::Float: {
        float f = static_cast<float>(value);
        uint32_t tmp = 0;
        std::memcpy(&tmp, &f, sizeof(tmp));
        return tmp;
    }
    case MDataType::Double: {
        double d = value;
        uint64_t tmp = 0;
        std::memcpy(&tmp, &d, sizeof(tmp));
        return tmp;
    }
    default:
        return static_cast<uint64_t>(static_cast<uint16_t>(value));
    }
}

void ModbusTcpSlave::RawToWireBytes(uint64_t rawVal, int byteCount,
                                  MEndian endian, uint8_t* out)
{
    std::memset(out, 0, static_cast<size_t>(byteCount));

    switch (endian) {
    // ── 2 字节 ──
    case MEndian::AB:
        out[0] = static_cast<uint8_t>((rawVal >> 8) & 0xFF);
        out[1] = static_cast<uint8_t>(rawVal & 0xFF);
        break;
    case MEndian::BA:
        out[0] = static_cast<uint8_t>(rawVal & 0xFF);
        out[1] = static_cast<uint8_t>((rawVal >> 8) & 0xFF);
        break;

    // ── 4 字节 ──
    case MEndian::ABCD:
        out[0] = static_cast<uint8_t>((rawVal >> 24) & 0xFF);
        out[1] = static_cast<uint8_t>((rawVal >> 16) & 0xFF);
        out[2] = static_cast<uint8_t>((rawVal >> 8) & 0xFF);
        out[3] = static_cast<uint8_t>(rawVal & 0xFF);
        break;
    case MEndian::CDAB:
        out[0] = static_cast<uint8_t>((rawVal >> 8) & 0xFF);
        out[1] = static_cast<uint8_t>(rawVal & 0xFF);
        out[2] = static_cast<uint8_t>((rawVal >> 24) & 0xFF);
        out[3] = static_cast<uint8_t>((rawVal >> 16) & 0xFF);
        break;
    case MEndian::BADC:
        out[0] = static_cast<uint8_t>((rawVal >> 16) & 0xFF);
        out[1] = static_cast<uint8_t>((rawVal >> 24) & 0xFF);
        out[2] = static_cast<uint8_t>(rawVal & 0xFF);
        out[3] = static_cast<uint8_t>((rawVal >> 8) & 0xFF);
        break;
    case MEndian::DCBA:
        out[0] = static_cast<uint8_t>(rawVal & 0xFF);
        out[1] = static_cast<uint8_t>((rawVal >> 8) & 0xFF);
        out[2] = static_cast<uint8_t>((rawVal >> 16) & 0xFF);
        out[3] = static_cast<uint8_t>((rawVal >> 24) & 0xFF);
        break;

    // ── 8 字节 ──
    case MEndian::ABCDEFGH:
        for (int i = 7; i >= 0; i--) {
            out[i] = static_cast<uint8_t>(rawVal & 0xFF);
            rawVal >>= 8;
        }
        break;
    case MEndian::GHEFCDAB:
        out[0] = static_cast<uint8_t>((rawVal >> 40) & 0xFF);
        out[1] = static_cast<uint8_t>((rawVal >> 48) & 0xFF);
        out[2] = static_cast<uint8_t>((rawVal >> 56) & 0xFF);
        out[3] = static_cast<uint8_t>((rawVal >> 32) & 0xFF);
        out[4] = static_cast<uint8_t>((rawVal >> 8) & 0xFF);
        out[5] = static_cast<uint8_t>(rawVal & 0xFF);
        out[6] = static_cast<uint8_t>((rawVal >> 24) & 0xFF);
        out[7] = static_cast<uint8_t>((rawVal >> 16) & 0xFF);
        break;
    case MEndian::HGFEDCBA:
        for (int i = 0; i < 8; i++) {
            out[i] = static_cast<uint8_t>(rawVal & 0xFF);
            rawVal >>= 8;
        }
        break;

    default: // AB
        out[0] = static_cast<uint8_t>((rawVal >> 8) & 0xFF);
        out[1] = static_cast<uint8_t>(rawVal & 0xFF);
        break;
    }
}

// ==================== ModbusTcpSlave ====================

ModbusTcpSlave::ModbusTcpSlave() {}
ModbusTcpSlave::~ModbusTcpSlave() { Stop(); }

// ==================== 配置解析 ====================

static bool ParseFCParams(const std::string& params,
                           std::map<std::string, std::string>& kv)
{
    auto pairs = ParseKeyValues(params);
    kv.clear();
    for (auto& [k, v] : pairs)
        kv[ToLower(k)] = v;
    return !kv.empty();
}

bool ModbusTcpSlave::ParseDIEntry(const std::string& params,
                                  SlaveDIEntry& entry)
{
    std::map<std::string, std::string> kv;
    if (!ParseFCParams(params, kv)) return false;
    for (auto& [k, v] : kv) {
        if      (k == "addr")  entry.addr  = static_cast<uint16_t>(SafeStoi(v));
        else if (k == "func")  entry.func  = static_cast<uint8_t>(SafeStoi(v));
        else if (k == "ch")    entry.ch    = static_cast<uint16_t>(SafeStoi(v));
        else if (k == "dev")   entry.dev   = static_cast<uint16_t>(SafeStoi(v));
        else if (k == "point") entry.point = static_cast<uint16_t>(SafeStoi(v));
    }
    return true;
}

bool ModbusTcpSlave::ParseAIEntry(const std::string& params,
                                  SlaveAIEntry& entry)
{
    std::map<std::string, std::string> kv;
    if (!ParseFCParams(params, kv)) return false;
    for (auto& [k, v] : kv) {
        if      (k == "addr")    entry.addr   = static_cast<uint16_t>(SafeStoi(v));
        else if (k == "ch")      entry.ch     = static_cast<uint16_t>(SafeStoi(v));
        else if (k == "dev")     entry.dev    = static_cast<uint16_t>(SafeStoi(v));
        else if (k == "point")   entry.point  = static_cast<uint16_t>(SafeStoi(v));
        else if (k == "dtype")   entry.dtype  = ParseDataType(v);
        else if (k == "endian")  entry.endian = ParseEndian(v);
        else if (k == "scale")   entry.scale  = SafeStod(v);
        else if (k == "offset")  entry.offset = SafeStod(v);
    }
    return true;
}

bool ModbusTcpSlave::ParseBitDIEntry(const std::string& params,
                                     SlaveBitDIEntry& entry)
{
    std::map<std::string, std::string> kv;
    if (!ParseFCParams(params, kv)) return false;
    for (auto& [k, v] : kv) {
        if      (k == "addr")  entry.addr  = static_cast<uint16_t>(SafeStoi(v));
        else if (k == "bit")   entry.bit   = static_cast<uint8_t>(SafeStoi(v));
        else if (k == "ch")    entry.ch    = static_cast<uint16_t>(SafeStoi(v));
        else if (k == "dev")   entry.dev   = static_cast<uint16_t>(SafeStoi(v));
        else if (k == "point") entry.point = static_cast<uint16_t>(SafeStoi(v));
    }
    return true;
}

bool ModbusTcpSlave::ParseDOEntry(const std::string& params,
                                  SlaveDOEntry& entry)
{
    std::map<std::string, std::string> kv;
    if (!ParseFCParams(params, kv)) return false;
    for (auto& [k, v] : kv) {
        if      (k == "addr")   entry.addr   = static_cast<uint16_t>(SafeStoi(v));
        else if (k == "ch")     entry.ch     = static_cast<uint16_t>(SafeStoi(v));
        else if (k == "dev")    entry.dev    = static_cast<uint16_t>(SafeStoi(v));
        else if (k == "point")  entry.point  = static_cast<uint16_t>(SafeStoi(v));
        else if (k == "invert") entry.invert = (v == "1" || v == "true" || v == "yes");
    }
    return true;
}

bool ModbusTcpSlave::LoadConfig(const std::string& path)
{
    IniReader ini;
    if (!ini.Load(path)) {
        std::cerr << "[ModbusTcpSlave] 无法打开配置文件: " << path << std::endl;
        return false;
    }

    config_ = SlaveConfig{};
    config_.port = ini.GetInt("global", "port", 502);
    config_.maxClients = ini.GetInt("global", "max_clients", 10);
    config_.verbose = ini.GetInt("global", "verbose", 1);

    // ── 解析 [listen_N] 访问控制 ──
    auto sections = ini.Sections();
    for (const auto& sec : sections) {
        std::string secLower = ToLower(sec);
        if (!StartsWith(secLower, "listen_")) continue;
        SlaveBind bind;
        bind.allowedIP = ini.Get(sec, "ip", "");
        bind.port = static_cast<uint16_t>(ini.GetInt(sec, "port", 0));
        if (bind.port > 0) binds_.push_back(bind);
    }
    if (binds_.empty())
        binds_.push_back({"", static_cast<uint16_t>(config_.port)});

    // ── 解析设备配置 ──
    // 注意：直接在 vector 中 emplace_back 构造设备，
    // 避免 GCC 8.1 MinGW 的 std::map copy/move 析构 bug
    {
        size_t devCnt = 0;
        for (const auto& s : sections) if (StartsWith(ToLower(s), "device_")) devCnt++;
        config_.devices.reserve(devCnt);
    }
    for (const auto& sec : sections) {
        std::string secLower = ToLower(sec);
        if (secLower == "global") continue;
        if (StartsWith(secLower, "listen_")) continue;
        if (!StartsWith(secLower, "device_")) continue;

        config_.devices.emplace_back();
        auto& dev = config_.devices.back();

        dev.stationId = static_cast<uint8_t>(ini.GetInt(sec, "station_id", 0));
        dev.desc = ini.Get(sec, "desc", "");

        if (dev.stationId == 0) {
            std::cerr << "[ModbusTcpSlave] " << sec << " station_id 未设置或为 0，跳过" << std::endl;
            config_.devices.pop_back();
            continue;
        }

        auto keys = ini.Keys(sec);
        for (const auto& key : keys) {
            std::string keyLower = ToLower(key);
            std::string val = ini.Get(sec, key, "");
            if (keyLower == "station_id" || keyLower == "desc") continue;

            if (StartsWith(keyLower, "fc01_")) {
                SlaveDIEntry e;
                if (ParseDIEntry(val, e)) { e.func = 1; dev.diMap[DI_KEY(1, e.addr)] = e; }
                else std::cerr << "[ModbusTcpSlave] " << sec << " " << key << " 解析失败" << std::endl;
            }
            else if (StartsWith(keyLower, "fc02_")) {
                SlaveDIEntry e;
                if (ParseDIEntry(val, e)) { e.func = 2; dev.diMap[DI_KEY(2, e.addr)] = e; }
                else std::cerr << "[ModbusTcpSlave] " << sec << " " << key << " 解析失败" << std::endl;
            }
            else if (StartsWith(keyLower, "fc03_") || StartsWith(keyLower, "fc04_")) {
                auto lp = ToLower(val);
                if (lp.find("bit=") != std::string::npos) {
                    SlaveBitDIEntry e;
                    if (ParseBitDIEntry(val, e)) dev.diBitMap[e.addr].push_back(e);
                    else std::cerr << "[ModbusTcpSlave] " << sec << " " << key << " 解析失败" << std::endl;
                } else {
                    SlaveAIEntry e;
                    if (ParseAIEntry(val, e)) dev.aiMap[e.addr] = e;
                    else std::cerr << "[ModbusTcpSlave] " << sec << " " << key << " 解析失败" << std::endl;
                }
            }
            else if (StartsWith(keyLower, "fc05_")) {
                SlaveDOEntry e;
                if (ParseDOEntry(val, e)) dev.doMap[e.addr] = e;
                else std::cerr << "[ModbusTcpSlave] " << sec << " " << key << " 解析失败" << std::endl;
            }
        }

        if (config_.verbose >= 1) {
            std::cout << "[ModbusTcpSlave] 加载设备: station_id="
                      << static_cast<int>(dev.stationId) << " desc=" << dev.desc
                      << " DI=" << dev.diMap.size()
                      << " AI=" << dev.aiMap.size()
                      << " BitDI=" << dev.diBitMap.size()
                      << " DO=" << dev.doMap.size()
                      << std::endl;
        }
    }

    if (config_.devices.empty()) {
        std::cerr << "[ModbusTcpSlave] 没有加载到任何设备配置" << std::endl;
        return false;
    }

    std::cout << "[ModbusTcpSlave] 配置加载完成: port=" << config_.port
              << " 设备数=" << config_.devices.size() << std::endl;
    return true;
}

// ==================== 启停 ====================

bool ModbusTcpSlave::Start()
{
    if (running_) return true;
    running_ = true;

    for (auto& bind : binds_) {
        try {
            socket sock;
            sock.bind(socket_addr("0.0.0.0", static_cast<uint16_t>(bind.port)));
            sock.listen(config_.maxClients);
            listenSocks_.push_back(std::move(sock));
        } catch (const socket_error& e) {
            std::cerr << "[ModbusTcpSlave] 绑定端口 " << bind.port
                      << " 失败: " << e.what() << std::endl;
            continue;
        }
    }

    if (listenSocks_.empty()) {
        running_ = false;
        return false;
    }

    for (size_t i = 0; i < listenSocks_.size(); i++)
        acceptThreads_.emplace_back([this, i]() { AcceptLoop(binds_[i], listenSocks_[i]); });

    for (auto& bind : binds_)
        std::cout << "[ModbusTcpSlave] 监听 " << (bind.allowedIP.empty() ? "0.0.0.0" : bind.allowedIP)
                  << ":" << bind.port << std::endl;
    return true;
}

void ModbusTcpSlave::Stop()
{
    if (!running_) return;
    running_ = false;

    for (auto& sock : listenSocks_)
        try { sock.close(); } catch (...) {}

    for (auto& t : acceptThreads_)
        if (t.joinable()) t.join();
    listenSocks_.clear();
    acceptThreads_.clear();

    std::lock_guard<std::mutex> lock(clientMtx_);
    for (auto& t : clientThreads_)
        if (t.joinable()) t.join();
    clientThreads_.clear();
    clientDoneFlags_.clear();

    std::cout << "[ModbusTcpSlave] 从站已停止" << std::endl;
}

// ==================== Accept 循环 ====================

void ModbusTcpSlave::AcceptLoop(const SlaveBind& bind, socket& listenSock)
{
    std::cout << "[ModbusTcpSlave] 监听线程启动: " << (bind.allowedIP.empty() ? "ALL" : bind.allowedIP)
              << ":" << bind.port << std::endl;

    while (running_) {
        try {
            socket_addr peer;
            socket client = listenSock.accept(&peer);
            if (!running_) break;

            if (!bind.allowedIP.empty() && peer.host() != bind.allowedIP) {
                if (config_.verbose >= 1)
                    std::cout << "[ModbusTcpSlave] 拒绝连接: " << peer.host() << std::endl;
                continue;
            }

            {
                std::lock_guard<std::mutex> lock(clientMtx_);
                if (++cleanupCnt_ % 10 == 0) {
                    for (size_t i = 0; i < clientThreads_.size(); ) {
                        if (clientDoneFlags_.size() > i &&
                            clientDoneFlags_[i]->load(std::memory_order_acquire)) {
                            if (clientThreads_[i].joinable()) clientThreads_[i].join();
                            clientThreads_.erase(clientThreads_.begin() + i);
                            clientDoneFlags_.erase(clientDoneFlags_.begin() + i);
                        } else { ++i; }
                    }
                }
                auto done = std::make_shared<std::atomic<bool>>(false);
                clientDoneFlags_.push_back(done);
                clientThreads_.emplace_back(&ModbusTcpSlave::ClientThread, this,
                    std::move(client), std::move(done));
            }

        } catch (const socket_error&) {
            if (running_)
                std::cerr << "[ModbusTcpSlave] Accept 错误" << std::endl;
            break;
        }
    }

    std::cout << "[ModbusTcpSlave] 监听线程退出: " << bind.allowedIP << ":" << bind.port << std::endl;
}

// ==================== 客户端线程 ====================

void ModbusTcpSlave::ClientThread(socket clientSock,
                                   std::shared_ptr<std::atomic<bool>> done)
{
    struct DoneGuard {
        std::atomic<bool>* d;
        ~DoneGuard() { if (d) d->store(true, std::memory_order_release); }
    } guard{done.get()};

    uint8_t buf[RECV_BUF];

    if (config_.verbose >= 1) {
        std::cout << "[ModbusTcpSlave] 客户端接入" << std::endl;
    }

    while (running_) {
        size_t total = 0;
        while (total < MBAP_LEN && running_) {
            size_t n;
            try {
                n = clientSock.recv(buf + total, MBAP_LEN - total);
            } catch (const socket_error&) {
                n = 0;
            }
            if (n == 0) {
                if (config_.verbose >= 2)
                    std::cout << "[ModbusTcpSlave] 客户端断开" << std::endl;
                return;
            }
            total += n;
        }
        if (!running_) return;

        uint16_t transId = static_cast<uint16_t>(
            (static_cast<uint16_t>(buf[0]) << 8) | buf[1]);
        uint16_t protoId = static_cast<uint16_t>(
            (static_cast<uint16_t>(buf[2]) << 8) | buf[3]);
        uint16_t frameLen = static_cast<uint16_t>(
            (static_cast<uint16_t>(buf[4]) << 8) | buf[5]);
        uint8_t stationId = buf[6];

        if (protoId != 0) return;
        if (frameLen < 1) return;
        uint16_t pduLen = static_cast<uint16_t>(frameLen - 1);
        if (pduLen > MAX_PDU) return;

        total = 0;
        while (total < pduLen && running_) {
            size_t n;
            try {
                n = clientSock.recv(buf + MBAP_LEN + total, pduLen - total);
            } catch (const socket_error&) {
                n = 0;
            }
            if (n == 0) return;
            total += n;
        }
        if (!running_) return;

        const uint8_t* pdu = buf + MBAP_LEN;

        const SlaveDeviceConfig* dev = nullptr;
        for (const auto& d : config_.devices) {
            if (d.stationId == stationId) {
                dev = &d;
                break;
            }
        }
        if (!dev) {
            SendException(clientSock, transId, stationId, pdu[0], 0x02);
            {
                PacketLogger::Instance().Log(PktDir::RX, 0, 0, stationId,
                pdu[0], transId, pdu, pduLen);
            PacketLogger::Instance().Log(PktDir::TX, 0, 0, stationId,
                pdu[0], transId, (const uint8_t*)"\x82\x02", 2);
            }
            continue;
        }

        uint8_t func = pdu[0];
        std::vector<uint8_t> response;
        bool ok = false;

        {
            PacketLogger::Instance().Log(PktDir::RX,
                static_cast<uint16_t>(dev->stationId),
                dev->stationId, stationId, func, transId, pdu, pduLen);
        }

        switch (func) {
        case 0x01: ok = HandleFC01(*dev, pdu, pduLen, response); break;
        case 0x02: ok = HandleFC02(*dev, pdu, pduLen, response); break;
        case 0x03: ok = HandleFC03(*dev, pdu, pduLen, response); break;
        case 0x04: ok = HandleFC04(*dev, pdu, pduLen, response); break;
        case 0x05: ok = HandleFC05(*dev, pdu, pduLen, response); break;
        default:
            SendException(clientSock, transId, stationId, func, 0x01);
            {
                PacketLogger::Instance().Log(PktDir::TX,
                    static_cast<uint16_t>(dev->stationId),
                    dev->stationId, stationId, func, transId,
                    (const uint8_t*)"\x81\x01", 2);
            }
            continue;
        }

        if (ok) {
            SendFrame(clientSock, transId, stationId,
                      response.data(), response.size());
            {
                PacketLogger::Instance().Log(PktDir::TX,
                    static_cast<uint16_t>(dev->stationId),
                    dev->stationId, stationId, func, transId,
                    response.data(), response.size());
            }
        } else {
            if (func == 0x05) {
                SendException(clientSock, transId, stationId, func, 0x02);
                {
                    PacketLogger::Instance().Log(PktDir::TX,
                        static_cast<uint16_t>(dev->stationId),
                        dev->stationId, stationId, func, transId,
                        (const uint8_t*)"\x85\x02", 2);
                }
            }
        }
    }
}

// ==================== 帧收发 ====================

bool ModbusTcpSlave::SendFrame(socket& sock, uint16_t transId,
                             uint8_t stationId,
                             const uint8_t* pdu, size_t pduLen)
{
    uint8_t buf[MBAP_LEN + MAX_PDU];
    size_t pos = 0;

    buf[pos++] = static_cast<uint8_t>((transId >> 8) & 0xFF);
    buf[pos++] = static_cast<uint8_t>(transId & 0xFF);
    buf[pos++] = 0x00; buf[pos++] = 0x00;
    uint16_t len = static_cast<uint16_t>(1 + pduLen);
    buf[pos++] = static_cast<uint8_t>((len >> 8) & 0xFF);
    buf[pos++] = static_cast<uint8_t>(len & 0xFF);
    buf[pos++] = stationId;
    std::memcpy(buf + pos, pdu, pduLen);
    pos += pduLen;

    size_t total = 0;
    while (total < pos) {
        size_t n;
        try {
            n = sock.send(buf + total, pos - total);
        } catch (const socket_error&) {
            return false;
        }
        if (n == 0) return false;
        total += n;
    }
    return true;
}

bool ModbusTcpSlave::SendException(socket& sock, uint16_t transId,
                                 uint8_t stationId, uint8_t func,
                                 uint8_t errCode)
{
    uint8_t pdu[] = {
        static_cast<uint8_t>(func | EXCEPTION_MASK),
        errCode
    };
    return SendFrame(sock, transId, stationId, pdu, sizeof(pdu));
}

// ==================== FC01: 读线圈 ====================

bool ModbusTcpSlave::HandleFC01(const SlaveDeviceConfig& dev,
                              const uint8_t* pdu, size_t /*pduLen*/,
                              std::vector<uint8_t>& resp)
{
    uint16_t startAddr = static_cast<uint16_t>(
        (static_cast<uint16_t>(pdu[1]) << 8) | pdu[2]);
    uint16_t qty = static_cast<uint16_t>(
        (static_cast<uint16_t>(pdu[3]) << 8) | pdu[4]);

    if (qty == 0 || qty > MAX_FC01_02_QTY) return false;

    uint16_t byteCount = static_cast<uint16_t>((qty + 7) / 8);
    uint8_t data[256];
    std::memset(data, 0, sizeof(data));

    auto& mgr = RemoteDataMgr::Instance();

    for (uint16_t i = 0; i < qty; i++) {
        uint16_t addr = static_cast<uint16_t>(startAddr + i);
        auto it = dev.diMap.find(DI_KEY(1, addr));
        if (it == dev.diMap.end()) continue;

        const auto& e = it->second;
        DiPoint pt;
        if (!mgr.GetDi(e.ch, e.dev, e.point, pt)) continue;

        if (pt.value) {
            uint16_t byteIdx = i / 8;
            uint8_t  bitIdx  = static_cast<uint8_t>(i % 8);
            data[byteIdx] |= static_cast<uint8_t>(1 << bitIdx);
        }
    }

    resp.clear();
    resp.push_back(0x01);
    resp.push_back(static_cast<uint8_t>(byteCount));
    resp.insert(resp.end(), data, data + byteCount);
    return true;
}

// ==================== FC02: 读离散输入 ====================

bool ModbusTcpSlave::HandleFC02(const SlaveDeviceConfig& dev,
                              const uint8_t* pdu, size_t /*pduLen*/,
                              std::vector<uint8_t>& resp)
{
    uint16_t startAddr = static_cast<uint16_t>(
        (static_cast<uint16_t>(pdu[1]) << 8) | pdu[2]);
    uint16_t qty = static_cast<uint16_t>(
        (static_cast<uint16_t>(pdu[3]) << 8) | pdu[4]);

    if (qty == 0 || qty > MAX_FC01_02_QTY) return false;

    uint16_t byteCount = static_cast<uint16_t>((qty + 7) / 8);
    uint8_t data[256];
    std::memset(data, 0, sizeof(data));

    auto& mgr = RemoteDataMgr::Instance();

    for (uint16_t i = 0; i < qty; i++) {
        uint16_t addr = static_cast<uint16_t>(startAddr + i);
        auto it = dev.diMap.find(DI_KEY(2, addr));
        if (it == dev.diMap.end()) continue;

        const auto& e = it->second;
        DiPoint pt;
        if (!mgr.GetDi(e.ch, e.dev, e.point, pt)) continue;

        if (pt.value) {
            uint16_t byteIdx = i / 8;
            uint8_t  bitIdx  = static_cast<uint8_t>(i % 8);
            data[byteIdx] |= static_cast<uint8_t>(1 << bitIdx);
        }
    }

    resp.clear();
    resp.push_back(0x02);
    resp.push_back(static_cast<uint8_t>(byteCount));
    resp.insert(resp.end(), data, data + byteCount);
    return true;
}

// ==================== FC03: 读保持寄存器 ====================

bool ModbusTcpSlave::HandleFC03(const SlaveDeviceConfig& dev,
                              const uint8_t* pdu, size_t pduLen,
                              std::vector<uint8_t>& resp)
{
    return HandleFCReadRegs(dev, pdu, pduLen, resp, 0x03,
                            dev.aiMap, dev.diBitMap);
}

// ==================== FC04: 读输入寄存器 ====================

bool ModbusTcpSlave::HandleFC04(const SlaveDeviceConfig& dev,
                              const uint8_t* pdu, size_t pduLen,
                              std::vector<uint8_t>& resp)
{
    return HandleFCReadRegs(dev, pdu, pduLen, resp, 0x04,
                            dev.aiMap, dev.diBitMap);
}

// ==================== FC03/04 读寄存器通用实现 ====================

bool ModbusTcpSlave::HandleFCReadRegs(
    const SlaveDeviceConfig& /*dev*/,
    const uint8_t* pdu, size_t /*pduLen*/,
    std::vector<uint8_t>& resp, uint8_t func,
    const std::map<uint16_t, SlaveAIEntry>& aiMap,
    const std::map<uint16_t, std::vector<SlaveBitDIEntry>>& diBitMap)
{
    uint16_t startAddr = static_cast<uint16_t>(
        (static_cast<uint16_t>(pdu[1]) << 8) | pdu[2]);
    uint16_t qty = static_cast<uint16_t>(
        (static_cast<uint16_t>(pdu[3]) << 8) | pdu[4]);

    if (qty == 0 || qty > MAX_FC03_04_QTY) return false;

    uint16_t byteCount = static_cast<uint16_t>(qty * 2);
    uint8_t data[256];
    std::memset(data, 0, sizeof(data));

    auto& mgr = RemoteDataMgr::Instance();

    // ── 1. 应用 AI 映射 ──
    auto aiIt = aiMap.lower_bound(startAddr);
    auto aiEnd = aiMap.end();
    uint16_t aiRangeEnd = static_cast<uint16_t>(startAddr + qty);

    while (aiIt != aiEnd && aiIt->first < aiRangeEnd) {
        const auto& ai = aiIt->second;
        int regCnt = DataConvert::GetRegCount(ai.dtype);
        uint16_t aiAddr = aiIt->first;

        AiPoint pt;
        if (mgr.GetAi(ai.ch, ai.dev, ai.point, pt)) {
            double rawVal = (pt.value - ai.offset) / ai.scale;
            uint64_t rawInt = DoubleToRawValue(rawVal, ai.dtype);
            int byteCountVal = regCnt * 2;
            uint8_t wireBuf[8] = {0};
            RawToWireBytes(rawInt, byteCountVal, ai.endian, wireBuf);

            for (int r = 0; r < regCnt; r++) {
                int offset = (static_cast<int>(aiAddr) - static_cast<int>(startAddr) + r) * 2;
                if (offset + 1 >= static_cast<int>(byteCount)) break;
                data[offset]     = wireBuf[r * 2];
                data[offset + 1] = wireBuf[r * 2 + 1];
            }
        }
        ++aiIt;
    }

    // ── 2. 应用 DI 按位映射 ──
    for (uint16_t i = 0; i < qty; i++) {
        uint16_t addr = static_cast<uint16_t>(startAddr + i);

        auto mapIt = diBitMap.find(addr);
        if (mapIt == diBitMap.end()) continue;

        int idx = static_cast<int>(addr - startAddr);
        uint16_t regVal = static_cast<uint16_t>(
            (static_cast<uint16_t>(data[idx * 2]) << 8) | data[idx * 2 + 1]);

        for (const auto& di : mapIt->second) {
            DiPoint pt;
            if (!mgr.GetDi(di.ch, di.dev, di.point, pt)) continue;

            if (pt.value)
                regVal |= static_cast<uint16_t>(1 << di.bit);
            else
                regVal &= static_cast<uint16_t>(~(1 << di.bit));
        }

        data[idx * 2]     = static_cast<uint8_t>((regVal >> 8) & 0xFF);
        data[idx * 2 + 1] = static_cast<uint8_t>(regVal & 0xFF);
    }

    resp.clear();
    resp.push_back(func);
    resp.push_back(static_cast<uint8_t>(byteCount));
    resp.insert(resp.end(), data, data + byteCount);
    return true;
}

// ==================== FC05: 写单个线圈 ====================

bool ModbusTcpSlave::HandleFC05(const SlaveDeviceConfig& dev,
                              const uint8_t* pdu, size_t /*pduLen*/,
                              std::vector<uint8_t>& resp)
{
    uint16_t addr = static_cast<uint16_t>(
        (static_cast<uint16_t>(pdu[1]) << 8) | pdu[2]);
    uint16_t val = static_cast<uint16_t>(
        (static_cast<uint16_t>(pdu[3]) << 8) | pdu[4]);

    auto it = dev.doMap.find(addr);
    if (it == dev.doMap.end()) return false;

    const auto& e = it->second;

    bool coilVal = (val == 0xFF00);
    if (e.invert) coilVal = !coilVal;

    auto& mgr = RemoteDataMgr::Instance();
    mgr.SetDoMaster(e.ch, e.dev, e.point, coilVal);

    if (config_.verbose >= 1) {
        std::cout << "[ModbusTcpSlave] FC05 station="
                  << static_cast<int>(dev.stationId)
                  << " addr=" << addr
                  << " val=" << (val == 0xFF00 ? "ON" : "OFF")
                  << (e.invert ? " (inverted)" : "")
                  << " -> ch=" << e.ch
                  << " dev=" << e.dev
                  << " pt=" << e.point
                  << std::endl;
    }

    resp.assign(pdu, pdu + 5);
    return true;
}

// ==================== ModbusTcpSlaveModule (AppModule 包装) ====================

struct ModbusTcpSlaveModule::Impl {
    ModbusTcpSlave slave;
    std::string cfgPath;
    bool loaded = false;
    bool running = false;
};

ModbusTcpSlaveModule::ModbusTcpSlaveModule()
    : impl_(std::make_unique<Impl>()) {}

ModbusTcpSlaveModule::~ModbusTcpSlaveModule() { Stop(); }

bool ModbusTcpSlaveModule::LoadConfig(const std::string& cfgPath)
{
    impl_->cfgPath = cfgPath;
    impl_->loaded = impl_->slave.LoadConfig(cfgPath);
    return impl_->loaded;
}

bool ModbusTcpSlaveModule::ValidateConfig(const std::string& cfgPath,
                                        std::vector<std::string>& errors)
{
    ModbusTcpSlave slave;
    if (!slave.LoadConfig(cfgPath)) {
        errors.push_back("Cannot load: " + cfgPath);
        return false;
    }
    return errors.empty();
}

bool ModbusTcpSlaveModule::Start()
{
    if (impl_->running) return true;
    impl_->running = impl_->slave.Start();
    return impl_->running;
}

void ModbusTcpSlaveModule::Stop()
{
    if (!impl_->running) return;
    impl_->slave.Stop();
    impl_->running = false;
}

bool ModbusTcpSlaveModule::IsRunning() const
{
    return impl_->running;
}

REGISTER_MODULE("modbus_tcp_slave", ModbusTcpSlaveModule)
