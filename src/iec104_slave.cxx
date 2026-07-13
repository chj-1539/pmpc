//=============================================================================
// iec104_slave.cxx - IEC 60870-5-104 slave implementation
//
// Server (slave) role:
//   Listen TCP -> STARTDT -> receive commands -> process and respond
//   DI change active upload, AI cycle/change upload
//=============================================================================

#include "iec104_slave.h"
#include "module_factory.h"
#include "ini_reader.h"
#include "packet_logger.h"
#include "str_util.h"
#include <iostream>
#include <sstream>

// 注：DecideRemoteControlTargets 已在 iec104_slave.h 里 inline 定义
// （方便测试直接链接不拖依赖图）。见 tests/test_iec104_slave_remote_ctrl.cxx。
#include <cstring>
#include <algorithm>
#include <cmath>

// Constants
constexpr size_t RECV_BUF = 4096;
constexpr int RECV_TIMEOUT_MS = 1000;

// ==================== Helpers ====================

int Iec104Slave::SafeStoi(const std::string& s, int def) {
    try { return std::stoi(s); } catch (...) { return def; }
}
double Iec104Slave::SafeStod(const std::string& s, double def) {
    try { return std::stod(s); } catch (...) { return def; }
}

uint64_t Iec104Slave::NowMs() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

uint32_t Iec104Slave::ReadCtrl(const uint8_t* buf) {
    return static_cast<uint32_t>(buf[2])
         | (static_cast<uint32_t>(buf[3]) << 8)
         | (static_cast<uint32_t>(buf[4]) << 16)
         | (static_cast<uint32_t>(buf[5]) << 24);
}

uint32_t Iec104Slave::GetSNr(uint32_t ctrl) {
    return (ctrl >> 1) & 0x7FFF;
}

uint32_t Iec104Slave::GetRNr(uint32_t ctrl) {
    return (ctrl >> 17) & 0x7FFF;
}

// ==================== Iec104Slave ====================

Iec104Slave::Iec104Slave() {}
Iec104Slave::~Iec104Slave() { Stop(); }

// ==================== Config loading ====================

bool Iec104Slave::LoadConfig(const std::string& path) {
    IniReader ini;
    if (!ini.Load(path)) {
        std::cerr << "[Iec104Slave] Cannot open: " << path << std::endl;
        return false;
    }

    config_ = SlaveConfig{};
    config_.port = ini.GetInt("global", "port", 2404);
    config_.maxClients = ini.GetInt("global", "max_clients", 10);
    config_.verbose = ini.GetInt("global", "verbose", 1);
    config_.spontaneousWithTimestamp = ini.GetInt("global", "spontaneous_with_timestamp", 1);
    config_.aiUploadMode = ini.Get("global", "ai_upload_mode", "change");
    config_.aiCycleS = ini.GetInt("global", "ai_cycle_s", 60);
    config_.clockSyncEnable = ini.GetInt("global", "clock_sync_enable", 1);
    config_.adjustLocalClock = ini.GetInt("global", "adjust_local_clock", 0);

    auto sections = ini.Sections();

    // Parse [listen_N]
    for (const auto& sec : sections) {
        std::string lowSec = ToLower(sec);
        if (!StartsWith(lowSec, "listen_")) continue;
        SlaveBind sb;
        sb.allowedIP = ini.Get(sec, "ip", "");
        sb.port = static_cast<uint16_t>(ini.GetInt(sec, "port", 0));
        if (sb.port > 0) binds_.push_back(sb);
    }

    for (const auto& sec : sections) {
        std::string lowSec = ToLower(sec);
        if (lowSec == "global") continue;
        if (StartsWith(lowSec, "listen_")) continue;
        if (!StartsWith(lowSec, "device_")) continue;

        SlaveDeviceConfig dev;
        dev.commonAddr = static_cast<uint16_t>(ini.GetInt(sec, "common_addr", 0));
        dev.desc = ini.Get(sec, "desc", "");
        if (dev.commonAddr == 0) continue;

        auto keys = ini.Keys(sec);
        for (const auto& key : keys) {
            std::string lowKey = ToLower(key);
            std::string val = ini.Get(sec, key, "");
            if (lowKey == "common_addr" || lowKey == "desc") continue;

            auto kv = ParseKeyValues(val);
            auto getKV = [&](const std::string& k) -> std::string {
                for (auto& [kk, vv] : kv)
                    if (ToLower(kk) == k) return vv;
                return "";
            };
            auto st = [](const std::string& s) { return SafeStoi(s, 0); };

            if (StartsWith(lowKey, "di_")) {
                SlaveDIMapping di;
                di.ioa   = static_cast<uint32_t>(st(getKV("ioa")));
                di.ch    = static_cast<uint16_t>(st(getKV("ch")));
                di.dev   = static_cast<uint16_t>(st(getKV("dev")));
                di.point = static_cast<uint16_t>(st(getKV("point")));
                if (di.ioa) dev.diMap[di.ioa] = di;
            }
            else if (StartsWith(lowKey, "ai_")) {
                SlaveAIMapping ai;
                ai.ioa   = static_cast<uint32_t>(st(getKV("ioa")));
                ai.ch    = static_cast<uint16_t>(st(getKV("ch")));
                ai.dev   = static_cast<uint16_t>(st(getKV("dev")));
                ai.point = static_cast<uint16_t>(st(getKV("point")));
                std::string t = ToLower(getKV("type"));
                if (t == "m_me_na_1") ai.type = IecType::M_ME_NA_1;
                else if (t == "m_me_nb_1") ai.type = IecType::M_ME_NB_1;
                else if (t == "m_me_nc_1") ai.type = IecType::M_ME_NC_1;
                else if (t == "m_me_td_1") ai.type = IecType::M_ME_TD_1;
                ai.scale  = SafeStod(getKV("scale"), 1.0);
                ai.offset = SafeStod(getKV("offset"), 0.0);
                if (ai.ioa) dev.aiMap[ai.ioa] = ai;
            }
            else if (StartsWith(lowKey, "energy_")) {
                SlaveEnergyMapping en;
                en.ioa   = static_cast<uint32_t>(st(getKV("ioa")));
                en.ch    = static_cast<uint16_t>(st(getKV("ch")));
                en.dev   = static_cast<uint16_t>(st(getKV("dev")));
                en.point = static_cast<uint16_t>(st(getKV("point")));
                en.scale  = SafeStod(getKV("scale"), 1.0);
                en.offset = SafeStod(getKV("offset"), 0.0);
                if (en.ioa) dev.energyMap[en.ioa] = en;
            }
            else if (StartsWith(lowKey, "do_")) {
                SlaveDOMapping do_;
                do_.ioa   = static_cast<uint32_t>(st(getKV("ioa")));
                do_.val   = st(getKV("val"));
                do_.ch    = static_cast<uint16_t>(st(getKV("ch")));
                do_.dev   = static_cast<uint16_t>(st(getKV("dev")));
                do_.point = static_cast<uint16_t>(st(getKV("point")));
                if (do_.ioa) dev.doMap[do_.ioa].push_back(do_);
            }
            else if (StartsWith(lowKey, "ao_")) {
                SlaveAOMapping ao;
                ao.ioa   = static_cast<uint32_t>(st(getKV("ioa")));
                ao.ch    = static_cast<uint16_t>(st(getKV("ch")));
                ao.dev   = static_cast<uint16_t>(st(getKV("dev")));
                ao.point = static_cast<uint16_t>(st(getKV("point")));
                ao.scale  = SafeStod(getKV("scale"), 1.0);
                ao.offset = SafeStod(getKV("offset"), 0.0);
                if (ao.ioa) dev.aoMap[ao.ioa] = ao;
            }
        }
        config_.devices.push_back(dev);

        if (config_.verbose >= 1)
            std::cout << "[Iec104Slave] Device: common_addr=" << dev.commonAddr
                     << " DI=" << dev.diMap.size() << " AI=" << dev.aiMap.size()
                     << " DO=" << dev.doMap.size() << " AO=" << dev.aoMap.size()
                     << " Energy=" << dev.energyMap.size() << std::endl;
    }

    if (config_.devices.empty()) {
        std::cerr << "[Iec104Slave] No devices configured" << std::endl;
        return false;
    }

    std::cout << "[Iec104Slave] Config: port=" << config_.port
             << " AI_mode=" << config_.aiUploadMode
             << " AI_cycle=" << config_.aiCycleS << "s"
             << " devices=" << config_.devices.size() << std::endl;
    return true;
}

