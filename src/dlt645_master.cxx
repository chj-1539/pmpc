//=============================================================================
// dlt645_master.cxx - DLT 645 meter acquisition implementation
//
// 协议：DLT 645-1997 / 2007
//  Frame format: 68 A0..A5 68 C L DI DATA CS 16
//  Layers: global -> Channel -> Template -> Device
//=============================================================================

#include "dlt645_master.h"
#include "packet_logger.h"
#include "module_factory.h"
#include "ini_reader.h"
#include "str_util.h"
#include "comm_io.h"
#include <iostream>
#include <sstream>
#include <cstring>
#include <algorithm>
#include <cmath>

//  Frame constants
constexpr uint8_t FRAME_START = 0x68;
constexpr uint8_t FRAME_END   = 0x16;
constexpr size_t MAX_FRAME    = 64;
constexpr int    SERIAL_RECV_TIMEOUT = 200; // ms

// ==================== 辅助 ====================

int Dlt645Master::SafeStoi(const std::string& s, int def) {
    try { return std::stoi(s); } catch (...) { return def; }
}
double Dlt645Master::SafeStod(const std::string& s, double def) {
    try { return std::stod(s); } catch (...) { return def; }
}

// ==================== Dlt645Master ====================

Dlt645Master::Dlt645Master() {}
Dlt645Master::~Dlt645Master() { Stop(); }

// ==================== Checksum ====================

uint8_t Dlt645Master::CheckSum(const uint8_t* data, size_t len) {
    uint8_t cs = 0;
    for (size_t i = 0; i < len; i++) cs = static_cast<uint8_t>(cs + data[i]);
    return cs;
}

// ==================== 地址转换 ====================

uint64_t Dlt645Master::AddrFromString(const std::string& s) {
    // 12-digit BCD string to uint64
    //  "010001234567" -> 0x010001234567
    uint64_t addr = 0;
    for (char c : s) {
        if (c >= '0' && c <= '9') {
            addr = (addr << 4) | static_cast<uint64_t>(c - '0');
        }
    }
    return addr;
}

// ==================== BCD to double ====================

double Dlt645Master::BCDToDouble(const uint8_t* data, size_t len) {
    // DLT 645 data is BCD encoded, low byte first
    // eg 4 bytes: 12 34 56 78 -> 0x78563412 -> 123456.78 (divide by 100)
    // This function decodes BCD to uint64
    uint64_t val = 0;
    for (size_t i = 0; i < len; i++) {
        uint8_t bcd = data[len - 1 - i]; // 高位在前
        val = val * 100 + ((bcd >> 4) * 10 + (bcd & 0x0F));
    }
    return static_cast<double>(val);
}

// ==================== Build read frame ====================

