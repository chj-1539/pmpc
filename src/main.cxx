//=============================================================================
// main.cxx — 四遥数据管理系统主入口
//
// 功能：
//   1. 加载 point_cfg.ini → 初始化点表
//   2. 加载 app_modules.ini → 启动子模块
//   3. 双机冗余联动：Standby→停采集, Master→启采集
//   4. 主循环 + Ctrl+C 优雅退出
//=============================================================================

#include "pmpc.h"
#include "module_manager.h"
#include "redundancy.h"
#include "socket.h"
#include <iostream>
#include <csignal>
#include <cstdlib>
#include <atomic>

std::atomic<bool> g_running{true};

#ifdef _WIN32
#include <windows.h>
#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif
#endif

static void SetupConsole()
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8); SetConsoleCP(CP_UTF8);
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h != INVALID_HANDLE_VALUE) { DWORD m=0; if (GetConsoleMode(h,&m)) { m|=ENABLE_VIRTUAL_TERMINAL_PROCESSING; SetConsoleMode(h,m); } }
#else
    std::setlocale(LC_ALL, "en_US.UTF-8");
#endif
}

extern "C" void SignalHandler(int sig)
{
    (void)sig;
    // 信号处理器中仅设置退出标志，不做 I/O（非 async-signal-safe）
    g_running.store(false);
}

static void PrintBanner()
{
    std::cout << "\n========================================\n"
              << "  PMPC 四遥数据管理系统 v2.0\n"
              << "  动态模块 + 双机冗余\n"
              << "========================================\n"
              << "  Ctrl+C 退出 | 修改.ini自动热重载\n"
              << "========================================\n\n";
}

int main(int argc, char* argv[])
{
    SetupConsole();
    std::signal(SIGINT, SignalHandler); std::signal(SIGTERM, SignalHandler);
#ifdef SIGBREAK
    std::signal(SIGBREAK, SignalHandler);
#endif
    PrintBanner();

    // 点表配置
    std::string cfgPath = "point_cfg.ini";
    if (argc >= 2) cfgPath = argv[1];
    {
        auto& mgr = RemoteDataMgr::Instance();
        ConfigErrorReporter errRep;
        std::cout << "加载点表: " << cfgPath << std::endl;
        if (!mgr.LoadConfig(cfgPath, &errRep)) { std::cerr << "点表加载失败" << std::endl; return 1; }
        if (errRep.HasErrors()) std::cout << "点表 " << errRep.ErrorCount() << " 条警告" << std::endl;
    }
    std::cout << "点表加载完成\n" << std::endl;

    // 禁用 DataMgr 内置 DO 脉冲（默认 300ms），由 modbus 模块的 pulse_ms 配置接管
    // 因 modbus 采集周期（通常是 500ms~3000ms）长于默认脉冲宽度，
    // DataMgr 的自动复位会抢在 WriteDOChanges 之前把 masterVal 置回 false，
    // 导致写回报文永远无法发出。
    RemoteDataMgr::Instance().SetDoPulseMs(365ULL * 24 * 3600 * 1000);

#ifdef _WIN32
    wsa_guard wsa;
#endif

    // ── 模块管理器 ──
    ModuleManager modules;
    g_moduleManager = &modules;
    if (modules.LoadConfig("app_modules.ini"))
        modules.StartAll();
    else
        std::cerr << "app_modules.ini 加载失败" << std::endl;

    // 双机冗余联动已在 ModuleManager::StartAll() 中通过 SetCollectControl 注册
    // 采集模块的启停由冗余模块的角色切换自动管理

    // 主循环
    std::cout << "\n主循环开始...\n" << std::endl;
    auto& mgr = RemoteDataMgr::Instance();
    while (g_running) {
        // L17 修复：外围 try/catch 防止 CheckAllPointChange 内部 lambda 抛出
        // 未捕获异常时直接杀掉进程。任何异常都记 stderr 后继续。
        try {
            mgr.CheckAllPointChange();
        } catch (const std::exception& e) {
            std::cerr << "[main] CheckAllPointChange 异常: " << e.what()
                      << "（已忽略，主循环继续）" << std::endl;
        } catch (...) {
            std::cerr << "[main] CheckAllPointChange 未知异常（已忽略，主循环继续）" << std::endl;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    std::cout << "\n正在停止所有模块..." << std::endl;
    modules.StopAll();
    std::cout << "主程序退出。" << std::endl;
    return 0;
}