// ==================== Start / Stop ====================

bool Iec104Slave::Start() {
    if (running_) return true;
    running_ = true;

    if (binds_.empty())
        binds_.push_back({"", static_cast<uint16_t>(config_.port)});

    for (auto& bind : binds_) {
        try {
            socket sock;
            sock.bind(socket_addr("0.0.0.0", static_cast<uint16_t>(bind.port)));
            sock.listen(config_.maxClients);
            listenSocks_.push_back(std::move(sock));
        } catch (const socket_error& e) {
            std::cerr << "[Iec104Slave] Bind port " << bind.port
                     << " failed: " << e.what() << std::endl;
            continue;
        }
    }
    if (listenSocks_.empty()) { running_ = false; return false; }
    for (size_t i = 0; i < listenSocks_.size(); i++)
        acceptThreads_.emplace_back([this, i]() { AcceptLoop(binds_[i], listenSocks_[i]); });

    // Subscribe EventBus
    tokenDI_ = EventBus::Subscribe<DIChange>(
        [this](const DIChange& e) { SendDIActiveUpload(e); });
    tokenAI_ = EventBus::Subscribe<AIChange>(
        [this](const AIChange& e) {
            // AI change upload
            if (config_.aiUploadMode == "change" || config_.aiUploadMode == "both") {
                for (auto& dev : config_.devices) {
                    for (auto& [ioa, ai] : dev.aiMap) {
                        if (ai.ch == e.channel && ai.dev == e.device && ai.point == e.point) {
                            SendAIActiveUpload(e, ioa, ai, dev);
                            return;
                        }
                    }
                }
            }
        });

    // Start timer (AI cycle upload)
    if (config_.aiUploadMode == "cycle" || config_.aiUploadMode == "both") {
        timerThr_ = std::thread(&Iec104Slave::TimerThread, this);
    }

    std::cout << "[Iec104Slave] Started on port " << config_.port << std::endl;
    return true;
}

