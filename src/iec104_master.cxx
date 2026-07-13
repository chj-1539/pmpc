//=============================================================================
// iec104_master.cxx — IEC 60870-5-104 主站采集实现
//
// 协议层次：APCI (6B) — 0x68 + Length + Control Field (4B)
//           ASDU — Type + VSQ + COT(2) + COA(2) + IOA(3) + Data
//=============================================================================

#include "iec104_master.h"
#include "module_factory.h"
#include "ini_reader.h"
#include "str_util.h"
#include "packet_logger.h"
#include <iostream>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <algorithm>
#include <cmath>

constexpr size_t APCI_HEADER  = 6;
constexpr size_t RECV_BUF     = 4096;
constexpr int    RECV_TICK_MS = 50;

// ==================== 数据解析工具 ====================

static double ParseNormalizedValue(const uint8_t* data) {
    int16_t raw = static_cast<int16_t>(static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8));
    return static_cast<double>(raw) / 32768.0;
}
static double ParseFloatValue(const uint8_t* data) {
    uint32_t raw = static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8)
                 | (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
    float f; std::memcpy(&f, &raw, sizeof(f)); return static_cast<double>(f);
}
static uint32_t ReadIOA(const uint8_t* data) {
    return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) | (static_cast<uint32_t>(data[2]) << 16);
}

uint64_t Iec104Master::NowMs() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

int Iec104Master::SafeStoi(const std::string& s, int def) { try { return std::stoi(s); } catch (...) { return def; } }
double Iec104Master::SafeStod(const std::string& s, double def) { try { return std::stod(s); } catch (...) { return def; } }

Iec104Master::Iec104Master() {}
Iec104Master::~Iec104Master() { Stop(); }

// ==================== 帧构建 ====================

bool Iec104Master::SendUFrame(socket& sock, uint32_t ctrl) {
    uint8_t buf[] = {FRAME_START, 0x04,
        static_cast<uint8_t>(ctrl & 0xFF), static_cast<uint8_t>((ctrl >> 8) & 0xFF),
        static_cast<uint8_t>((ctrl >> 16) & 0xFF), static_cast<uint8_t>((ctrl >> 24) & 0xFF)};
    PacketLogger::Instance().Log(PktDir::TX, 0, 0, 0, 0, 0, buf, 6);
    size_t total = 0;
    while (total < sizeof(buf)) { size_t n = sock.send(buf + total, sizeof(buf) - total); if (n == 0) return false; total += n; }
    return true;
}

bool Iec104Master::SendSFrame(socket& sock) {
    uint32_t ctrl = 0x00010000;
    uint8_t buf[] = {FRAME_START, 0x04,
        static_cast<uint8_t>(ctrl & 0xFF), static_cast<uint8_t>((ctrl >> 8) & 0xFF),
        static_cast<uint8_t>((ctrl >> 16) & 0xFF), static_cast<uint8_t>((ctrl >> 24) & 0xFF)};
    PacketLogger::Instance().Log(PktDir::TX, 0, 0, 0, 0, 0, buf, 6);
    size_t total = 0;
    while (total < sizeof(buf)) { size_t n = sock.send(buf + total, sizeof(buf) - total); if (n == 0) return false; total += n; }
    return true;
}

bool Iec104Master::SendIFrame(socket& sock, const uint8_t* asdu, size_t asduLen,
                              uint32_t& sendSeq, uint32_t recvSeq) {
    if (asduLen > 250) return false;
    // M2 修复：以前 ctrl 恒为 0，严格从站收满 k=12 个未确认 I 帧后拒绝。
    // 现在按 IEC 104 规范用 sendSeq/recvSeq 编码 ctrl，并把 sendSeq +1。
    uint32_t ctrl = EncodeIFrameCtrl(sendSeq, recvSeq);
    uint8_t buf[256]; size_t pos = 0;
    buf[pos++] = FRAME_START;
    buf[pos++] = static_cast<uint8_t>(4 + asduLen);
    buf[pos++] = static_cast<uint8_t>(ctrl & 0xFF); buf[pos++] = static_cast<uint8_t>((ctrl >> 8) & 0xFF);
    buf[pos++] = static_cast<uint8_t>((ctrl >> 16) & 0xFF); buf[pos++] = static_cast<uint8_t>((ctrl >> 24) & 0xFF);
    std::memcpy(buf + pos, asdu, asduLen); pos += asduLen;
    PacketLogger::Instance().Log(PktDir::TX, 0, 0, 0, asdu[0], 0, buf, pos);
    size_t total = 0;
    while (total < pos) { size_t n = sock.send(buf + total, pos - total); if (n == 0) return false; total += n; }
    sendSeq = (sendSeq + 1) & 0x7FFF;   // sNr 是 15 位
    return true;
}