bool Dlt645Master::BuildReadFrame(uint8_t* frame, size_t& frameLen,
                                    uint64_t addr, uint32_t dataId,
                                    DltVersion ver) {
    // Frame: 68 A0 A1 A2 A3 A4 A5 68 C L DI0 DI1 [DI2 DI3] CS 16
    size_t pos = 0;
    frame[pos++] = FRAME_START;                     // Start
    // Address field (6 bytes, BCD reverse order, low byte first)
    // addr = 0x010001234567 -> send 67 45 23 01 00 01
    frame[pos++] = static_cast<uint8_t>((addr >> 40) & 0xFF); // byte5
    frame[pos++] = static_cast<uint8_t>((addr >> 32) & 0xFF); // byte4
    frame[pos++] = static_cast<uint8_t>((addr >> 24) & 0xFF); // byte3
    frame[pos++] = static_cast<uint8_t>((addr >> 16) & 0xFF); // byte2
    frame[pos++] = static_cast<uint8_t>((addr >> 8) & 0xFF);  // byte1
    frame[pos++] = static_cast<uint8_t>(addr & 0xFF);         // byte0

    frame[pos++] = FRAME_START;                     // Start
    frame[pos++] = 0x11;                            // Control: read
    size_t lenPos = pos++;                          // Placeholder for L

    // Data ID DI
    // 1997: 2 bytes (DI0, DI1) little-endian
    // 2007: 4 bytes (DI0, DI1, DI2, DI3) little-endian
    // dataId = 0xDDCCBBAA (2007) / 0xBBAA (1997)
    // Send order: DI0(AA) DI1(BB) [DI2(CC) DI3(DD)]
    frame[pos++] = static_cast<uint8_t>(dataId & 0xFF);         // DI0
    frame[pos++] = static_cast<uint8_t>((dataId >> 8) & 0xFF);  // DI1
    if (ver == DltVersion::V2007) {
        frame[pos++] = static_cast<uint8_t>((dataId >> 16) & 0xFF); // DI2
        frame[pos++] = static_cast<uint8_t>((dataId >> 24) & 0xFF); // DI3
    }

    size_t dataLen = (ver == DltVersion::V2007) ? 4 : 2; // DI length
    frame[lenPos] = static_cast<uint8_t>(dataLen & 0xFF); // L = DI length

    // Checksum (from address field to byte before CS)
    frame[pos] = CheckSum(frame + 1, pos - 1); // start from address
    pos++;

    frame[pos++] = FRAME_END;
    frameLen = pos;
    return true;
}

// ==================== 配置加载 ====================

bool Dlt645Master::LoadConfig(const std::string& path) {
    IniReader ini;
    if (!ini.Load(path)) {
        std::cerr << "[DLT645] Cannot open: " << path << std::endl;
        return false;
    }

    config_ = DltMasterConfig{};
    template1997_.clear();
    template2007_.clear();

    // Global settings
    for (auto& k : ini.Keys("global"))
        ParseGlobal(k, ini.Get("global", k, ""));

    auto sections = ini.Sections();
    for (const auto& sec : sections) {
        std::string lowSec = ToLower(sec);

        if (lowSec == "global") continue;

        //  Template_1997 / Template_2007
        if (lowSec == "template_1997") {
            bool is2007 = false;
            for (auto& k : ini.Keys(sec)) {
                auto v = ini.Get(sec, k, "");
                if (!StartsWith(ToLower(k), "ai_")) continue;
                DltAIMapping ai;
                if (ParseTemplateEntry(v, ai, is2007))
                    template1997_.push_back(ai);
            }
            continue;
        }
        if (lowSec == "template_2007") {
            bool is2007 = true;
            for (auto& k : ini.Keys(sec)) {
                auto v = ini.Get(sec, k, "");
                if (!StartsWith(ToLower(k), "ai_")) continue;
                DltAIMapping ai;
                if (ParseTemplateEntry(v, ai, is2007))
                    template2007_.push_back(ai);
            }
            continue;
        }

        //  Channel_N
        if (StartsWith(lowSec, "channel_")) {
            DltChannelConfig ch;
            for (auto& k : ini.Keys(sec))
                ParseChannel(k, ini.Get(sec, k, ""), ch);
            config_.channels.push_back(ch);
            continue;
        }

        //  Device_N
        if (StartsWith(lowSec, "device_")) {
            if (config_.channels.empty()) continue;
            DltDeviceConfig dev;
            int chIdx = 0;
            for (auto& k : ini.Keys(sec)) {
                std::string kl = ToLower(k);
                std::string v = ini.Get(sec, k, "");
                if (kl == "channel") chIdx = SafeStoi(v, 1) - 1;
                else if (kl == "address") {
                    dev.addressStr = v;
                    dev.address = AddrFromString(v);
                }
                else if (kl == "template") dev.templateName = v;
                else if (kl == "desc") dev.name = v;
                else if (kl == "baud") dev.baud = SafeStoi(v, 2400);
                else ParseDeviceEntry(k, v, dev);
            }

            // 合并模板
            if (dev.templateName == "1997")
                dev.aiList = template1997_;
            else if (dev.templateName == "2007")
                dev.aiList = template2007_;

            if (chIdx >= 0 && chIdx < static_cast<int>(config_.channels.size()))
                config_.channels[chIdx].devices.push_back(dev);
            continue;
        }
    }

    if (config_.channels.empty()) {
        std::cerr << "[DLT645] No channels" << std::endl;
        return false;
    }

    int devCount = 0;
    for (auto& ch : config_.channels) devCount += static_cast<int>(ch.devices.size());
    std::cout << "[DLT645] Loaded " << config_.channels.size() << " channels "
              << devCount << " devices tpl1997=" << template1997_.size()
              << " tpl2007=" << template2007_.size() << std::endl;
    return true;
}