void Iec104Slave::Stop() {
    if (!running_) return;
    running_ = false;

    // Unsubscribe EventBus
    EventBus::Unsubscribe<DIChange>(tokenDI_);
    EventBus::Unsubscribe<AIChange>(tokenAI_);

    // Close listeners
    for (auto& sock : listenSocks_)
        try { sock.close(); } catch (...) {}
    for (auto& t : acceptThreads_)
        if (t.joinable()) t.join();
    listenSocks_.clear();
    acceptThreads_.clear();

    // Close all client connections and wait for threads to exit
    {
        std::lock_guard<std::mutex> lock(clientsMtx_);
        for (auto& c : clients_)
            try { c->sock.close(); } catch (...) {}
        clients_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(clientMtx_);
        for (auto& e : clientThreads_)
            if (e.thr.joinable()) e.thr.join();
        clientThreads_.clear();
    }

    if (timerThr_.joinable()) timerThr_.join();

    std::cout << "[Iec104Slave] Stopped" << std::endl;
}

// ==================== Accept loop ====================

void Iec104Slave::AcceptLoop(const SlaveBind& bind, socket& listenSock) {
    std::cout << "[Iec104Slave] Listen thread: " << (bind.allowedIP.empty() ? "ALL" : bind.allowedIP)
             << ":" << bind.port << std::endl;
    int cleanupCounter = 0;
    while (running_) {
        try {
            socket_addr peer;
            socket client = listenSock.accept(&peer);
            if (!running_) break;

            // IP filter
            if (!bind.allowedIP.empty() && peer.host() != bind.allowedIP) {
                continue;
            }

            // Start client thread; the thread owns the socket
            // M4 修复：每个 client thread 关联一个 done 标志，线程 return
            // 前置位，让下面的 cleanup 循环能真正 erase 已结束线程。
            auto done = std::make_shared<std::atomic<bool>>(false);
            {
                std::lock_guard<std::mutex> lock(clientMtx_);
                ClientThreadEntry entry;
                entry.done = done;
                entry.thr = std::thread([this, s = std::move(client), done]() mutable {
                    ClientThread(std::move(s));
                    done->store(true);
                });
                clientThreads_.push_back(std::move(entry));
            }

            // Periodically clean up finished client threads
            cleanupCounter++;
            if (cleanupCounter >= 10) {
                cleanupCounter = 0;
                std::lock_guard<std::mutex> lock2(clientMtx_);
                CleanupFinishedClientThreads(clientThreads_);
            }
        } catch (const socket_error&) {
            if (running_) break;
        }
    }
    std::cout << "[Iec104Slave] Listen thread exit " << bind.allowedIP << ":" << bind.port << std::endl;
}

// ==================== Client thread ====================

void Iec104Slave::ClientThread(socket clientSock) {
    if (config_.verbose >= 1)
        std::cout << "[Iec104Slave] Client connected" << std::endl;

    try {
        clientSock.set_recv_timeout(std::chrono::milliseconds(RECV_TIMEOUT_MS));
    } catch (...) {}

    // C1 修复：把这条 client 连接注册进 clients_，让 active/cycle upload 能
    // 找到它。用 shared_ptr 保证下面 recv/send 循环拿到的 &sNr, &rNr 与
    // 主动上传遍历时看到的是同一份状态。
    auto info = std::make_shared<ClientInfo>();
    info->sock = std::move(clientSock);
    {
        std::lock_guard<std::mutex> lk(clientsMtx_);
        clients_.push_back(info);
    }

    SlaveDeviceConfig* currentDev = nullptr;
    uint8_t buf[RECV_BUF];

    while (running_) {
        size_t len = 0;
        bool gotFrame = false;
        bool peerClosed = false;

        try {
            gotFrame = RecvFrame(info->sock, 1000, buf, len, &peerClosed);
        } catch (const socket_error& e) {
            if (e.code() == socket_errc::timeout) {
                gotFrame = false;
            } else {
                break;
            }
        }
        if (peerClosed) break;    // C1 修复：对端 close 时退出，让下方 erase 生效

        if (gotFrame) {
            if (!HandleFrame(info->sock, buf, len, info->sNr, info->rNr, currentDev)) {
                break;
            }
        }

        if (!running_) break;
    }

    // C1 修复：退出前把这条 client 从 clients_ 里删掉，避免主动上传路径
    // 继续尝试往已关闭的 socket 写。
    {
        std::lock_guard<std::mutex> lk(clientsMtx_);
        clients_.erase(std::remove(clients_.begin(), clients_.end(), info),
                       clients_.end());
    }

    if (config_.verbose >= 1)
        std::cout << "[Iec104Slave] Client disconnected" << std::endl;
}

// ==================== Frame send / receive ====================

bool Iec104Slave::SendUFrame(socket& sock, uint32_t ctrl) {
    uint8_t buf[] = {FRAME_START, 0x04,
        static_cast<uint8_t>(ctrl & 0xFF),
        static_cast<uint8_t>((ctrl >> 8) & 0xFF),
        static_cast<uint8_t>((ctrl >> 16) & 0xFF),
        static_cast<uint8_t>((ctrl >> 24) & 0xFF)};
    PacketLogger::Instance().Log(PktDir::TX, 0, 0, 0, 0, 0, buf, 6);
    size_t total = 0;
    while (total < sizeof(buf)) {
        size_t n = sock.send(buf + total, sizeof(buf) - total);
        if (n == 0) return false;
        total += n;
    }
    return true;
}

bool Iec104Slave::SendSFrame(socket& sock, uint32_t rNr) {
    uint32_t ctrl = 0x01 | (rNr << 17);
    return SendUFrame(sock, ctrl); // reuse U-frame send (ctrl field differs but format is same)
}