// ==================== 帧接收 ====================

bool Iec104Master::RecvFrame(socket& sock, int timeoutMs, uint8_t* buf, size_t& len) {
    sock.set_recv_timeout(std::chrono::milliseconds(timeoutMs));
    if (sock.recv(buf, 1) != 1) return false;
    if (buf[0] != FRAME_START) return false;
    if (sock.recv(buf + 1, 1) != 1) return false;
    uint8_t apduLen = buf[1];
    if (apduLen < 4 || apduLen > 253) {
        // M12 修复：apduLen 无效意味着帧同步已丢失。任何"继续读 N 字节
        // 排空"都是猜测，只会把噪声当帧内容。安全的做法是关闭 socket，
        // 让上层重连重新握手。见 CLAUDE.md「已知陷阱 / 修复历史」M12。
        try { sock.shutdown(2); } catch (...) {}
        try { sock.close();   } catch (...) {}
        return false;
    }
    size_t total = 0;
    while (total < apduLen) { size_t n = sock.recv(buf + 2 + total, apduLen - total); if (n == 0) return false; total += n; }
    len = 2 + apduLen;
    PacketLogger::Instance().Log(PktDir::RX, 0, 0, 0, 0, 0, buf, len);
    return true;
}

// ==================== ASDU 处理 ====================

void Iec104Master::HandleGIResponseDI(const uint8_t* asdu, size_t asduLen, int chIdx, const IEC104DeviceConfig* dev) {
    if (asduLen < 6 || !dev) return;
    uint8_t vsq = asdu[1]; int count = vsq & 0x7F; size_t pos = 6;
    auto& mgr = RemoteDataMgr::Instance();
    uint64_t now = NowMs();
    for (int i = 0; i < count && pos + 4 <= asduLen; i++) {
        uint32_t ioa = ReadIOA(asdu + pos); pos += 3;
        bool val = (asdu[pos++] & 0x01) != 0;
        for (auto& di : dev->diList) { if (di.ioa == ioa) mgr.SetDi(di.ch, di.dev, di.point, val, now, true); }
    }
    (void)chIdx;
}

void Iec104Master::HandleGIResponseAI(const uint8_t* asdu, size_t asduLen, int chIdx, const IEC104DeviceConfig* dev) {
    if (asduLen < 6 || !dev) return;
    uint8_t type = asdu[0]; uint8_t vsq = asdu[1]; int count = vsq & 0x7F; size_t pos = 6;
    auto& mgr = RemoteDataMgr::Instance();
    for (int i = 0; i < count; i++) {
        if (pos + 3 > asduLen) break;
        uint32_t ioa = ReadIOA(asdu + pos); pos += 3;
        double rawVal = 0;
        if (type == IecType::M_ME_NC_1 && pos + 5 <= asduLen) { rawVal = ParseFloatValue(asdu + pos); pos += 5; }
        else if (type == IecType::M_ME_NA_1 && pos + 3 <= asduLen) { rawVal = ParseNormalizedValue(asdu + pos); pos += 3; }
        else break;
        for (auto& ai : dev->aiList) { if (ai.ioa == ioa) mgr.SetAi(ai.ch, ai.dev, ai.point, rawVal * ai.scale + ai.offset); }
    }
    (void)chIdx;
}

void Iec104Master::HandleGIResponseEnergy(const uint8_t* asdu, size_t asduLen, int chIdx, const IEC104DeviceConfig* dev) {
    if (asduLen < 6 || !dev || asdu[0] != IecType::M_IT_NA_1) return;
    uint8_t vsq = asdu[1]; int count = vsq & 0x7F; size_t pos = 6;
    auto& mgr = RemoteDataMgr::Instance();
    for (int i = 0; i < count && pos + 8 <= asduLen; i++) {
        uint32_t ioa = ReadIOA(asdu + pos); pos += 3;
        int64_t counter = static_cast<int64_t>(static_cast<uint32_t>(asdu[pos]) | (static_cast<uint32_t>(asdu[pos+1]) << 8)
                         | (static_cast<uint32_t>(asdu[pos+2]) << 16) | (static_cast<uint32_t>(asdu[pos+3]) << 24)); pos += 5;
        for (auto& en : dev->energyList) { if (en.ioa == ioa) mgr.SetAi(en.ch, en.dev, en.point, static_cast<double>(counter) * en.scale + en.offset); }
    }
    (void)chIdx;
}

