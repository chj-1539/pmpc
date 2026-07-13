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

    // 测试挂钩：供 tests/test_debug_console_lifecycle.cxx 访问私有
    // CreateAutoTask/StopAutoTask/autoTasks_，验证 M8 死锁修复。
    // 生产代码不使用。见 CLAUDE.md「已知陷阱 / 修复历史」M8 条目。
    friend class DebugConsoleTestAccess;

    // ── M8 死锁修复的核心操作：抽出为静态模板供直接单元测试 ──
    // 语义：在 vec 中查找 id 匹配的元素，将其标记 stopFlag 并从 vec 中
    // 移出（**在锁内**），随后返回给调用方在**锁外**执行 join()。
    //
    // 关键：join() 必须锁外调用；否则工作线程等锁、Stop 线程等 join → 死锁。
    // Element 需具备 ->id / ->stopFlag / ->thr 三个成员（AutoTask 满足）。
    template <typename PtrT>
    static PtrT DetachTaskLocked(std::mutex& mtx,
                                 std::vector<PtrT>& vec,
                                 int id) {
        std::lock_guard<std::mutex> lock(mtx);
        for (auto it = vec.begin(); it != vec.end(); ++it) {
            if ((*it)->id == id) {
                (*it)->stopFlag = true;
                PtrT victim = std::move(*it);
                vec.erase(it);
                return victim;
            }
        }
        return PtrT{};
    }

    // 同上，一次性把 vec 中所有元素标记 stopFlag 并 move 到返回值容器。
    template <typename PtrT>
    static std::vector<PtrT> DetachAllTasksLocked(std::mutex& mtx,
                                                  std::vector<PtrT>& vec) {
        std::lock_guard<std::mutex> lock(mtx);
        for (auto& e : vec) e->stopFlag = true;
        std::vector<PtrT> stopped = std::move(vec);
        vec.clear();
        return stopped;
    }

    // 自动变位任务结构 —— 原为 private，为了让 test_debug_console_lifecycle.cxx
    // 能直接实例化 std::vector<unique_ptr<AutoTask>> 驱动死锁模拟，上移到
    // public。运行时无其他影响。
    struct AutoTask {
        int id;
        enum Type { DI_TOGGLE = 0, AI_SWEEP } type;
        uint16_t ch, dev, pt;
        int intervalMs;
        std::atomic<bool> stopFlag{false};
        std::thread thr;
    };

private:
    struct BindConfig {
        std::string allowedIP;
        int port;
    };
    // ── TCP 服务 ──
    void AcceptLoop(const BindConfig& bind, socket& listenSock);
    // DC-3（第二轮）修复：ClientThread 接收 shared_ptr<atomic<bool>> done，
    // 退出前置位。AcceptLoop 的 cleanup 只 join+erase done=true 的线程；
    // 老代码用 joinable() 判活会把正在跑的线程 detach，Stop() 后 UAF。
    void ClientThread(socket clientSock, std::shared_ptr<std::atomic<bool>> done);

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
    // AutoTask 结构现在定义在 public 部分（供测试 —— 见 M8 修复说明）。
    int CreateAutoTask(AutoTask::Type type, uint16_t ch, uint16_t dev,
                       uint16_t pt, int intervalMs);
    bool StopAutoTask(int id);
    // DC-1（第二轮）修复：AutoToggleThread / AutoSweepThread 现在直接接收
    // 指向自己 AutoTask::stopFlag 的指针，不再遍历 autoTasks_ 找自己 ——
    // 避免"thread 启动 vs push_back 顺序 race"（老代码线程先跑、push 后做，
    // 首轮找不到自己就 return，任务空转）。stopFlag 用 atomic<bool>，
    // 无需锁。
    void AutoToggleThread(std::atomic<bool>* stopFlag,
                          uint16_t ch, uint16_t dev, uint16_t pt, int intervalMs);
    void AutoSweepThread(std::atomic<bool>* stopFlag,
                         uint16_t ch, uint16_t dev, uint16_t pt, int intervalMs);

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
    // DC-3（第二轮）: 与 clientThreads_ 一一对应的完成标志。ClientThread
    // 返回前置位 true；AcceptLoop 定期 cleanup 只 join+erase 已完成的。
    std::vector<std::shared_ptr<std::atomic<bool>>> clientDoneFlags_;
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