bool Iec104Slave::SendIFrame(socket& sock, uint32_t& sendSNr, uint32_t rNr,
                               const uint8_t* asdu, size_t asduLen) {
    if (asduLen > 249) return false;
    uint32_t ctrl = (sendSNr << 1) | (rNr << 17);
    sendSNr = (sendSNr + 1) & 0x7FFF;

    uint8_t buf[256];
    size_t pos = 0;
    buf[pos++] = FRAME_START;
    buf[pos++] = static_cast<uint8_t>(4 + asduLen); // APCI length
    buf[pos++] = static_cast<uint8_t>(ctrl & 0xFF);
    buf[pos++] = static_cast<uint8_t>((ctrl >> 8) & 0xFF);
    buf[pos++] = static_cast<uint8_t>((ctrl >> 16) & 0xFF);
    buf[pos++] = static_cast<uint8_t>((ctrl >> 24) & 0xFF);
    std::memcpy(buf + pos, asdu, asduLen);
    pos += asduLen;

    PacketLogger::Instance().Log(PktDir::TX, 0, 0, 0, asduLen > 0 ? asdu[0] : 0, 0, buf, pos);

    size_t total = 0;
    while (total < pos) {
        size_t n = sock.send(buf + total, pos - total);
        if (n == 0) return false;
        total += n;
    }
    return true;
}

bool Iec104Slave::RecvFrame(socket& sock, int timeoutMs, uint8_t* buf, size_t& len,
                            bool* peerClosedOut)
{
    if (peerClosedOut) *peerClosedOut = false;
    sock.set_recv_timeout(std::chrono::milliseconds(timeoutMs));

    if (sock.recv(buf, 1) != 1) {
        if (peerClosedOut) *peerClosedOut = true;   // recv 返回 0 → EOF
        return false;
    }
    if (buf[0] != FRAME_START) return false;
    if (sock.recv(buf + 1, 1) != 1) {
        if (peerClosedOut) *peerClosedOut = true;
        return false;
    }

    uint8_t apduLen = buf[1];
    if (apduLen < 4 || apduLen > 253) {
        // M12 修复：apduLen 无效意味着帧同步已丢失。任何"继续读 N 字节
        // 排空"都是猜测，只会把噪声当帧内容。安全的做法是关闭 socket，
        // 让 client_handler 循环退出并释放这条连接。
        try { sock.shutdown(2); } catch (...) {}
        try { sock.close();   } catch (...) {}
        return false;
    }

    size_t remaining = apduLen;
    size_t total = 0;
    while (total < remaining) {
        size_t n = sock.recv(buf + 2 + total, remaining - total);
        if (n == 0) {
            if (peerClosedOut) *peerClosedOut = true;
            return false;
        }
        total += n;
    }
    len = 2 + remaining;
    PacketLogger::Instance().Log(PktDir::RX, 0, 0, 0, 0, 0, buf, len);
    return true;
}

// ==================== Frame handling ====================