// ==================== 总召唤 ====================

void Iec104Master::SendTotalInterrogation(socket& sock, uint32_t& sendSeq, uint32_t recvSeq) {
    uint8_t asdu[] = {IecType::C_IC_NA_1, 0x01, IecCOT::ACTIVATION, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x14};
    SendIFrame(sock, asdu, sizeof(asdu), sendSeq, recvSeq);
}

void Iec104Master::SendClockSync(socket& sock, uint32_t& sendSeq, uint32_t recvSeq) {
    auto now = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(now);
    struct tm t;
#ifdef _WIN32
    localtime_s(&t, &tt);
#else
    localtime_r(&tt, &t);
#endif
    uint8_t asdu[] = {0x67, 0x01, IecCOT::ACTIVATION, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00,
        static_cast<uint8_t>((t.tm_sec * 1000) & 0xFF), static_cast<uint8_t>(((t.tm_sec * 1000) >> 8) & 0xFF),
        static_cast<uint8_t>(t.tm_min & 0x3F), static_cast<uint8_t>(t.tm_hour & 0x1F),
        static_cast<uint8_t>(t.tm_mday & 0x1F), static_cast<uint8_t>((t.tm_mon + 1) & 0x0F),
        static_cast<uint8_t>((t.tm_year - 100) & 0x7F)};
    SendIFrame(sock, asdu, sizeof(asdu), sendSeq, recvSeq);
}

// ==================== I-frame 分发 ====================

bool Iec104Master::HandleIFrame(socket& sock, const uint8_t* asdu, size_t asduLen, int chIdx, const IEC104DeviceConfig* dev) {
    if (asduLen < 6) return true;
    uint8_t type = asdu[0]; uint8_t cot = asdu[2];
    switch (cot) {
    case IecCOT::SPONTANEOUS:
        if (type == IecType::M_SP_NA_1 || type == IecType::M_SP_TB_1) HandleGIResponseDI(asdu, asduLen, chIdx, dev);
        else HandleGIResponseAI(asdu, asduLen, chIdx, dev);
        break;
    case IecCOT::BACKGROUND: case IecCOT::REQUEST: case IecCOT::ACTIVATION_TERM:
        if (type == IecType::M_SP_NA_1 || type == IecType::M_SP_TB_1) HandleGIResponseDI(asdu, asduLen, chIdx, dev);
        else if (type == IecType::M_ME_NC_1 || type == IecType::M_ME_NA_1) HandleGIResponseAI(asdu, asduLen, chIdx, dev);
        else if (type == IecType::M_IT_NA_1) HandleGIResponseEnergy(asdu, asduLen, chIdx, dev);
        break;
    }
    SendSFrame(sock);
    return true;
}

// ==================== 通道线程 ====================

