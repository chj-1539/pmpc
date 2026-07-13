//=============================================================================
// module_manager.cxx — Unified module lifecycle + hot-reload
//=============================================================================

#include "module_manager.h"
#include "module_factory.h"
#include "str_util.h"
#include "ini_reader.h"
#include "modbus_tcp_master.h"
#include "pemp_server.h"
#include "redundancy.h"
#include "pmpc.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <cstring>
#include <algorithm>

ModuleManager* g_moduleManager = nullptr;

#ifdef _WIN32
#include <sys/stat.h>
#endif

static int64_t fileTimeNs(const std::string& path) {
#ifdef _WIN32
    struct _stat st;
    if (_stat(path.c_str(), &st) != 0) return 0;
    return static_cast<int64_t>(st.st_mtime) * 1000000000LL;
#else
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return 0;
    return static_cast<int64_t>(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec;
#endif
}

// ==================== ModbusTcpMasterModule ====================

struct ModbusTcpMasterModule::Impl {
    ModbusTcpMaster master;
    std::string cfgPath;
    bool loaded = false;
    bool running = false;
};
ModbusTcpMasterModule::ModbusTcpMasterModule() : impl_(std::make_unique<Impl>()) {}
ModbusTcpMasterModule::~ModbusTcpMasterModule() { Stop(); }

bool ModbusTcpMasterModule::LoadConfig(const std::string& cfgPath) {
    impl_->cfgPath = cfgPath;
    impl_->loaded = impl_->master.LoadConfig(cfgPath);
    return impl_->loaded;
}

bool ModbusTcpMasterModule::ValidateConfig(const std::string& cfgPath, std::vector<std::string>& errors) {
    ModbusTcpMaster temp;
    if (!temp.LoadConfig(cfgPath)) { errors.push_back("Cannot load: " + cfgPath); return false; }
    const auto& cfg = temp.GetConfig();
    if (cfg.channels.empty()) { errors.push_back("No channels"); return false; }
    for (size_t i = 0; i < cfg.channels.size(); i++) {
        if (cfg.channels[i].ip.empty()) errors.push_back("Ch" + std::to_string(i+1) + " IP empty");
        if (cfg.channels[i].port == 0) errors.push_back("Ch" + std::to_string(i+1) + " port=0");
    }
    return errors.empty();
}

bool ModbusTcpMasterModule::Start() {
    if (impl_->running) return true;
    impl_->running = impl_->master.Start();
    return impl_->running;
}
void ModbusTcpMasterModule::Stop() {
    if (impl_->running) { impl_->master.Stop(); impl_->running = false; }
}
bool ModbusTcpMasterModule::IsRunning() const { return impl_->running; }
REGISTER_MODULE("modbus_tcp_master", ModbusTcpMasterModule)

// ==================== PempServerModule ====================

struct PempServerModule::Impl {
    PempServer* server = nullptr;
    std::string cfgPath;
    int port = 4096;
    int diUploadMs = 5000;
    int aiUploadMs = 5000;
    std::vector<PempBind> binds;
    bool rcPasswordParsed = false;
    std::string rcPassword;    ///< 遥控密码（M3 修复），空 = 不校验
    bool clockSyncEnable = false;   ///< PS-2（第二轮）
    bool clockSyncVerbose = false;  ///< PS-2（第二轮）
    bool running = false;
    ~Impl() { delete server; }
};
PempServerModule::PempServerModule() : impl_(std::make_unique<Impl>()) {}
PempServerModule::~PempServerModule() { Stop(); }

bool PempServerModule::LoadConfig(const std::string& cfgPath) {
    impl_->cfgPath = cfgPath;
    IniReader ini;
    if (!ini.Load(cfgPath)) {
        std::cerr << "[PempServer] 配置文件不存在: " << cfgPath
                  << "，使用默认参数 (port=" << impl_->port << ")" << std::endl;
        return true;  // 允许用默认值启动
    }
    impl_->port = ini.GetInt("global", "port", 4096);
    impl_->diUploadMs = ini.GetInt("global", "di_upload_ms", 5000);
    impl_->aiUploadMs = ini.GetInt("global", "ai_upload_ms", 5000);
    impl_->rcPassword = ini.Get("global", "rc_password", "");
    impl_->clockSyncEnable = ini.GetInt("global", "clock_sync_enable", 0) != 0;
    impl_->clockSyncVerbose = ini.GetInt("global", "clock_sync_verbose", 0) != 0;
    // 解析 [listen_N]
    impl_->binds.clear();
    for (auto& sec : ini.Sections()) {
        if (!StartsWith(ToLower(sec), "listen_")) continue;
        PempBind b;
        b.allowedIP = ini.Get(sec, "ip", "");
        b.port = static_cast<uint16_t>(ini.GetInt(sec, "port", 0));
        if (b.port > 0) impl_->binds.push_back(b);
    }
    return true;
}

bool PempServerModule::ValidateConfig(const std::string& cfgPath, std::vector<std::string>& errors) {
    IniReader ini;
    if (!ini.Load(cfgPath)) {
        std::cerr << "[PempServer] ValidateConfig: 配置文件不存在: " << cfgPath << std::endl;
        return true;
    }
    int p = ini.GetInt("global", "port", 4096);
    if (p < 1 || p > 65535) errors.push_back("Bad port: " + std::to_string(p));
    return errors.empty();
}

bool PempServerModule::Start() {
    if (impl_->running) return true;
    impl_->server = new PempServer(static_cast<uint16_t>(impl_->port),
                                    impl_->diUploadMs, impl_->aiUploadMs);
    if (!impl_->binds.empty())
        impl_->server->setBinds(impl_->binds);
    impl_->server->setRemoteCtrlPassword(impl_->rcPassword);
    impl_->server->setClockSyncEnable(impl_->clockSyncEnable);
    impl_->server->setClockSyncVerbose(impl_->clockSyncVerbose);
    impl_->running = impl_->server->start();
    return impl_->running;
}
void PempServerModule::Stop() {
    if (!impl_->running) return;
    if (impl_->server) { impl_->server->stop(); delete impl_->server; impl_->server = nullptr; }
    impl_->running = false;
}
bool PempServerModule::IsRunning() const { return impl_->running; }
REGISTER_MODULE("pemp_server", PempServerModule)

// ==================== RedundancyModule ====================

struct RedundancyModule::Impl {
    RedundancyManager mgr;
    std::string cfgPath;
    bool loaded = false;
    bool running = false;
};
RedundancyModule::RedundancyModule() : impl_(std::make_unique<Impl>()) {}
RedundancyModule::~RedundancyModule() { Stop(); }

bool RedundancyModule::LoadConfig(const std::string& cfgPath) {
    impl_->cfgPath = cfgPath;
    impl_->loaded = impl_->mgr.LoadConfig(cfgPath);
    return impl_->loaded;
}

bool RedundancyModule::ValidateConfig(const std::string& cfgPath, std::vector<std::string>& errors) {
    IniReader ini;
    if (!ini.Load(cfgPath)) { errors.push_back("No file: " + cfgPath); return false; }
    if (ini.Get("global", "peer_ip", "").empty()) errors.push_back("Missing peer_ip");
    return errors.empty();
}

bool RedundancyModule::Start() {
    if (impl_->running) return true;
    impl_->running = impl_->mgr.Start();
    return impl_->running;
}
void RedundancyModule::Stop() {
    if (impl_->running) { impl_->mgr.Stop(); impl_->running = false; }
}
bool RedundancyModule::IsRunning() const { return impl_->running; }
RedundancyManager& RedundancyModule::GetManager() { return impl_->mgr; }
REGISTER_MODULE("redundancy", RedundancyModule)

// ==================== ModuleManager ====================

ModuleManager::ModuleManager() {}
ModuleManager::~ModuleManager() { StopAll(); }

bool ModuleManager::LoadConfig(const std::string& mainCfgPath) {
    IniReader ini;
    if (!ini.Load(mainCfgPath)) {
        std::cerr << "[ModuleManager] Cannot open: " << mainCfgPath << std::endl;
        return false;
    }
    entries_.clear();
    modules_.clear();
    watchIntervalSec_ = ini.GetInt("global", "watch_interval", 2);
    for (const auto& section : ini.Sections()) {
        if (ToLower(section) == "global") continue;
        ModuleEntry me;
        me.name = section;
        me.enable = ini.GetBool(section, "enable", true);
        me.cfgFile = ini.Get(section, "cfg_file", "");
        me.autoReload = ini.GetBool(section, "auto_reload", true);
        me.builtIn = ini.GetBool(section, "built_in", false);
        entries_.push_back(me);
    }
    for (auto& entry : entries_) {
        if (!entry.enable) continue;
        auto mod = ModuleFactory::Create(entry.name);
        if (!mod) {
            std::cerr << "[ModuleManager] Unknown module: " << entry.name << std::endl;
            continue;
        }
        if (!entry.cfgFile.empty()) {
            if (!mod->LoadConfig(entry.cfgFile)) {
                std::cerr << "[ModuleManager] " << entry.name << " config load failed" << std::endl;
                continue;
            }
        }
        modules_.push_back(std::move(mod));
        if (entry.autoReload && !entry.cfgFile.empty()) {
            fileTimes_[entry.cfgFile] = FileWatch::GetTimestamp(entry.cfgFile);
            std::cout << "[ModuleManager] " << entry.name << " watch " << entry.cfgFile << std::endl;
        }
    }
    std::cout << "[ModuleManager] Loaded: " << modules_.size() << "/" << entries_.size()
              << " modules" << std::endl;
    return true;
}

bool ModuleManager::StartAll() {
    for (auto& mod : modules_) {
        if (!mod->Start())
            std::cerr << "[ModuleManager] " << mod->Name() << " start failed" << std::endl;
    }
    for (auto& mod : modules_) {
        if (std::string(mod->Name()) == "redundancy" && mod->IsRunning()) {
            auto* rd = dynamic_cast<RedundancyModule*>(mod.get());
            if (rd) {
                rd->GetManager().SetCollectControl([this](bool start) {
                    // 主备切换时联动所有采集模块
                    const char* collectors[] = {
                        "modbus_tcp_master", "modbus_rtu_master",
                        "iec104_master", "iec103_master",
                        "iec101_master", "dlt645_master",
                        "cdt_master"
                    };
                    if (start) {
                        std::cout << "[Redundancy] -> Start collectors (Master)" << std::endl;
                        for (auto name : collectors) StartModule(name);
                    } else {
                        std::cout << "[Redundancy] -> Stop collectors (Standby)" << std::endl;
                        for (auto name : collectors) StopModule(name);
                    }
                });
                std::cout << "[ModuleManager] Redundancy linked" << std::endl;
            }
            break;
        }
    }
    bool hasWatch = false;
    for (const auto& e : entries_)
        if (e.autoReload && !e.cfgFile.empty()) { hasWatch = true; break; }
    if (hasWatch && !running_) {
        running_ = true;
        watchThr_ = std::thread(&ModuleManager::WatchLoop, this);
        std::cout << "[ModuleManager] File watch started (" << watchIntervalSec_ << "s)" << std::endl;
    }
    return true;
}

void ModuleManager::StopAll() {
    running_ = false;
    if (watchThr_.joinable()) watchThr_.join();
    for (auto it = modules_.rbegin(); it != modules_.rend(); ++it) (*it)->Stop();
}

int ModuleManager::FindModule(const std::string& name) const {
    for (size_t i = 0; i < modules_.size(); i++)
        if (modules_[i]->Name() == name) return static_cast<int>(i);
    return -1;
}

bool ModuleManager::ReloadModule(const std::string& name) {
    int idx = FindModule(name);
    if (idx < 0) return false;
    auto& mod = modules_[idx];

    // entries_ 包含已禁用模块，索引与 modules_ 不对应，按名查找
    int entryIdx = -1;
    for (size_t i = 0; i < entries_.size(); i++)
        if (entries_[i].name == name) { entryIdx = static_cast<int>(i); break; }
    if (entryIdx < 0) return false;
    auto* entry = &entries_[entryIdx];

    std::cout << "[ModuleManager] Hot-reload: " << name << " ..." << std::endl;
    if (!entry->cfgFile.empty()) {
        std::vector<std::string> errs;
        if (!mod->ValidateConfig(entry->cfgFile, errs)) {
            std::cerr << "[ModuleManager] Hot-reload " << name << " FAILED (validation):" << std::endl;
            for (auto& e : errs) std::cerr << "  [ERR] " << e << std::endl;
            std::cerr << "[ModuleManager] Keeping old config" << std::endl;
            return false;
        }
    }
    bool wasRunning = mod->IsRunning();
    mod->Stop();
    if (!entry->cfgFile.empty())
        if (!mod->LoadConfig(entry->cfgFile)) {
            std::cerr << "[ModuleManager] Hot-reload " << name << " config load failed" << std::endl;
            return false;
        }
    if (wasRunning)
        if (!mod->Start()) {
            std::cerr << "[ModuleManager] Hot-reload " << name << " start failed" << std::endl;
            return false;
        }
    std::cout << "[ModuleManager] Hot-reload OK: " << name << std::endl;
    return true;
}

bool ModuleManager::StartModule(const std::string& name) {
    std::lock_guard<std::mutex> lock(mgrMtx_);
    int idx = FindModule(name);
    if (idx < 0) { std::cerr << "[ModuleManager] Unknown: " << name << std::endl; return false; }
    if (modules_[idx]->IsRunning()) return true;
    bool ok = modules_[idx]->Start();
    std::cout << "[ModuleManager] " << (ok ? "Started" : "Start FAIL") << ": " << name << std::endl;
    return ok;
}

bool ModuleManager::StopModule(const std::string& name) {
    std::lock_guard<std::mutex> lock(mgrMtx_);
    int idx = FindModule(name);
    if (idx < 0) { std::cerr << "[ModuleManager] Unknown: " << name << std::endl; return false; }
    if (!modules_[idx]->IsRunning()) return true;
    modules_[idx]->Stop();
    std::cout << "[ModuleManager] Stopped: " << name << std::endl;
    return true;
}

std::vector<std::pair<std::string, bool>> ModuleManager::GetStatus() const {
    std::vector<std::pair<std::string, bool>> st;
    for (auto& m : modules_) st.push_back({m->Name(), m->IsRunning()});
    return st;
}

void ModuleManager::WatchLoop() {
    while (running_) {
        for (const auto& e : entries_) {
            if (!e.enable || !e.autoReload || e.cfgFile.empty()) continue;
            auto nt = FileWatch::GetTimestamp(e.cfgFile);
            auto& ot = fileTimes_[e.cfgFile];
            if (FileWatch::HasChanged(ot, nt)) {
                ot = nt;
                std::cout << "\n[ModuleManager] File changed: " << e.cfgFile << std::endl;
                ReloadModule(e.name);
            }
        }
        for (int i = 0; i < watchIntervalSec_ * 10 && running_; i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

AppModule* ModuleManager::GetModule(const std::string& name)
{
    int idx = FindModule(name);
    if (idx < 0) return nullptr;
    return modules_[idx].get();
}

// ==================== FileWatch ====================

int64_t FileWatch::GetTimestamp(const std::string& path) { return fileTimeNs(path); }

bool FileWatch::HasChanged(int64_t oldStamp, int64_t newStamp) {
    if (oldStamp == 0) return newStamp != 0;
    if (newStamp == 0) return false;
    int64_t d = newStamp - oldStamp;
    if (d < 0) d = -d;
    return d > 500000000;
}