bool Iec104Slave::HandleFrame(socket& sock, const uint8_t* buf, size_t len,
                                uint32_t& sNr, uint32_t& rNr,
                                SlaveDeviceConfig*& currentDev) {
    uint32_t ctrl = ReadCtrl(buf);
    size_t asduLen = len > 6 ? len - 6 : 0;
    const uint8_t* asdu = buf + 6;

    // U-format
    if ((ctrl & 0x03) == 0x03) {
        if (ctrl == CTRL_U_STARTDT_ACT) {
            SendUFrame(sock, CTRL_U_STARTDT_CON);
            if (config_.verbose >= 2)
                std::cout << "[Iec104Slave] STARTDT confirmed" << std::endl;
        } else if (ctrl == CTRL_U_STOPDT_ACT) {
            SendUFrame(sock, CTRL_U_STOPDT_CON);
        } else if (ctrl == CTRL_U_TESTFR_ACT) {
            SendUFrame(sock, CTRL_U_TESTFR_CON);
        }
        return true;
    }

    // S-format
    if ((ctrl & 0x03) == 0x01) {
        rNr = GetRNr(ctrl);
        return true;
    }

    // I-format
    if ((ctrl & 0x03) != 0) return true;
    if (asduLen < 6) return true;

    uint32_t recvSNr = GetSNr(ctrl);
    rNr = GetRNr(ctrl);
    (void)recvSNr;

    uint8_t type = asdu[0];
    uint8_t cot = asdu[2];
    uint16_t coa = static_cast<uint16_t>(static_cast<uint16_t>(asdu[4])
                   | (static_cast<uint16_t>(asdu[5]) << 8));

    // Find device by COA
    currentDev = FindDevice(coa);

    switch (type) {
    // Total interrogation C_IC_NA_1
    case IecType::C_IC_NA_1: {
        if (cot != IecCOT::ACTIVATION || !currentDev) break;
        // Activation confirm
        uint8_t ackAsdu[] = {IecType::C_IC_NA_1, 0x01, IecCOT::ACTIVATION_CON, 0x00,
                             static_cast<uint8_t>(coa & 0xFF),
                             static_cast<uint8_t>((coa >> 8) & 0xFF),
                             0x00, 0x00, 0x00, 0x14};
        SendIFrame(sock, sNr, rNr, ackAsdu, sizeof(ackAsdu));

        // Send DI
        BuildGIResponseDI(sock, sNr, rNr, *currentDev);
        // Send AI
        BuildGIResponseAI(sock, sNr, rNr, *currentDev);
        // Send Energy
        BuildGIResponseEnergy(sock, sNr, rNr, *currentDev);

        // Activation termination
        uint8_t termAsdu[] = {IecType::C_IC_NA_1, 0x01, IecCOT::ACTIVATION_TERM, 0x00,
                              static_cast<uint8_t>(coa & 0xFF),
                              static_cast<uint8_t>((coa >> 8) & 0xFF),
                              0x00, 0x00, 0x00, 0x14};
        SendIFrame(sock, sNr, rNr, termAsdu, sizeof(termAsdu));

        if (config_.verbose >= 1)
            std::cout << "[Iec104Slave] GI response complete (COA=" << coa << ")" << std::endl;
        break;
    }

    // Remote control C_SC_NA_1 / C_DC_NA_1
    case IecType::C_SC_NA_1:
    case IecType::C_DC_NA_1: {
        if (!currentDev) break;
        uint32_t ioA = static_cast<uint32_t>(asdu[6])
                     | (static_cast<uint32_t>(asdu[7]) << 8)
                     | (static_cast<uint32_t>(asdu[8]) << 16);
        int cmdVal = (asduLen > 9) ? (asdu[9] & 0x01) : 0;

        auto it = currentDev->doMap.find(ioA);
        bool executed = false;
        if (it != currentDev->doMap.end()) {
            auto targets = DecideRemoteControlTargets(it->second, cmdVal);
            for (auto& t : targets) {
                RemoteDataMgr::Instance().SetDoMaster(t.ch, t.dev, t.point, t.doVal);
                executed = true;
                if (config_.verbose >= 1)
                    std::cout << "[Iec104Slave] Remote control: IOA=" << ioA
                              << " val=" << cmdVal
                              << " DO(" << t.ch << "," << t.dev
                              << "," << t.point << ")" << std::endl;
            }
        }
        // H5 修复：以前当 cmdVal 不匹配任何 mapping 时会 fallback 到第一个
        // entry 并强制写 DO —— 那是把不匹配值当成"随便找一个"的错误逻辑，
        // 属于安全隐患（任意 cmdVal 都能触发 SetDoMaster）。改为不写 DO，
        // 用 COT 里的 P/N=1（negative ack）告诉主站请求无效。
        // 见 CLAUDE.md「已知陷阱 / 修复历史」H5 (code review)。

        // Activation confirm （P/N=1 表 negative）
        uint8_t respAsdu[16];
        size_t p = 0;
        respAsdu[p++] = type;
        respAsdu[p++] = 0x01;
        respAsdu[p++] = executed
            ? IecCOT::ACTIVATION_CON
            : static_cast<uint8_t>(IecCOT::ACTIVATION_CON | 0x40);   // COT 高位 P/N=1
        respAsdu[p++] = 0x00;
        respAsdu[p++] = static_cast<uint8_t>(coa & 0xFF);
        respAsdu[p++] = static_cast<uint8_t>((coa >> 8) & 0xFF);
        respAsdu[p++] = asdu[6]; respAsdu[p++] = asdu[7]; respAsdu[p++] = asdu[8]; // IOA
        respAsdu[p++] = static_cast<uint8_t>(cmdVal & 0xFF);
        if (p < asduLen) respAsdu[p++] = asdu[9]; // SE
        SendIFrame(sock, sNr, rNr, respAsdu, p);
        break;
    }

    // Remote adjustment C_SE_NC_1
    case IecType::C_SE_NC_1: {
        if (!currentDev || asduLen < 13) break;
        uint32_t ioA = static_cast<uint32_t>(asdu[6])
                     | (static_cast<uint32_t>(asdu[7]) << 8)
                     | (static_cast<uint32_t>(asdu[8]) << 16);
        // Float value (little-endian IEEE 754)
        uint32_t raw = static_cast<uint32_t>(asdu[9])
                     | (static_cast<uint32_t>(asdu[10]) << 8)
                     | (static_cast<uint32_t>(asdu[11]) << 16)
                     | (static_cast<uint32_t>(asdu[12]) << 24);
        float f;
        std::memcpy(&f, &raw, sizeof(f));

        auto it = currentDev->aoMap.find(ioA);
        if (it != currentDev->aoMap.end()) {
            double val = static_cast<double>(f) * it->second.scale + it->second.offset;
            RemoteDataMgr::Instance().SetAo(it->second.ch, it->second.dev, it->second.point, val);
        }

        // Activation confirm
        uint8_t respAsdu[16];
        std::memcpy(respAsdu, asdu, (asduLen < 16 ? asduLen : 16));
        respAsdu[2] = IecCOT::ACTIVATION_CON;
        SendIFrame(sock, sNr, rNr, respAsdu, asduLen);
        break;
    }

    // Clock sync C_CS_NA_1
    case 0x67: {  // C_CS_NA_1
        if (config_.clockSyncEnable) {
            // Parse CP56Time2a (asdu + 9)
            if (asduLen >= 16) {
                int ms  = asdu[9] | (asdu[10] << 8);
                int min = asdu[11] & 0x3F;
                int hr  = asdu[12] & 0x1F;
                int day = asdu[13] & 0x1F;
                int mon = asdu[14] & 0x0F;
                int yr  = (asdu[15] & 0x7F) + 2000;
                if (config_.verbose >= 1)
                    std::cout << "[Iec104Slave] Clock sync: " << yr << "-" << mon << "-" << day
                             << " " << hr << ":" << min << ":" << (ms/1000) << std::endl;
            }
        }
        // Activation confirm
        uint8_t respAsdu[16];
        std::memcpy(respAsdu, asdu, (asduLen < 16 ? asduLen : 16));
        respAsdu[2] = IecCOT::ACTIVATION_CON;
        SendIFrame(sock, sNr, rNr, respAsdu, asduLen);
        break;
    }

    default:
        break;
    }

    // Send S-frame confirmation
    SendSFrame(sock, recvSNr + 1);
    return true;
}