void Iec104Master::ChannelThread(int chIdx) {
    if (chIdx < 0 || chIdx >= static_cast<int>(config_.channels.size())) return;
    IEC104ChannelConfig& ch = config_.channels[chIdx];
    int giMs = ch.giCycleS > 0 ? ch.giCycleS * 1000 : 0;
    int clockMs = ch.clockSyncEnable && ch.clockSyncIntervalS > 0 ? ch.clockSyncIntervalS * 1000 : 0;

    struct Endpoint { std::string ip; int port; };
    Endpoint endpoints[] = {{ch.ip, static_cast<int>(ch.port)}, {ch.standbyIp, ch.standbyPort > 0 ? ch.standbyPort : static_cast<int>(ch.port)}};
    int currentEp = 0;

    // M2 修复：每连接维护 I 帧发/收序号，写入 ctrl 字段供从站按 IEC 104
    // 规范滑动窗口 (k=12/w=8) 确认。新连接建立时重置为 0。
    uint32_t sendSeq = 0;
    uint32_t recvSeq = 0;

    while (running_) {
        socket sock; bool connected = false;
        int tried = 0;
        while (running_ && !connected && tried < 2) {
            int idx = (currentEp + tried) % 2; tried++;
            if (endpoints[idx].port == 0) continue;
            if ((idx == 1) && ch.standbyIp.empty()) continue;
            int retryLeft = ch.retryCount;
            while (running_ && !connected && retryLeft > 0) {
                try {
                    sock.connect(endpoints[idx].ip, static_cast<uint16_t>(endpoints[idx].port));
                    connected = true; currentEp = idx;
                    sendSeq = 0; recvSeq = 0;   // M2: 新连接重置 seq
                    std::cout << "[IEC104] Channel" << (chIdx+1) << " connected "
                              << endpoints[idx].ip << ":" << endpoints[idx].port
                              << (idx == 1 ? " (standby)" : "") << std::endl;
                    if (SendUFrame(sock, CTRL_U_STARTDT_ACT)) {
                        uint8_t buf[RECV_BUF]; size_t len = 0;
                        if (RecvFrame(sock, 5000, buf, len)) {
                            uint32_t ctrl = (uint32_t)((uint32_t)buf[2] | ((uint32_t)buf[3] << 8) | ((uint32_t)buf[4] << 16) | ((uint32_t)buf[5] << 24));
                            if (ctrl == CTRL_U_STARTDT_CON) std::cout << "[IEC104] STARTDT confirmed" << std::endl;
                        }
                    }
                    retryLeft = ch.retryCount;
                    if (giMs > 0) SendTotalInterrogation(sock, sendSeq, recvSeq);
                    if (clockMs > 0) SendClockSync(sock, sendSeq, recvSeq);
                } catch (const std::exception& e) {
                    retryLeft--;
                    std::cerr << "[IEC104] " << endpoints[idx].ip << ":" << endpoints[idx].port
                              << " connect failed: " << e.what() << std::endl;
                    if (retryLeft > 0) for (int i = 0; i < 25 && running_; i++) std::this_thread::sleep_for(std::chrono::milliseconds(200));
                }
            }
        }
        if (!connected) {
            std::cerr << "[IEC104] Channel" << (chIdx+1) << " all endpoints failed, retry..." << std::endl;
            for (int i = 0; i < 50 && running_; i++) std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }

        auto lastGI = std::chrono::steady_clock::now();
        auto lastClock = std::chrono::steady_clock::now();
        uint8_t buf[RECV_BUF];

        while (running_ && connected) {
            try {
                size_t len = 0; bool gotFrame = false;
                try { gotFrame = RecvFrame(sock, 1000, buf, len); }
                catch (const socket_error& e) { if (e.code() == socket_errc::timeout) gotFrame = false; else throw; }
                if (gotFrame && len >= 6) {
                    uint32_t ctrl = (uint32_t)((uint32_t)buf[2] | ((uint32_t)buf[3] << 8) | ((uint32_t)buf[4] << 16) | ((uint32_t)buf[5] << 24));
                    size_t asduLen = len > 6 ? len - 6 : 0;
                    const uint8_t* asdu = buf + 6;
                    if ((ctrl & 0x03) == 0x03) { /* U-frame */ }
                    else if ((ctrl & 0x03) == 0x01) { /* S-frame */ }
                    else if (asduLen > 0) {
                        // M2: 收到 I 帧后 recvSeq +1，等下次自己发帧时把它填进 rNr
                        recvSeq = (recvSeq + 1) & 0x7FFF;
                        uint16_t coa = 0;
                        if (asduLen >= 6) coa = static_cast<uint16_t>(static_cast<uint16_t>(asdu[4]) | (static_cast<uint16_t>(asdu[5]) << 8));
                        IEC104DeviceConfig* dev = FindDevice(chIdx, coa);
                        HandleIFrame(sock, asdu, asduLen, chIdx, dev);
                    }
                }
                auto now = std::chrono::steady_clock::now();
                if (giMs > 0 && (now - lastGI >= std::chrono::milliseconds(giMs))) { SendTotalInterrogation(sock, sendSeq, recvSeq); lastGI = now; }
                if (clockMs > 0 && (now - lastClock >= std::chrono::milliseconds(clockMs))) { SendClockSync(sock, sendSeq, recvSeq); lastClock = now; }
            } catch (const socket_error& e) {
                std::cerr << "[IEC104] Disconnected: " << e.what() << std::endl;
                connected = false;
                if (ch.fallback && currentEp != 0) currentEp = 0;
                break;
            }
        }
        try { sock.close(); } catch (...) {}
        std::cout << "[IEC104] Channel" << (chIdx+1) << " disconnected" << std::endl;
    }
}

// ==================== 设备查找 ====================

IEC104DeviceConfig* Iec104Master::FindDevice(int chIdx, uint16_t commonAddr) {
    if (chIdx < 0 || chIdx >= static_cast<int>(config_.channels.size())) return nullptr;
    for (auto& dev : config_.channels[chIdx].devices) if (dev.commonAddr == commonAddr) return &dev;
    return nullptr;
}

// ==================== 配置加载 ====================