bool Dlt645Master::ParseGlobal(const std::string& key, const std::string& val) {
    auto kl = ToLower(key);
    if (kl == "timeout_ms") config_.timeoutMs = SafeStoi(val, 1000);
    else if (kl == "retry_count") config_.retryCount = SafeStoi(val, 2);
    else if (kl == "retry_sleep_ms") config_.retrySleepMs = SafeStoi(val, 5000);
    else if (kl == "verbose") config_.verbose = SafeStoi(val, 0);
    else if (kl == "baud") config_.defaultBaud = SafeStoi(val, 2400);
    return true;
}

bool Dlt645Master::ParseChannel(const std::string& key, const std::string& val,
                                  DltChannelConfig& ch) {
    auto kl = ToLower(key);
    if (kl == "port_name") ch.portName = val;
    else if (kl == "baud") ch.baud = SafeStoi(val, 2400);
    else if (kl == "parity") ch.parity = ToLower(val);
    else if (kl == "data_bits") ch.dataBits = SafeStoi(val, 8);
    else if (kl == "stop_bits") ch.stopBits = SafeStoi(val, 1);
    else if (kl == "scan_ms") ch.scanMs = SafeStoi(val, 5000);
    else if (kl == "timeout_ms") ch.timeoutMs = SafeStoi(val, 1000);
    else if (kl == "retry_count") ch.retryCount = SafeStoi(val, 2);
    else if (kl == "retry_sleep_ms") ch.retrySleepMs = SafeStoi(val, 5000);
    else if (kl == "verbose") ch.verbose = SafeStoi(val, 0);
    if (ch.timeoutMs <= 0) ch.timeoutMs = config_.timeoutMs;
    if (ch.retryCount <= 0) ch.retryCount = config_.retryCount;
    if (ch.retrySleepMs <= 0) ch.retrySleepMs = config_.retrySleepMs;
    return true;
}

bool Dlt645Master::ParseTemplateEntry(const std::string& params,
                                        DltAIMapping& ai, bool is2007) {
    auto kv = ParseKeyValues(params);
    if (kv.empty()) return false;

    for (auto& [k, v] : kv) {
        auto lk = ToLower(k);
        if (lk == "data_id") {
            // HEX string to uint32
            uint32_t id = 0;
            std::stringstream ss;
            ss << std::hex << v;
            ss >> id;
            ai.dataId = id;
        }
        else if (lk == "point") ai.point = static_cast<uint16_t>(SafeStoi(v, 1));
        else if (lk == "scale") ai.scale = SafeStod(v, 1.0);
        else if (lk == "offset") ai.offset = SafeStod(v, 0.0);
    }

    // Infer dataLen from data ID
    if (is2007) {
        uint8_t di1 = static_cast<uint8_t>((ai.dataId >> 8) & 0xFF);
        if (di1 == 0x01) ai.dataLen = 2;   // Voltage
        else if (di1 == 0x02) ai.dataLen = 3; // Current
        else if (di1 == 0x05) ai.dataLen = 2; // Power factor
        else if (di1 == 0x06) ai.dataLen = 2; // Frequency
        else if (di1 == 0x03) ai.dataLen = 3; // Power
        else ai.dataLen = 4; // Energy
    } else {
        uint8_t di1 = static_cast<uint8_t>((ai.dataId >> 8) & 0xFF);
        if (di1 == 0x01) ai.dataLen = 2;
        else if (di1 == 0x02) ai.dataLen = 3;
        else if (di1 == 0x05) ai.dataLen = 2;
        else if (di1 == 0x06) ai.dataLen = 2;
        else if (di1 == 0x03) ai.dataLen = 3;
        else ai.dataLen = 4;
    }

    return ai.dataId > 0 && ai.point > 0;
}