// ==================== Device lookup ====================

SlaveDeviceConfig* Iec104Slave::FindDevice(uint16_t commonAddr) {
    for (auto& dev : config_.devices)
        if (dev.commonAddr == commonAddr) return &dev;
    return nullptr;
}

// ==================== ASDU building: GI response ====================

void Iec104Slave::BuildGIResponseDI(socket& sock, uint32_t& sNr, uint32_t rNr,
                                      const SlaveDeviceConfig& dev) {
    // Batch pack: max 120 points per frame
    constexpr int MAX_PER_FRAME = 120;
    auto& mgr = RemoteDataMgr::Instance();
    std::vector<SlaveDIMapping> points;
    for (auto& [ioa, di] : dev.diMap) points.push_back(di);
    if (points.empty()) return;

    for (size_t i = 0; i < points.size(); i += MAX_PER_FRAME) {
        size_t cnt = std::min(MAX_PER_FRAME, static_cast<int>(points.size() - i));
        uint8_t asdu[512];
        size_t pos = 0;
        asdu[pos++] = IecType::M_SP_NA_1;      // Type
        asdu[pos++] = static_cast<uint8_t>(cnt | 0x80); // VSQ: sequence + SQ=1
        asdu[pos++] = IecCOT::BACKGROUND;       // COT
        asdu[pos++] = 0x00;
        asdu[pos++] = static_cast<uint8_t>(dev.commonAddr & 0xFF);
        asdu[pos++] = static_cast<uint8_t>((dev.commonAddr >> 8) & 0xFF);

        for (size_t j = 0; j < cnt; j++) {
            auto& p = points[i + j];
            // IOA (3 bytes, little-endian)
            asdu[pos++] = static_cast<uint8_t>(p.ioa & 0xFF);
            asdu[pos++] = static_cast<uint8_t>((p.ioa >> 8) & 0xFF);
            asdu[pos++] = static_cast<uint8_t>((p.ioa >> 16) & 0xFF);
            // SIQ
            DiPoint pt;
            bool val = mgr.GetDi(p.ch, p.dev, p.point, pt) && pt.value;
            asdu[pos++] = val ? 0x01 : 0x00;
        }
        SendIFrame(sock, sNr, rNr, asdu, pos);
    }
}

void Iec104Slave::BuildGIResponseAI(socket& sock, uint32_t& sNr, uint32_t rNr,
                                      const SlaveDeviceConfig& dev) {
    auto& mgr = RemoteDataMgr::Instance();

    for (auto& [ioa, ai] : dev.aiMap) {
        AiPoint pt;
        if (!mgr.GetAi(ai.ch, ai.dev, ai.point, pt)) continue;

        uint8_t asdu[32];
        size_t pos = 0;
        asdu[pos++] = ai.type;
        asdu[pos++] = 0x01;   // VSQ: 1 object, SQ=0
        asdu[pos++] = IecCOT::BACKGROUND;
        asdu[pos++] = 0x00;
        asdu[pos++] = static_cast<uint8_t>(dev.commonAddr & 0xFF);
        asdu[pos++] = static_cast<uint8_t>((dev.commonAddr >> 8) & 0xFF);
        // IOA
        asdu[pos++] = static_cast<uint8_t>(ioa & 0xFF);
        asdu[pos++] = static_cast<uint8_t>((ioa >> 8) & 0xFF);
        asdu[pos++] = static_cast<uint8_t>((ioa >> 16) & 0xFF);

        if (ai.type == IecType::M_ME_NC_1) {
            // IEEE 754 float
            float f = static_cast<float>((pt.value - ai.offset) / ai.scale);
            uint32_t raw;
            std::memcpy(&raw, &f, sizeof(raw));
            asdu[pos++] = static_cast<uint8_t>(raw & 0xFF);
            asdu[pos++] = static_cast<uint8_t>((raw >> 8) & 0xFF);
            asdu[pos++] = static_cast<uint8_t>((raw >> 16) & 0xFF);
            asdu[pos++] = static_cast<uint8_t>((raw >> 24) & 0xFF);
            asdu[pos++] = 0x00; // QDS
        } else if (ai.type == IecType::M_ME_NA_1) {
            // Normalized value
            double norm = (pt.value - ai.offset) / ai.scale;
            int16_t raw = static_cast<int16_t>(std::round(norm * 32768.0));
            asdu[pos++] = static_cast<uint8_t>(raw & 0xFF);
            asdu[pos++] = static_cast<uint8_t>((raw >> 8) & 0xFF);
            asdu[pos++] = 0x00; // QDS
        } else if (ai.type == IecType::M_ME_NB_1) {
            int16_t raw = static_cast<int16_t>((pt.value - ai.offset) / ai.scale);
            asdu[pos++] = static_cast<uint8_t>(raw & 0xFF);
            asdu[pos++] = static_cast<uint8_t>((raw >> 8) & 0xFF);
            asdu[pos++] = 0x00; // QDS
        }

        SendIFrame(sock, sNr, rNr, asdu, pos);
    }
}

