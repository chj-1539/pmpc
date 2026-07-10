#ifndef DEBUG_CONSOLE_H
#define DEBUG_CONSOLE_H

//=============================================================================
// debug_console.h — TCP 调试控制台模块
//
// 功能：通过 TCP 连接（端口 9090）提供交互式调试命令
//   - 读取/设置 DI/AI/DO/AO 点值
//   - 自动变位（DI 翻转 / AI 正弦波）
//   - 查询通道/设备列表
//   - 模块状态和启停控制
//   - 角色切换（主机/备机）
//   - 配置文件热重载
//=============================================================================

#include "socket.h"
#include "pmpc.h"
#include "module_manager.h"
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <atomic>
#include <thread>
#include <mutex>
#include <cstdint>
#include <functional>

// ==================== 调试控制台 ====================

class DebugConsole {
public:
    DebugConsole();
    ~DebugConsole();

    bool LoadConfig(const std::string& path);
    bool Start();
    void Stop();
    bool IsRunning() const { return running_; }

private:
    struct BindConfig {
        std::string allowedIP;
        int port;
    };
    // ── TCP 服务 ──
    void AcceptLoop(const BindConfig& bind, socket& listenSock);
    void ClientThread(socket clientSock);

    // ── 发送响应 ──
    void Send(socket& sock, const std::string& text);
    void SendLine(socket& sock, const std::string& line);
    void PrintBanner(socket& sock);

    // ── 命令处理 ──
    bool ProcessCommand(socket& sock, const std::string& line);

    // ── 各命令实现 ──
    void CmdHelp(socket& sock);
    void CmdGetDi(socket& sock, const std::vector<std::string>& args);
    void CmdGetAi(socket& sock, const std::vector<std::string>& args);
    void CmdSetDi(socket& sock, const std::vector<std::string>& args);
    void CmdSetAi(socket& sock, const std::vector<std::string>& args);
    void CmdSetDo(socket& sock, const std::vector<std::string>& args);
    void CmdSetAo(socket& sock, const std::vector<std::string>& args);
    void CmdAuto(socket& sock, const std::vector<std::string>& args);
    void CmdAutoStop(socket& sock, const std::vector<std::string>& args);
    void CmdRole(socket& sock, const std::vector<std::string>& args);
    void CmdStatus(socket& sock);
    void CmdChannels(socket& sock);
    void CmdDevices(socket& sock, const std::vector<std::string>& args);
    void CmdReload(socket& sock, const std::vector<std::string>& args);
    void CmdStartStop(socket& sock, const std::vector<std::string>& args);
    void CmdUpload(socket& sock, const std::vector<std::string>& args);
    void CmdLog(socket& sock, const std::vector<std::string>& args);

    // ── 自动变位任务 ──
    struct AutoTask {
        int id;
        enum Type { DI_TOGGLE = 0, AI_SWEEP } type;
        uint16_t ch, dev, pt;
        int intervalMs;
        std::atomic<bool> stopFlag{false};
        std::thread thr;
    };
    int CreateAutoTask(AutoTask::Type type, uint16_t ch, uint16_t dev,
                       uint16_t pt, int intervalMs);
    bool StopAutoTask(int id);
    void AutoToggleThread(int taskId, uint16_t ch, uint16_t dev,
                          uint16_t pt, int intervalMs);
    void AutoSweepThread(int taskId, uint16_t ch, uint16_t dev,
                         uint16_t pt, int intervalMs);

    // ── 辅助 ──
    static bool ParseUint16(const std::string& s, uint16_t& out);
    static bool ParseInt(const std::string& s, int& out);
    static bool ParseDouble(const std::string& s, double& out);
    static std::vector<std::string> SplitArgs(const std::string& line);

    int port_ = 9090;
    int verbose_ = 1;
    int cleanupCnt_ = 0;
    std::string password_;
    std::vector<BindConfig> binds_;
    std::atomic<bool> running_{false};
    std::vector<socket> listenSocks_;
    std::vector<std::thread> acceptThreads_;
    std::vector<std::thread> clientThreads_;
    std::mutex clientMtx_;

    // 自动变位任务管理
    std::mutex autoMtx_;
    std::vector<std::unique_ptr<AutoTask>> autoTasks_;
    int nextAutoId_ = 1;
};

// ==================== AppModule 包装 ====================

class DebugConsoleModule : public AppModule {
public:
    DebugConsoleModule();
    ~DebugConsoleModule() override;
    const char* Name() const override { return "debug_console"; }
    bool LoadConfig(const std::string& cfgPath) override;
    bool ValidateConfig(const std::string& cfgPath,
                        std::vector<std::string>& errors) override;
    bool Start() override;
    void Stop() override;
    bool IsRunning() const override;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

#endif // DEBUG_CONSOLE_H