bool Dlt645Master::ParseDeviceEntry(const std::string& /*key*/, const std::string& val,
                                      DltDeviceConfig& dev) {
    // 设备的自定义条目（追加到模板后）
    auto kv = ParseKeyValues(val);
    if (kv.empty()) return false;
    DltAIMapping ai;
    bool is2007 = (dev.templateName == "2007");
    if (ParseTemplateEntry(val, ai, is2007))
        dev.aiList.push_back(ai);
    return true;
}

// ==================== 启停 ====================

bool Dlt645Master::Start() {
    if (running_) return false;
    if (config_.channels.empty()) return false;

    running_ = true;
    threads_.reserve(config_.channels.size());
    for (size_t i = 0; i < config_.channels.size(); i++)
        threads_.emplace_back(&Dlt645Master::ChannelThread, this, static_cast<int>(i));

    std::cout << "[DLT645] Started " << config_.channels.size() << " channels" << std::endl;
    return true;
}

void Dlt645Master::Stop() {
    running_ = false;
    for (auto& t : threads_)
        if (t.joinable()) t.join();
    threads_.clear();
}

// ==================== Channel thread ====================

void Dlt645Master::ChannelThread(int chIdx) {
    if (chIdx < 0 || chIdx >= static_cast<int>(config_.channels.size())) return;
    DltChannelConfig& ch = config_.channels[chIdx];
    int scanMs = ch.scanMs > 0 ? ch.scanMs : 5000;

    std::cout << "[DLT645] Channel " << (chIdx+1) << " start: " << ch.portName
              << " scan=" << scanMs << "ms devices=" << ch.devices.size() << std::endl;

    while (running_) {
        CommIO io;
        bool opened = false;

        try {
            io.open(ch.portName, ch.baud, ch.parity, ch.dataBits, ch.stopBits, ch.timeoutMs);
            opened = true;
            std::cout << "[DLT645] " << ch.portName << " opened" << std::endl;
        }
        catch (const std::exception& e) {
            std::cerr << "[DLT645] " << ch.portName << " open fail: " << e.what() << std::endl;
            for (int i = 0; i < 50 && running_; i++)
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }

        // Acquisition loop
        while (running_ && opened) {
            try {
                for (auto& dev : ch.devices) {
                    if (!running_) break;

                    // Dynamic baud switching
                    if (io.currentBaud() != dev.baud && dev.baud > 0) { io.setBaud(dev.baud); }

                    DltVersion ver = (dev.templateName == "1997") ? DltVersion::V1997 : DltVersion::V2007;
                    int devIdx = static_cast<int>(&dev - &ch.devices[0]);
                    PollDevice(io, dev, ver, ch.timeoutMs, chIdx, devIdx, ch);
                }
            }
            catch (const std::exception& e) {
                std::cerr << "[DLT645] comm error: " << e.what() << std::endl;
                opened = false;
                break;
            }

            if (running_ && opened) {
                for (int i = 0; i < scanMs / 200 && running_; i++)
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
        }

        try { io.close(); } catch (...) {}
        std::cout << "[DLT645] " << ch.portName << " disconnected, retry..." << std::endl;
    }
    std::cout << "[DLT645] Channel " << (chIdx+1) << " thread exit" << std::endl;
}

// ==================== Poll device ====================

bool Dlt645Master::PollDevice(CommIO& io, const DltDeviceConfig& dev,
                                DltVersion ver, int timeoutMs,
                                int chIdx, int devIdx,
                                const DltChannelConfig& ch) {
    for (auto& ai : dev.aiList) {
        if (!running_) return false;

        uint8_t frame[MAX_FRAME];
        size_t frameLen = 0;
        if (!BuildReadFrame(frame, frameLen, dev.address, ai.dataId, ver))
            continue;

        // Send
        try {
            io.write(frame, frameLen);
        } catch (const std::exception&) {
            return false;
        }

        // Wait for response
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // Receive response
        uint8_t resp[MAX_FRAME];
        size_t pos = 0;
        auto start = std::chrono::steady_clock::now();
        bool gotStart = false;

        while (pos < MAX_FRAME) {
            uint8_t byte;
            try {
                size_t n = io.read(&byte, 1);
                if (n == 0) {
                    if (pos > 0) break;
                    if (std::chrono::steady_clock::now() - start >
                        std::chrono::milliseconds(timeoutMs)) break;
                    continue;
                }
            } catch (...) { break; }

            if (!gotStart && byte == FRAME_START) {
                gotStart = true;
                pos = 0;
            }
            if (!gotStart) continue;

            resp[pos++] = byte;
            if (pos >= 2 && resp[pos-1] == FRAME_END) break;
        }

        if (pos < 7) continue; // 至少 68 + addr(6) + 68 + C + L + CS + 16

        if (resp[0] != FRAME_START || resp[pos-1] != FRAME_END) continue;

        // Control code should be 0x91 (read response)
        uint8_t ctrl = resp[7];
        if (ctrl != 0x91) continue;

        uint8_t dataLen = resp[8];
        if (pos < static_cast<size_t>(9 + dataLen + 2)) continue;

        // Skip DI bytes, data starts after DI field
        // resp[7]=C, resp[8]=L, resp[9..]=DI+data
        int diLen = (ver == DltVersion::V2007) ? 4 : 2;
        const uint8_t* dataStart = resp + 9 + diLen;
        size_t dataBytes = dataLen - diLen;

        if (dataBytes < ai.dataLen) continue;

        // BCD to double
        double rawVal = BCDToDouble(dataStart, ai.dataLen);
        double engVal = rawVal * ai.scale + ai.offset;

        // Write to AI
        RemoteDataMgr::Instance().SetAi(
            static_cast<uint16_t>(chIdx + 1),
            static_cast<uint16_t>(devIdx + 1),
            ai.point, engVal);

        if (ch.verbose >= 1)
            std::cout << "[DLT645] " << dev.name << " AI pt=" << ai.point
                     << " val=" << engVal << std::endl;
    }
    return true;
}

// ==================== Dlt645MasterModule ====================

struct Dlt645MasterModule::Impl {
    Dlt645Master master;
    std::string cfgPath;
    bool loaded = false;
    bool running = false;
};

Dlt645MasterModule::Dlt645MasterModule() : impl_(std::make_unique<Impl>()) {}
Dlt645MasterModule::~Dlt645MasterModule() { Stop(); }

bool Dlt645MasterModule::LoadConfig(const std::string& cfgPath) {
    impl_->cfgPath = cfgPath;
    impl_->loaded = impl_->master.LoadConfig(cfgPath);
    return impl_->loaded;
}

bool Dlt645MasterModule::ValidateConfig(const std::string& cfgPath,
    std::vector<std::string>& errors) {
    Dlt645Master temp;
    if (!temp.LoadConfig(cfgPath)) {
        errors.push_back("Cannot load: " + cfgPath);
        return false;
    }
    return errors.empty();
}

bool Dlt645MasterModule::Start() {
    if (impl_->running) return true;
    impl_->running = impl_->master.Start();
    return impl_->running;
}

void Dlt645MasterModule::Stop() {
    if (!impl_->running) return;
    impl_->master.Stop();
    impl_->running = false;
}

bool Dlt645MasterModule::IsRunning() const { return impl_->running; }

REGISTER_MODULE("dlt645_master", Dlt645MasterModule)