void Iec104Slave::BuildGIResponseEnergy(socket& sock, uint32_t& sNr, uint32_t rNr,
                                          const SlaveDeviceConfig& dev) {
    auto& mgr = RemoteDataMgr::Instance();

    for (auto& [ioa, en] : dev.energyMap) {
        AiPoint pt;
        if (!mgr.GetAi(en.ch, en.dev, en.point, pt)) continue;

        uint8_t asdu[24];
        size_t pos = 0;
        asdu[pos++] = IecType::M_IT_NA_1;
        asdu[pos++] = 0x01;
        asdu[pos++] = IecCOT::BACKGROUND;
        asdu[pos++] = 0x00;
        asdu[pos++] = static_cast<uint8_t>(dev.commonAddr & 0xFF);
        asdu[pos++] = static_cast<uint8_t>((dev.commonAddr >> 8) & 0xFF);
        asdu[pos++] = static_cast<uint8_t>(ioa & 0xFF);
        asdu[pos++] = static_cast<uint8_t>((ioa >> 8) & 0xFF);
        asdu[pos++] = static_cast<uint8_t>((ioa >> 16) & 0xFF);

        // Binary counter (4 bytes, little-endian)
        int64_t counter = static_cast<int64_t>((pt.value - en.offset) / en.scale);
        uint32_t cLow = static_cast<uint32_t>(counter & 0xFFFFFFFF);
        asdu[pos++] = static_cast<uint8_t>(cLow & 0xFF);
        asdu[pos++] = static_cast<uint8_t>((cLow >> 8) & 0xFF);
        asdu[pos++] = static_cast<uint8_t>((cLow >> 16) & 0xFF);
        asdu[pos++] = static_cast<uint8_t>((cLow >> 24) & 0xFF);
        asdu[pos++] = 0x00; // QDS

        SendIFrame(sock, sNr, rNr, asdu, pos);
    }
}

// ==================== Active upload ====================

void Iec104Slave::PackFloatASDU(uint8_t* buf, size_t& pos, double value, uint32_t ioa) {
    buf[pos++] = static_cast<uint8_t>(ioa & 0xFF);
    buf[pos++] = static_cast<uint8_t>((ioa >> 8) & 0xFF);
    buf[pos++] = static_cast<uint8_t>((ioa >> 16) & 0xFF);
    float f = static_cast<float>(value);
    uint32_t raw;
    std::memcpy(&raw, &f, sizeof(raw));
    buf[pos++] = static_cast<uint8_t>(raw & 0xFF);
    buf[pos++] = static_cast<uint8_t>((raw >> 8) & 0xFF);
    buf[pos++] = static_cast<uint8_t>((raw >> 16) & 0xFF);
    buf[pos++] = static_cast<uint8_t>((raw >> 24) & 0xFF);
}

void Iec104Slave::PackNormalizedASDU(uint8_t* buf, size_t& pos, double value,
                                       uint32_t ioa, double scale) {
    buf[pos++] = static_cast<uint8_t>(ioa & 0xFF);
    buf[pos++] = static_cast<uint8_t>((ioa >> 8) & 0xFF);
    buf[pos++] = static_cast<uint8_t>((ioa >> 16) & 0xFF);
    int16_t raw = static_cast<int16_t>(std::round(value / scale * 32768.0));
    buf[pos++] = static_cast<uint8_t>(raw & 0xFF);
    buf[pos++] = static_cast<uint8_t>((raw >> 8) & 0xFF);
}

void Iec104Slave::PackCP56Time2a(uint8_t* buf, size_t& pos) {
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
    uint16_t totalMs = static_cast<uint16_t>(t.tm_sec * 1000 + ms);
    buf[pos++] = static_cast<uint8_t>(totalMs & 0xFF);
    buf[pos++] = static_cast<uint8_t>((totalMs >> 8) & 0xFF);
    buf[pos++] = static_cast<uint8_t>(t.tm_min & 0x3F);
    buf[pos++] = static_cast<uint8_t>(t.tm_hour & 0x1F);
    buf[pos++] = static_cast<uint8_t>(t.tm_mday & 0x1F);
    buf[pos++] = static_cast<uint8_t>((t.tm_mon + 1) & 0x0F);
    buf[pos++] = static_cast<uint8_t>((t.tm_year - 100) & 0x7F);
}

