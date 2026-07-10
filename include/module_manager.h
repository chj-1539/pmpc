#ifndef MODULE_MANAGER_H
#define MODULE_MANAGER_H

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <atomic>
#include <thread>
#include <chrono>
#include <functional>
#include <cstdint>

class RedundancyManager;

struct ModuleEntry {
    std::string name;
    std::string cfgFile;
    bool enable = true;
    bool autoReload = true;
    bool builtIn = false;
};

class AppModule {
public:
    virtual ~AppModule() = default;
    virtual const char* Name() const = 0;
    virtual bool LoadConfig(const std::string& cfgPath) = 0;
    virtual bool ValidateConfig(const std::string& cfgPath, std::vector<std::string>& errors) = 0;
    virtual bool Start() = 0;
    virtual void Stop() = 0;
    virtual bool IsRunning() const = 0;
};

class ModbusTcpMasterModule : public AppModule {
public:
    ModbusTcpMasterModule();
    ~ModbusTcpMasterModule() override;
    const char* Name() const override { return "modbus_tcp_master"; }
    bool LoadConfig(const std::string& cfgPath) override;
    bool ValidateConfig(const std::string& cfgPath, std::vector<std::string>& errors) override;
    bool Start() override;
    void Stop() override;
    bool IsRunning() const override;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class PempServerModule : public AppModule {
public:
    PempServerModule();
    ~PempServerModule() override;
    const char* Name() const override { return "pemp_server"; }
    bool LoadConfig(const std::string& cfgPath) override;
    bool ValidateConfig(const std::string& cfgPath, std::vector<std::string>& errors) override;
    bool Start() override;
    void Stop() override;
    bool IsRunning() const override;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class RedundancyModule : public AppModule {
public:
    RedundancyModule();
    ~RedundancyModule() override;
    const char* Name() const override { return "redundancy"; }
    bool LoadConfig(const std::string& cfgPath) override;
    bool ValidateConfig(const std::string& cfgPath, std::vector<std::string>& errors) override;
    bool Start() override;
    void Stop() override;
    bool IsRunning() const override;
    RedundancyManager& GetManager();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class ModuleManager {
public:
    ModuleManager();
    ~ModuleManager();
    bool LoadConfig(const std::string& mainCfgPath);
    bool StartAll();
    void StopAll();
    bool ReloadModule(const std::string& name);
    std::vector<std::pair<std::string, bool>> GetStatus() const;
    bool StartModule(const std::string& name);
    bool StopModule(const std::string& name);
    /// 按名称获取模块指针（供调试控制台等模块使用）
    AppModule* GetModule(const std::string& name);
private:
    void WatchLoop();
    int FindModule(const std::string& name) const;
    std::vector<ModuleEntry> entries_;
    std::vector<std::unique_ptr<AppModule>> modules_;
    std::map<std::string, int64_t> fileTimes_;
    std::atomic<bool> running_{false};
    std::thread watchThr_;
    int watchIntervalSec_ = 2;
};

namespace FileWatch {
    int64_t GetTimestamp(const std::string& path);
    bool HasChanged(int64_t oldStamp, int64_t newStamp);
}

/// 全局 ModuleManager 指针（main 中设置，供调试控制台等模块使用）
extern ModuleManager* g_moduleManager;

#endif