bool Iec104Master::LoadConfig(const std::string& path) {
    IniReader ini; if (!ini.Load(path)) { std::cerr << "[IEC104] Cannot open: " << path << std::endl; return false; }
    config_ = IEC104MasterConfig{};
    for (auto& k : ini.Keys("global")) {
        auto v = ini.Get("global", k, ""); auto kl = ToLower(k);
        if (kl == "timeout_ms") config_.timeoutMs = SafeStoi(v, 5000);
        else if (kl == "verbose") config_.verbose = SafeStoi(v, 0);
    }
    for (auto& sec : ini.Sections()) {
        std::string low = ToLower(sec); if (low == "global") continue;
        if (StartsWith(low, "channel_")) {
            IEC104ChannelConfig ch;
            for (auto& k : ini.Keys(sec)) {
                auto v = ini.Get(sec, k, ""); auto kl = ToLower(k);
                if (kl == "ip") ch.ip = v;
                else if (kl == "port") ch.port = static_cast<uint16_t>(SafeStoi(v, 2404));
                else if (kl == "timeout_ms") ch.timeoutMs = SafeStoi(v, 5000);
                else if (kl == "retry_count") ch.retryCount = SafeStoi(v, 3);
                else if (kl == "gi_cycle_s") ch.giCycleS = SafeStoi(v, 300);
                else if (kl == "clock_sync_enable") ch.clockSyncEnable = SafeStoi(v, 1);
                else if (kl == "spontaneous_enable") ch.spontaneousEnable = SafeStoi(v, 1);
                else if (kl == "standby_ip") ch.standbyIp = v;
                else if (kl == "standby_port") ch.standbyPort = SafeStoi(v, 0);
                else if (kl == "fallback") ch.fallback = SafeStoi(v, 1);
            }
            if (ch.timeoutMs <= 0) ch.timeoutMs = config_.timeoutMs;
            config_.channels.push_back(ch);
            continue;
        }
        if (StartsWith(low, "device_")) {
            if (config_.channels.empty()) continue;
            int chIdx = 0; IEC104DeviceConfig dev;
            for (auto& k : ini.Keys(sec)) {
                auto v = ini.Get(sec, k, ""); auto kl = ToLower(k);
                if (kl == "channel") chIdx = SafeStoi(v, 1) - 1;
                else if (kl == "common_addr") dev.commonAddr = static_cast<uint16_t>(SafeStoi(v, 1));
                else if (kl == "desc") dev.name = v;
                else if (kl == "template") dev.templateName = v;
            }
            if (chIdx >= 0 && chIdx < static_cast<int>(config_.channels.size()))
                config_.channels[chIdx].devices.push_back(dev);
        }
    }
    if (config_.channels.empty()) { std::cerr << "[IEC104] No channels" << std::endl; return false; }
    int dc = 0; for (auto& c : config_.channels) dc += static_cast<int>(c.devices.size());
    std::cout << "[IEC104] Loaded " << config_.channels.size() << " channels " << dc << " devices" << std::endl;
    return true;
}

bool Iec104Master::Start() {
    if (running_) return false;
    running_ = true;
    for (size_t i = 0; i < config_.channels.size(); i++)
        threads_.emplace_back(&Iec104Master::ChannelThread, this, static_cast<int>(i));
    std::cout << "[IEC104] Started " << config_.channels.size() << " channels" << std::endl;
    return true;
}

void Iec104Master::Stop() { running_ = false; for (auto& t : threads_) if (t.joinable()) t.join(); threads_.clear(); }

// ==================== Iec104MasterModule ====================

struct Iec104MasterModule::Impl { Iec104Master master; std::string cfgPath; bool loaded = false; bool running = false; };
Iec104MasterModule::Iec104MasterModule() : impl_(std::make_unique<Impl>()) {}
Iec104MasterModule::~Iec104MasterModule() { Stop(); }
bool Iec104MasterModule::LoadConfig(const std::string& cfgPath) { impl_->cfgPath = cfgPath; impl_->loaded = impl_->master.LoadConfig(cfgPath); return impl_->loaded; }
bool Iec104MasterModule::ValidateConfig(const std::string& cfgPath, std::vector<std::string>& errors) { Iec104Master m; if (!m.LoadConfig(cfgPath)) { errors.push_back("Cannot load: " + cfgPath); return false; } return true; }
bool Iec104MasterModule::Start() { if (impl_->running) return true; impl_->running = impl_->master.Start(); return impl_->running; }
void Iec104MasterModule::Stop() { if (!impl_->running) return; impl_->master.Stop(); impl_->running = false; }
bool Iec104MasterModule::IsRunning() const { return impl_->running; }
REGISTER_MODULE("iec104_master", Iec104MasterModule)