void Iec104Slave::SendDIActiveUpload(const DIChange& ev) {
    if (!running_) return;
    uint8_t type = config_.spontaneousWithTimestamp ? IecType::M_SP_TB_1 : IecType::M_SP_NA_1;

    for (auto& dev : config_.devices) {
        for (auto& [ioa, di] : dev.diMap) {
            if (di.ch == ev.channel && di.dev == ev.device && di.point == ev.point) {
                uint8_t asdu[32];
                size_t pos = 0;
                asdu[pos++] = type;
                asdu[pos++] = 0x01;
                asdu[pos++] = IecCOT::SPONTANEOUS;  // COT=3
                asdu[pos++] = 0x00;
                asdu[pos++] = static_cast<uint8_t>(dev.commonAddr & 0xFF);
                asdu[pos++] = static_cast<uint8_t>((dev.commonAddr >> 8) & 0xFF);
                asdu[pos++] = static_cast<uint8_t>(ioa & 0xFF);
                asdu[pos++] = static_cast<uint8_t>((ioa >> 8) & 0xFF);
                asdu[pos++] = static_cast<uint8_t>((ioa >> 16) & 0xFF);
                asdu[pos++] = ev.value ? 0x01 : 0x00;
                if (config_.spontaneousWithTimestamp) {
                    PackCP56Time2a(asdu, pos);
                }

                // Send to all connected clients
                std::lock_guard<std::mutex> lock(clientsMtx_);
                for (auto& cl : clients_) {
                    try {
                        SendIFrame(cl->sock, cl->sNr, cl->rNr, asdu, pos);
                    } catch (...) {}
                }
                return;
            }
        }
    }
}

void Iec104Slave::SendAIActiveUpload(const AIChange& ev, uint32_t ioa,
                                       const SlaveAIMapping& ai,
                                       const SlaveDeviceConfig& dev) {
    if (!running_) return;
    double engVal = ev.value;

    uint8_t asdu[32];
    size_t pos = 0;
    asdu[pos++] = ai.type;
    asdu[pos++] = 0x01;
    asdu[pos++] = IecCOT::SPONTANEOUS;
    asdu[pos++] = 0x00;
    asdu[pos++] = static_cast<uint8_t>(dev.commonAddr & 0xFF);
    asdu[pos++] = static_cast<uint8_t>((dev.commonAddr >> 8) & 0xFF);

    if (ai.type == IecType::M_ME_NC_1) {
        PackFloatASDU(asdu, pos, engVal, ioa);
    } else {
        PackNormalizedASDU(asdu, pos, engVal, ioa, ai.scale);
    }

    std::lock_guard<std::mutex> lock(clientsMtx_);
    for (auto& cl : clients_) {
        try {
            SendIFrame(cl->sock, cl->sNr, cl->rNr, asdu, pos);
        } catch (...) {}
    }
}

// ==================== Timer thread (AI cycle upload) ====================

void Iec104Slave::TimerThread() {
    std::cout << "[Iec104Slave] AI cycle upload thread started, interval="
             << config_.aiCycleS << "s" << std::endl;

    int cycleMs = config_.aiCycleS > 0 ? config_.aiCycleS * 1000 : 60000;
    while (running_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(std::min(cycleMs, 5000)));

        if (!running_) break;

        // Check if need to send cycle AI
        // Simplified: traverse all AI points for all clients on each tick
        {
            uint8_t asdu[32];
            std::lock_guard<std::mutex> lock(clientsMtx_);

            for (auto& cl : clients_) {
                for (auto& dev : config_.devices) {
                    for (auto& [ioa, ai] : dev.aiMap) {
                        AiPoint pt;
                        if (!RemoteDataMgr::Instance().GetAi(ai.ch, ai.dev, ai.point, pt))
                            continue;

                        size_t pos = 0;
                        asdu[pos++] = ai.type;
                        asdu[pos++] = 0x01;
                        asdu[pos++] = IecCOT::PERIODIC;
                        asdu[pos++] = 0x00;
                        asdu[pos++] = static_cast<uint8_t>(dev.commonAddr & 0xFF);
                        asdu[pos++] = static_cast<uint8_t>((dev.commonAddr >> 8) & 0xFF);

                        double engVal = pt.value;
                        if (ai.type == IecType::M_ME_NC_1) {
                            PackFloatASDU(asdu, pos, engVal, ioa);
                        } else {
                            PackNormalizedASDU(asdu, pos, engVal, ioa, ai.scale);
                        }

                        try {
                            SendIFrame(cl->sock, cl->sNr, cl->rNr, asdu, pos);
                        } catch (...) {}
                    }
                }
            }
        }
    }

    std::cout << "[Iec104Slave] AI cycle upload thread exited" << std::endl;
}

// ==================== Iec104SlaveModule ====================

struct Iec104SlaveModule::Impl {
    Iec104Slave slave;
    std::string cfgPath;
    bool loaded = false;
    bool running = false;
};

Iec104SlaveModule::Iec104SlaveModule() : impl_(std::make_unique<Impl>()) {}
Iec104SlaveModule::~Iec104SlaveModule() { Stop(); }

bool Iec104SlaveModule::LoadConfig(const std::string& cfgPath) {
    impl_->cfgPath = cfgPath;
    impl_->loaded = impl_->slave.LoadConfig(cfgPath);
    return impl_->loaded;
}

bool Iec104SlaveModule::ValidateConfig(const std::string& cfgPath,
                                         std::vector<std::string>& errors) {
    Iec104Slave slave;
    if (!slave.LoadConfig(cfgPath)) {
        errors.push_back("Cannot load: " + cfgPath);
        return false;
    }
    return errors.empty();
}

bool Iec104SlaveModule::Start() {
    if (impl_->running) return true;
    impl_->running = impl_->slave.Start();
    return impl_->running;
}

void Iec104SlaveModule::Stop() {
    if (!impl_->running) return;
    impl_->slave.Stop();
    impl_->running = false;
}

bool Iec104SlaveModule::IsRunning() const { return impl_->running; }

REGISTER_MODULE("iec104_slave", Iec104SlaveModule)
