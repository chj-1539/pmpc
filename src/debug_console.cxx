//=============================================================================
// debug_console.cxx — TCP 调试控制台实现
//
// 协议：纯文本行协议，客户端用 telnet/nc 连接
// 连接后自动打印使用帮助，输入命令后回车执行
//=============================================================================

#include "debug_console.h"
#include "module_factory.h"
#include "redundancy.h"
#include "packet_logger.h"
#include "ini_reader.h"
#include "str_util.h"
#include "telnet_iac.h"
#include <iostream>
#include <sstream>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <set>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ─── 常量 ───────────────────────────────────────────────────────────────────
constexpr size_t SEND_BUF_SIZE = 4096;
constexpr int    RECV_TIMEOUT_MS = 500;

// ─── 过滤 telnet IAC 协商字节 ──────────────────────────────────────────
// 实现已迁移到 include/telnet_iac.h（pmpc::filter_telnet_iac），
// 便于直接单元测试。见 tests/test_debug_console_telnet.cxx (bug #3 回归)。
using pmpc::filter_telnet_iac;

// ─── UTF-8 → 本地编码（解决 Windows telnet 中文乱码） ─────────────────────
#ifdef _WIN32
static std::string utf8_to_local(const std::string& utf8)
{
    if (utf8.empty()) return utf8;
    // UTF-8 → UTF-16
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (wlen <= 0) return utf8;
    std::wstring wbuf(static_cast<size_t>(wlen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &wbuf[0], wlen);
    // UTF-16 → 本地 ANSI（中文 Windows = GBK/CP936）
    int clen = WideCharToMultiByte(CP_ACP, 0, wbuf.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (clen <= 0) return utf8;
    std::string cbuf(static_cast<size_t>(clen), '\0');
    WideCharToMultiByte(CP_ACP, 0, wbuf.c_str(), -1, &cbuf[0], clen, nullptr, nullptr);
    // 去掉末尾 null 终止符
    if (clen > 0) cbuf.resize(static_cast<size_t>(clen) - 1);
    return cbuf;
}
#else
static std::string utf8_to_local(const std::string& s) { return s; }
#endif

// ==================== DebugConsole ====================

DebugConsole::DebugConsole() {}
DebugConsole::~DebugConsole() { Stop(); }

// ==================== 配置加载 ====================

bool DebugConsole::LoadConfig(const std::string& path)
{
    IniReader ini;
    if (!ini.Load(path)) {
        // 允许无配置文件，使用默认值
        std::cout << "[DebugConsole] 无配置文件，使用默认端口 " << port_ << std::endl;
        return true;
    }

    port_ = ini.GetInt("global", "port", 9090);
    verbose_ = ini.GetInt("global", "verbose", 1);
    password_ = ini.Get("global", "password", "");

    // 解析 [listen_N]
    for (auto& sec : ini.Sections()) {
        if (!StartsWith(ToLower(sec), "listen_")) continue;
        BindConfig bc;
        bc.allowedIP = ini.Get(sec, "ip", "");
        bc.port = ini.GetInt(sec, "port", 0);
        if (bc.port > 0) binds_.push_back(bc);
    }

    if (!password_.empty())
        std::cout << "[DebugConsole] 密码认证已启用" << std::endl;

    std::cout << "[DebugConsole] 配置加载: port=" << port_
              << " verbose=" << verbose_ << std::endl;
    return true;
}

// ==================== 启停 ====================

bool DebugConsole::Start()
{
    if (running_) return true;
    running_ = true;

    if (binds_.empty())
        binds_.push_back({"", port_});

    for (auto& bind : binds_) {
        try {
            socket sock;
            sock.bind(socket_addr("0.0.0.0", static_cast<uint16_t>(bind.port)));
            sock.listen(10);
            listenSocks_.push_back(std::move(sock));
        } catch (const socket_error& e) {
            std::cerr << "[DebugConsole] 绑定端口 " << bind.port
                      << " 失败: " << e.what() << std::endl;
            continue;
        }
    }
    if (listenSocks_.empty()) { running_ = false; return false; }

    for (size_t i = 0; i < listenSocks_.size(); i++)
        acceptThreads_.emplace_back([this, i]() { AcceptLoop(binds_[i], listenSocks_[i]); });

    std::cout << "[DebugConsole] 调试控制台启动" << std::endl;
    std::cout << "[DebugConsole] telnet 127.0.0.1 " << port_ << " 连接" << std::endl;
    return true;
}

void DebugConsole::Stop()
{
    if (!running_) return;
    running_ = false;

    // 停止所有自动变位任务
    // 修复 M8 死锁：先在锁内把所有 AutoTask 取出到局部 vector 并设置 stopFlag，
    // 释放锁后再逐个 join。理由同 StopAutoTask。
    auto stoppedTasks = DetachAllTasksLocked(autoMtx_, autoTasks_);
    for (auto& t : stoppedTasks)
        if (t->thr.joinable()) t->thr.join();

    for (auto& sock : listenSocks_)
        try { sock.close(); } catch (...) {}
    for (auto& t : acceptThreads_)
        if (t.joinable()) t.join();
    listenSocks_.clear();
    acceptThreads_.clear();

    {
        std::lock_guard<std::mutex> lock(clientMtx_);
        for (auto& t : clientThreads_)
            if (t.joinable()) t.join();
        clientThreads_.clear();
    }

    std::cout << "[DebugConsole] 调试控制台已停止" << std::endl;
}

// ==================== Accept 循环 ====================

void DebugConsole::AcceptLoop(const BindConfig& bind, socket& listenSock)
{
    while (running_) {
        try {
            socket_addr peer;
            socket client = listenSock.accept(&peer);
            if (!running_) break;

            if (!bind.allowedIP.empty() && peer.host() != bind.allowedIP) {
                continue;
            }

            {
                std::lock_guard<std::mutex> lock(clientMtx_);
                // 定期清理已退出的客户端线程，避免累积
                if (++cleanupCnt_ % 10 == 0) {
                    for (auto it = clientThreads_.begin(); it != clientThreads_.end(); ) {
                        if (it->joinable()) { it->detach(); it = clientThreads_.erase(it); }
                        else ++it;
                    }
                }
                clientThreads_.emplace_back(&DebugConsole::ClientThread, this, std::move(client));
            }

        } catch (const socket_error&) {
            if (running_)
                std::cerr << "[DebugConsole] Accept 错误" << std::endl;
            break;
        }
    }
}

// ==================== 发送响应 ====================

void DebugConsole::Send(socket& sock, const std::string& text)
{
    try {
        sock.send(utf8_to_local(text));
    } catch (const socket_error&) {
        // 客户端断开，静默忽略
    }
}

void DebugConsole::SendLine(socket& sock, const std::string& line)
{
    Send(sock, line + "\r\n");
}

// ==================== 欢迎横幅 ====================

void DebugConsole::PrintBanner(socket& sock)
{
    SendLine(sock, "============================================");
    SendLine(sock, "  PMPC 四遥数据管理系统 - 调试控制台");
    SendLine(sock, "============================================");
    SendLine(sock, "  输入 help 查看命令列表");
    SendLine(sock, "  输入 quit 或 exit 断开连接");
    SendLine(sock, "============================================");
    SendLine(sock, "");
    CmdHelp(sock);
    SendLine(sock, "");
}

// ==================== 客户端线程 ====================

void DebugConsole::ClientThread(socket clientSock)
{
    if (verbose_ >= 1) {
        std::cout << "[DebugConsole] 客户端接入 (port=" << port_ << ")" << std::endl;
    }

    // 设置接收超时
    try {
        clientSock.set_recv_timeout(std::chrono::milliseconds(RECV_TIMEOUT_MS));
    } catch (...) {}

    // ── 密码认证 ──
    if (!password_.empty()) {
        SendLine(clientSock, "密码: ");
        std::string pwBuf;
        uint8_t tmp[256];
        bool authed = false;
        for (int attempt = 0; attempt < 3 && !authed; ) {
            size_t n;
            try {
                n = clientSock.recv(tmp, sizeof(tmp));
            } catch (const socket_error&) {
                // 超时 — 继续等，不断开
                continue;
            }
            if (n == 0) return;
            pwBuf.append(filter_telnet_iac(tmp, n));
            size_t pos;
            while ((pos = pwBuf.find('\n')) != std::string::npos) {
                std::string line = pwBuf.substr(0, pos);
                pwBuf.erase(0, pos + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                line = Trim(line);
                if (line == password_) {
                    authed = true;
                    break;
                }
                if (attempt < 2) SendLine(clientSock, "密码错误，请重试: ");
                ++attempt;
            }
        }
        if (!authed) {
            SendLine(clientSock, "认证失败，断开连接。");
            return;
        }
        SendLine(clientSock, "认证成功！");
    }

    // 打印欢迎信息 + 帮助
    PrintBanner(clientSock);

    std::string buf;
    uint8_t tmp[256];

    while (running_) {
        // 读取数据
        size_t n;
        try {
            n = clientSock.recv(tmp, sizeof(tmp));
        } catch (const socket_error&) {
            // recv 超时（WSAETIMEDOUT/EAGAIN）— 继续等待，不断开
            if (!running_) break;
            continue;
        }

        if (n == 0) {
            // recv 返回 0 = 对端正常关闭连接
            if (!buf.empty()) {
                if (!ProcessCommand(clientSock, buf)) break;
            }
            break;
        }

        // 累积到缓冲区（过滤 telnet IAC 协商字节）
        buf.append(filter_telnet_iac(tmp, n));

        // 逐行处理
        size_t pos;
        while ((pos = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, pos);
            buf.erase(0, pos + 1);

            // 去掉 \r
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            // 去掉首尾空白
            line = Trim(line);

            if (line.empty()) continue;

            // 退出命令
            if (line == "quit" || line == "exit" || line == "q") {
                SendLine(clientSock, "再见！");
                return;
            }

            if (!ProcessCommand(clientSock, line))
                return;
        }
    }
}

// ==================== 命令分发 ====================

bool DebugConsole::ProcessCommand(socket& sock, const std::string& line)
{
    if (verbose_ >= 1) {
        std::cout << "[DebugConsole] 收到命令: \"" << line << "\"" << std::endl;
    }
    auto args = SplitArgs(line);
    if (args.empty()) return true;

    std::string cmd = ToLower(args[0]);

    if (cmd == "help" || cmd == "h" || cmd == "?") {
        CmdHelp(sock);
    }
    else if (cmd == "set") {
        if (args.size() < 2) { SendLine(sock, "用法: set <di|ai|do|ao> <ch> <dev> <pt> <value>"); return true; }
        std::string sub = ToLower(args[1]);
        if (sub == "di") CmdSetDi(sock, args);
        else if (sub == "ai") CmdSetAi(sock, args);
        else if (sub == "do") CmdSetDo(sock, args);
        else if (sub == "ao") CmdSetAo(sock, args);
        else SendLine(sock, "未知类型: " + args[1] + " (可选: di/ai/do/ao)");
    }
    else if (cmd == "get") {
        if (args.size() < 2) { SendLine(sock, "用法: get <di|ai> <ch> <dev> [pt]"); return true; }
        std::string sub = ToLower(args[1]);
        if (sub == "di") CmdGetDi(sock, args);
        else if (sub == "ai") CmdGetAi(sock, args);
        else SendLine(sock, "未知类型: " + args[1] + " (可选: di/ai)");
    }
    else if (cmd == "auto") {
        if (args.size() < 2) { SendLine(sock, "用法: auto <di|ai> <ch> <dev> <pt> <interval_ms>"); return true; }
        std::string sub = ToLower(args[1]);
        if (sub == "stop") CmdAutoStop(sock, args);
        else if (sub == "di" || sub == "ai") CmdAuto(sock, args);
        else SendLine(sock, "未知: " + args[1] + " (可选: di/ai/stop)");
    }
    else if (cmd == "role") {
        CmdRole(sock, args);
    }
    else if (cmd == "status" || cmd == "st") {
        CmdStatus(sock);
    }
    else if (cmd == "channels" || cmd == "ch") {
        CmdChannels(sock);
    }
    else if (cmd == "devices" || cmd == "dev") {
        CmdDevices(sock, args);
    }
    else if (cmd == "reload") {
        CmdReload(sock, args);
    }
    else if (cmd == "start" || cmd == "stop") {
        CmdStartStop(sock, args);
    }
    else if (cmd == "upload") {
        CmdUpload(sock, args);
    }
    else if (cmd == "log") {
        CmdLog(sock, args);
    }
    else if (cmd == "cls" || cmd == "clear") {
        Send(sock, "\033[2J\033[H");
    }
    else {
        SendLine(sock, "未知命令: " + cmd + " (输入 help 查看帮助)");
    }

    return true;
}

// ==================== 辅助函数 ====================

bool DebugConsole::ParseUint16(const std::string& s, uint16_t& out)
{
    try {
        int v = std::stoi(s);
        if (v < 0 || v > 65535) return false;
        out = static_cast<uint16_t>(v);
        return true;
    } catch (...) {
        return false;
    }
}

bool DebugConsole::ParseInt(const std::string& s, int& out)
{
    try {
        out = std::stoi(s);
        return true;
    } catch (...) {
        return false;
    }
}

bool DebugConsole::ParseDouble(const std::string& s, double& out)
{
    try {
        out = std::stod(s);
        return true;
    } catch (...) {
        return false;
    }
}

std::vector<std::string> DebugConsole::SplitArgs(const std::string& line)
{
    std::vector<std::string> args;
    std::istringstream iss(line);
    std::string token;
    while (iss >> token) args.push_back(token);
    return args;
}

// ==================== 帮助 ====================

void DebugConsole::CmdHelp(socket& sock)
{
    SendLine(sock, "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
    SendLine(sock, "  命令列表");
    SendLine(sock, "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
    SendLine(sock, "");
    SendLine(sock, "── 读取四遥值 ──");
    SendLine(sock, "  get di   <ch> <dev> [pt]     读取遥信（不指定 pt 则列出全部）");
    SendLine(sock, "  get ai   <ch> <dev> [pt]     读取遥测");
    SendLine(sock, "  示例: get di 1 1          → 通道1设备1全部遥信");
    SendLine(sock, "        get ai 1 1 5        → 通道1设备1第5点遥测");
    SendLine(sock, "");
    SendLine(sock, "── 设置四遥值 ──");
    SendLine(sock, "  set di   <ch> <dev> <pt> <0|1>       设置遥信");
    SendLine(sock, "  set ai   <ch> <dev> <pt> <value>     设置遥测");
    SendLine(sock, "  set do   <ch> <dev> <pt> <0|1>       设置遥控主站值");
    SendLine(sock, "  set ao   <ch> <dev> <pt> <value>     设置遥调");
    SendLine(sock, "  示例: set di 1 1 3 1      → 通道1设备1第3点遥信=ON");
    SendLine(sock, "        set ai 1 1 2 23.45  → 通道1设备1第2点遥测=23.45");
    SendLine(sock, "");
    SendLine(sock, "── 自动变位（模拟采集） ──");
    SendLine(sock, "  auto di  <ch> <dev> <pt> <ms>    DI 自动翻转（周期毫秒）");
    SendLine(sock, "  auto ai  <ch> <dev> <pt> <ms>    AI 自动正弦波（周期毫秒）");
    SendLine(sock, "  auto stop <id>                   停止指定自动变位任务");
    SendLine(sock, "  auto stop all                    停止全部自动变位任务");
    SendLine(sock, "  示例: auto di 1 1 10 500    → 开始任务#N (500ms翻转)");
    SendLine(sock, "        auto stop 1            → 停止任务#1");
    SendLine(sock, "");
    SendLine(sock, "── 报文记录 ──");
    SendLine(sock, "  log start [ch dev]              启动报文记录（可选指定设备）");
    SendLine(sock, "  log stop                        停止报文记录");
    SendLine(sock, "  log status                      查看记录状态");
    SendLine(sock, "  log parse <on|off>              开启/关闭报文解析");
    SendLine(sock, "  示例: log start 1 1       → 记录通道1设备1的报文");
    SendLine(sock, "        log parse off        → 仅记录 HEX，不解析");
    SendLine(sock, "  注: 日志文件位于 logs/traffic/ch{N}_dev{N}_YYYYMMDD.log");
    SendLine(sock, "");
    SendLine(sock, "── 模块控制 ──");
    SendLine(sock, "  start <module_name>              启动模块");
    SendLine(sock, "  stop  <module_name>              停止模块");
    SendLine(sock, "  reload [point|<module_name>]     重载点表或模块配置");
    SendLine(sock, "  示例: start modbus_tcp_master");
    SendLine(sock, "        reload modbus_tcp_slave");
    SendLine(sock, "        reload point");
    SendLine(sock, "");
    SendLine(sock, "── 角色切换（双机冗余） ──");
    SendLine(sock, "  role             查看当前角色");
    SendLine(sock, "  role master      切换为主机");
    SendLine(sock, "  role standby     切换为备机");
    SendLine(sock, "  role idle        切换为空闲");
    SendLine(sock, "  示例: role master");
    SendLine(sock, "");
    SendLine(sock, "── 查询 ──");
    SendLine(sock, "  status                     模块运行状态");
    SendLine(sock, "  channels                   查看通道列表");
    SendLine(sock, "  devices <ch>               查看指定通道的设备列表");
    SendLine(sock, "");
    SendLine(sock, "── 其他 ──");
    SendLine(sock, "  help / h / ?               本帮助");
    SendLine(sock, "  quit / exit / q            断开连接");
    SendLine(sock, "  cls / clear                清屏");
    SendLine(sock, "");
    SendLine(sock, "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
}

// ==================== Get 命令 ====================

void DebugConsole::CmdGetDi(socket& sock, const std::vector<std::string>& args)
{
    if (args.size() < 4) { SendLine(sock, "用法: get di <ch> <dev> [pt]"); return; }

    uint16_t ch, dev, pt = 0;
    bool hasPt = (args.size() >= 5);
    if (!ParseUint16(args[2], ch) || !ParseUint16(args[3], dev)) {
        SendLine(sock, "参数错误: ch 和 dev 须为数字"); return;
    }
    if (hasPt && !ParseUint16(args[4], pt)) {
        SendLine(sock, "参数错误: pt 须为数字"); return;
    }

    auto& mgr = RemoteDataMgr::Instance();

    if (hasPt) {
        DiPoint p;
        if (!mgr.GetDi(ch, dev, pt, p)) {
            SendLine(sock, "未找到: ch=" + std::to_string(ch)
                     + " dev=" + std::to_string(dev)
                     + " pt=" + std::to_string(pt));
            return;
        }
        char buf[128];
        std::snprintf(buf, sizeof(buf), "DI  ch=%u dev=%u pt=%u val=%s ts=%llu",
                      ch, dev, pt, p.value ? "1(ON)" : "0(OFF)",
                      (unsigned long long)p.tsMs);
        SendLine(sock, buf);
    } else {
        std::vector<DiPoint> list;
        if (!mgr.GetDiList(ch, dev, list) || list.empty()) {
            SendLine(sock, "无遥信点: ch=" + std::to_string(ch)
                     + " dev=" + std::to_string(dev));
            return;
        }
        for (auto& p : list) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "  pt=%u val=%s ts=%llu",
                          p.pointNo, p.value ? "1(ON)" : "0(OFF)",
                          (unsigned long long)p.tsMs);
            SendLine(sock, buf);
        }
        SendLine(sock, "共 " + std::to_string(list.size()) + " 点");
    }
}

void DebugConsole::CmdGetAi(socket& sock, const std::vector<std::string>& args)
{
    if (args.size() < 4) { SendLine(sock, "用法: get ai <ch> <dev> [pt]"); return; }

    uint16_t ch, dev, pt = 0;
    bool hasPt = (args.size() >= 5);
    if (!ParseUint16(args[2], ch) || !ParseUint16(args[3], dev)) {
        SendLine(sock, "参数错误"); return;
    }
    if (hasPt && !ParseUint16(args[4], pt)) { SendLine(sock, "参数错误"); return; }

    auto& mgr = RemoteDataMgr::Instance();

    if (hasPt) {
        AiPoint p;
        if (!mgr.GetAi(ch, dev, pt, p)) {
            SendLine(sock, "未找到"); return;
        }
        char buf[128];
        std::snprintf(buf, sizeof(buf), "AI  ch=%u dev=%u pt=%u val=%.4f",
                      ch, dev, pt, p.value);
        SendLine(sock, buf);
    } else {
        std::vector<AiPoint> list;
        if (!mgr.GetAiList(ch, dev, list) || list.empty()) {
            SendLine(sock, "无遥测点"); return;
        }
        for (auto& p : list) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "  pt=%u val=%.4f",
                          p.pointNo, p.value);
            SendLine(sock, buf);
        }
        SendLine(sock, "共 " + std::to_string(list.size()) + " 点");
    }
}

// ==================== Set 命令 ====================

void DebugConsole::CmdSetDi(socket& sock, const std::vector<std::string>& args)
{
    if (args.size() < 6) { SendLine(sock, "用法: set di <ch> <dev> <pt> <0|1>"); return; }
    uint16_t ch, dev, pt;
    int val;
    if (!ParseUint16(args[2], ch) || !ParseUint16(args[3], dev)
        || !ParseUint16(args[4], pt) || !ParseInt(args[5], val)
        || (val != 0 && val != 1)) {
        SendLine(sock, "参数错误"); return;
    }

    uint64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    RemoteDataMgr::Instance().SetDi(ch, dev, pt, val != 0, now, true);

    char buf[128];
    std::snprintf(buf, sizeof(buf), "OK: DI ch=%u dev=%u pt=%u = %s",
                  ch, dev, pt, val ? "ON" : "OFF");
    SendLine(sock, buf);
}

void DebugConsole::CmdSetAi(socket& sock, const std::vector<std::string>& args)
{
    if (args.size() < 6) { SendLine(sock, "用法: set ai <ch> <dev> <pt> <value>"); return; }
    uint16_t ch, dev, pt;
    double val;
    if (!ParseUint16(args[2], ch) || !ParseUint16(args[3], dev)
        || !ParseUint16(args[4], pt) || !ParseDouble(args[5], val)) {
        SendLine(sock, "参数错误"); return;
    }

    RemoteDataMgr::Instance().SetAi(ch, dev, pt, val);

    char buf[128];
    std::snprintf(buf, sizeof(buf), "OK: AI ch=%u dev=%u pt=%u = %.4f",
                  ch, dev, pt, val);
    SendLine(sock, buf);
}

void DebugConsole::CmdSetDo(socket& sock, const std::vector<std::string>& args)
{
    if (args.size() < 6) { SendLine(sock, "用法: set do <ch> <dev> <pt> <0|1>"); return; }
    uint16_t ch, dev, pt;
    int val;
    if (!ParseUint16(args[2], ch) || !ParseUint16(args[3], dev)
        || !ParseUint16(args[4], pt) || !ParseInt(args[5], val)
        || (val != 0 && val != 1)) {
        SendLine(sock, "参数错误"); return;
    }

    RemoteDataMgr::Instance().SetDoMaster(ch, dev, pt, val != 0);

    char buf[128];
    std::snprintf(buf, sizeof(buf), "OK: DO ch=%u dev=%u pt=%u master=%s",
                  ch, dev, pt, val ? "ON" : "OFF");
    SendLine(sock, buf);
}

void DebugConsole::CmdSetAo(socket& sock, const std::vector<std::string>& args)
{
    if (args.size() < 6) { SendLine(sock, "用法: set ao <ch> <dev> <pt> <value>"); return; }
    uint16_t ch, dev, pt;
    double val;
    if (!ParseUint16(args[2], ch) || !ParseUint16(args[3], dev)
        || !ParseUint16(args[4], pt) || !ParseDouble(args[5], val)) {
        SendLine(sock, "参数错误"); return;
    }

    RemoteDataMgr::Instance().SetAo(ch, dev, pt, val);

    char buf[128];
    std::snprintf(buf, sizeof(buf), "OK: AO ch=%u dev=%u pt=%u = %.4f",
                  ch, dev, pt, val);
    SendLine(sock, buf);
}

// ==================== 自动变位 ====================

void DebugConsole::CmdAuto(socket& sock, const std::vector<std::string>& args)
{
    // args: auto <di|ai> <ch> <dev> <pt> <interval_ms>
    if (args.size() < 6) { SendLine(sock, "用法: auto <di|ai> <ch> <dev> <pt> <interval_ms>"); return; }

    uint16_t ch, dev, pt;
    int intervalMs;
    if (!ParseUint16(args[2], ch) || !ParseUint16(args[3], dev)
        || !ParseUint16(args[4], pt) || !ParseInt(args[5], intervalMs)
        || intervalMs < 100) {
        SendLine(sock, "参数错误（interval_ms >= 100）"); return;
    }

    AutoTask::Type type = (ToLower(args[1]) == "ai")
                          ? AutoTask::AI_SWEEP : AutoTask::DI_TOGGLE;

    int id = CreateAutoTask(type, ch, dev, pt, intervalMs);

    char buf[128];
    std::snprintf(buf, sizeof(buf), "OK: 自动变位任务#%d 已启动 (%s ch=%u dev=%u pt=%u %dms)",
                  id, args[1].c_str(), ch, dev, pt, intervalMs);
    SendLine(sock, buf);
}

void DebugConsole::CmdAutoStop(socket& sock, const std::vector<std::string>& args)
{
    if (args.size() < 3) { SendLine(sock, "用法: auto stop <id|all>"); return; }

    if (ToLower(args[2]) == "all") {
        // 修复 M8 死锁：同 StopAutoTask，锁内取出、锁外 join。
        auto stoppedTasks = DetachAllTasksLocked(autoMtx_, autoTasks_);
        for (auto& t : stoppedTasks)
            if (t->thr.joinable()) t->thr.join();
        SendLine(sock, "OK: 已停止全部自动变位任务");
        return;
    }

    int id;
    if (!ParseInt(args[2], id)) { SendLine(sock, "参数错误"); return; }

    if (StopAutoTask(id))
        SendLine(sock, "OK: 任务#" + std::to_string(id) + " 已停止");
    else
        SendLine(sock, "未找到任务#" + std::to_string(id));
}

int DebugConsole::CreateAutoTask(AutoTask::Type type, uint16_t ch,
                                  uint16_t dev, uint16_t pt, int intervalMs)
{
    auto task = std::make_unique<AutoTask>();
    task->id = nextAutoId_++;
    task->type = type;
    task->ch = ch;
    task->dev = dev;
    task->pt = pt;
    task->intervalMs = intervalMs;

    int id = task->id;

    if (type == AutoTask::DI_TOGGLE) {
        task->thr = std::thread(&DebugConsole::AutoToggleThread, this,
                                id, ch, dev, pt, intervalMs);
    } else {
        task->thr = std::thread(&DebugConsole::AutoSweepThread, this,
                                id, ch, dev, pt, intervalMs);
    }

    std::lock_guard<std::mutex> lock(autoMtx_);
    autoTasks_.push_back(std::move(task));
    return id;
}

bool DebugConsole::StopAutoTask(int id)
{
    // 修复 M8 死锁：以前是「先持 autoMtx_ → join()」，
    // 但 AutoToggleThread/AutoSweepThread 每轮循环都要再抢 autoMtx_，
    // → 工作线程等锁、Stop 线程等 join，必然死锁。
    // 现在的做法：DetachTaskLocked 在锁内把 unique_ptr 取出、标记 stopFlag、
    // 从 vector erase；锁外再 join。stopFlag 是 atomic 从锁外可安全读写。
    auto victim = DetachTaskLocked(autoMtx_, autoTasks_, id);
    if (!victim) return false;
    if (victim->thr.joinable()) victim->thr.join();
    return true;
}

void DebugConsole::AutoToggleThread(int /*taskId*/, uint16_t ch, uint16_t dev,
                                     uint16_t pt, int intervalMs)
{
    while (running_) {
        {
            std::lock_guard<std::mutex> lock(autoMtx_);
            // 检查任务是否已被删除
            bool found = false;
            for (auto& t : autoTasks_) {
                if (t->thr.get_id() == std::this_thread::get_id()) {
                    found = true;
                    if (t->stopFlag) return;
                    break;
                }
            }
            if (!found) return;
        }

        DiPoint p;
        RemoteDataMgr::Instance().GetDi(ch, dev, pt, p);
        bool newVal = !p.value;
        uint64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        RemoteDataMgr::Instance().SetDi(ch, dev, pt, newVal, now, true);

        for (int i = 0; i < intervalMs / 100 && running_; i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void DebugConsole::AutoSweepThread(int /*taskId*/, uint16_t ch, uint16_t dev,
                                    uint16_t pt, int intervalMs)
{
    double phase = 0.0;
    while (running_) {
        {
            std::lock_guard<std::mutex> lock(autoMtx_);
            bool found = false;
            for (auto& t : autoTasks_) {
                if (t->thr.get_id() == std::this_thread::get_id()) {
                    found = true;
                    if (t->stopFlag) return;
                    break;
                }
            }
            if (!found) return;
        }

        // 0~100 正弦波
        double val = 50.0 + 50.0 * std::sin(phase);
        RemoteDataMgr::Instance().SetAi(ch, dev, pt, val);
        phase += 0.1;
        if (phase > 2.0 * M_PI) phase -= 2.0 * M_PI;

        for (int i = 0; i < intervalMs / 100 && running_; i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

// ==================== 角色切换 ====================

void DebugConsole::CmdRole(socket& sock, const std::vector<std::string>& args)
{
    if (!g_moduleManager) {
        SendLine(sock, "错误: ModuleManager 未初始化");
        return;
    }

    auto* mod = g_moduleManager->GetModule("redundancy");
    if (!mod) {
        SendLine(sock, "冗余模块未加载（检查 app_modules.ini 是否 enable=1）");
        return;
    }

    auto* rdMod = dynamic_cast<RedundancyModule*>(mod);
    if (!rdMod) {
        SendLine(sock, "错误: 无法获取冗余模块");
        return;
    }

    auto& mgr = rdMod->GetManager();

    if (args.size() < 2) {
        // 查看当前角色
        char buf[128];
        std::snprintf(buf, sizeof(buf), "当前角色: %s  (对端:%s)",
                      RoleName(mgr.GetRole()),
                      mgr.IsPeerAlive() ? "在线" : "离线");
        SendLine(sock, buf);
        return;
    }

    std::string target = ToLower(args[1]);
    RedundRole newRole;
    if (target == "master") newRole = RedundRole::Master;
    else if (target == "standby") newRole = RedundRole::Standby;
    else if (target == "idle") newRole = RedundRole::Idle;
    else {
        SendLine(sock, "未知角色: " + args[1] + " (可选: master/standby/idle)");
        return;
    }

    mgr.RequestRoleChange(newRole);
    SendLine(sock, std::string("OK: 角色已切换为 ") + RoleName(newRole));
}

// ==================== 状态查询 ====================

void DebugConsole::CmdStatus(socket& sock)
{
    SendLine(sock, "── 模块运行状态 ──");

    if (!g_moduleManager) {
        SendLine(sock, "  ModuleManager 未初始化");
        return;
    }

    auto status = g_moduleManager->GetStatus();
    for (auto& [name, running] : status) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "  %-20s %s", name.c_str(),
                      running ? "RUNNING" : "STOPPED");
        SendLine(sock, buf);
    }
}

// ==================== 通道/设备查询 ====================

void DebugConsole::CmdChannels(socket& sock)
{
    auto& mgr = RemoteDataMgr::Instance();
    auto ids = mgr.GetChannelIds();
    if (ids.empty()) {
        SendLine(sock, "无通道");
        return;
    }
    SendLine(sock, "── 通道列表 ──");
    for (auto ch : ids) {
        auto devIds = mgr.GetDeviceIds(ch);
        char buf[128];
        std::snprintf(buf, sizeof(buf), "  通道%u: %zu 个设备", ch, devIds.size());
        SendLine(sock, buf);
    }
}

void DebugConsole::CmdDevices(socket& sock, const std::vector<std::string>& args)
{
    if (args.size() < 2) { SendLine(sock, "用法: devices <ch>"); return; }
    uint16_t ch;
    if (!ParseUint16(args[1], ch)) { SendLine(sock, "参数错误"); return; }

    auto& mgr = RemoteDataMgr::Instance();
    auto devIds = mgr.GetDeviceIds(ch);
    if (devIds.empty()) {
        SendLine(sock, "通道" + std::to_string(ch) + " 无设备");
        return;
    }

    std::string rsp = "── 通道" + std::to_string(ch) + " 设备列表 ──";
    SendLine(sock, rsp);
    for (auto dev : devIds) {
        std::vector<DiPoint> di;
        std::vector<AiPoint> ai;
        mgr.GetDiList(ch, dev, di);
        mgr.GetAiList(ch, dev, ai);
        char buf[128];
        std::snprintf(buf, sizeof(buf), "  设备%u: DI=%zu AI=%zu", dev, di.size(), ai.size());
        SendLine(sock, buf);
    }
}

// ==================== 重载配置 ====================

void DebugConsole::CmdReload(socket& sock, const std::vector<std::string>& args)
{
    if (!g_moduleManager) {
        SendLine(sock, "错误: ModuleManager 未初始化");
        return;
    }

    if (args.size() < 2) {
        SendLine(sock, "用法: reload <module_name|point>");
        return;
    }

    std::string target = args[1];

    if (target == "point") {
        SendLine(sock, "重载点表...");
        auto& mgr = RemoteDataMgr::Instance();
        ConfigErrorReporter errRep;
        if (mgr.LoadConfig("point_cfg.ini", &errRep)) {
            std::string msg = "OK: 点表重载完成";
            if (errRep.HasErrors())
                msg += " (" + std::to_string(errRep.ErrorCount()) + " 条警告)";
            SendLine(sock, msg);
        } else {
            SendLine(sock, "错误: 点表重载失败");
        }
        return;
    }

    if (g_moduleManager->ReloadModule(target))
        SendLine(sock, "OK: 模块 " + target + " 重载完成");
    else
        SendLine(sock, "错误: 模块 " + target + " 重载失败（模块不存在或配置有误）");
}

// ==================== 启停模块 ====================

void DebugConsole::CmdStartStop(socket& sock, const std::vector<std::string>& args)
{
    if (!g_moduleManager) {
        SendLine(sock, "错误: ModuleManager 未初始化");
        return;
    }
    if (args.size() < 2) {
        SendLine(sock, "用法: start|stop <module_name>");
        return;
    }

    std::string cmd = ToLower(args[0]);
    std::string modName = args[1];
    bool ok;

    if (cmd == "start") {
        ok = g_moduleManager->StartModule(modName);
    } else {
        ok = g_moduleManager->StopModule(modName);
    }

    if (ok)
        SendLine(sock, "OK: " + cmd + " " + modName + " 成功");
    else
        SendLine(sock, "错误: " + cmd + " " + modName + " 失败");
}

// ==================== 上传控制（桩） ====================

void DebugConsole::CmdUpload(socket& sock, const std::vector<std::string>& args)
{
    if (args.size() < 2) {
        SendLine(sock, "用法: upload start|stop");
        SendLine(sock, "注: 自动上传控制需要在 PempServer 中实现");
        return;
    }

    std::string action = ToLower(args[1]);
    if (action == "start") {
        // PempServer 暂无自动上传启停接口
        SendLine(sock, "自动上传启动（TODO: 需 PempServer 支持）");
    } else if (action == "stop") {
        SendLine(sock, "自动上传停止（TODO: 需 PempServer 支持）");
    } else {
        SendLine(sock, "未知: " + args[1] + " (可选: start/stop)");
    }
}

// ==================== 报文记录控制 ====================

void DebugConsole::CmdLog(socket& sock, const std::vector<std::string>& args)
{
    // log start|stop|status|parse|help [ch] [dev]
    if (args.size() < 2) {
        SendLine(sock, "用法:");
        SendLine(sock, "  log start          启动报文记录（记录全部通道/设备）");
        SendLine(sock, "  log start <ch> <dev>  启动报文记录（仅记录指定设备）");
        SendLine(sock, "  log stop           停止报文记录");
        SendLine(sock, "  log status         查看记录状态");
        SendLine(sock, "  log parse <on|off> 开启/关闭报文解析");
        return;
    }

    auto* plMod = PacketLoggerModule::GetInstance();
    if (!plMod || !plMod->IsRunning()) {
        SendLine(sock, "错误: packet_logger 模块未加载或未启动");
        return;
    }

    auto& logger = plMod->GetLogger();
    std::string sub = ToLower(args[1]);

    if (sub == "start") {
        if (args.size() >= 4) {
            uint16_t ch, dev;
            if (!ParseUint16(args[2], ch) || !ParseUint16(args[3], dev)) {
                SendLine(sock, "参数错误");
                return;
            }
            logger.EnableDevice(ch, dev);
            logger.SetEnabled(true);
            char buf[128];
            std::snprintf(buf, sizeof(buf), "OK: 报文记录已启动 (ch=%u dev=%u)", ch, dev);
            SendLine(sock, buf);
        } else {
            logger.EnableAll();
            logger.SetEnabled(true);
            SendLine(sock, "OK: 报文记录已启动 (全部设备)");
        }
    }
    else if (sub == "stop") {
        logger.SetEnabled(false);
        SendLine(sock, "OK: 报文记录已停止");
    }
    else if (sub == "status") {
        SendLine(sock, logger.GetStatus());
        SendLine(sock, "模块状态: " + std::string(plMod->IsRunning()?"运行中":"已停止"));
    }
    else if (sub == "parse") {
        if (args.size() < 3) {
            SendLine(sock, "用法: log parse <on|off>");
            return;
        }
        std::string en = ToLower(args[2]);
        if (en == "on" || en == "1" || en == "true") {
            logger.SetParseEnabled(true);
            SendLine(sock, "OK: 报文解析已开启");
        } else {
            logger.SetParseEnabled(false);
            SendLine(sock, "OK: 报文解析已关闭（仅记录 HEX）");
        }
    }
    else {
        SendLine(sock, "未知: " + args[1] + " (可选: start/stop/status/parse)");
    }
}

// ==================== DebugConsoleModule ====================

struct DebugConsoleModule::Impl {
    DebugConsole console;
    std::string cfgPath;
    bool loaded = true;  // 默认 true，允许无配置文件
    bool running = false;
};

DebugConsoleModule::DebugConsoleModule()
    : impl_(std::make_unique<Impl>()) {}

DebugConsoleModule::~DebugConsoleModule() { Stop(); }

bool DebugConsoleModule::LoadConfig(const std::string& cfgPath)
{
    impl_->cfgPath = cfgPath;
    impl_->loaded = impl_->console.LoadConfig(cfgPath);
    return impl_->loaded;
}

bool DebugConsoleModule::ValidateConfig(const std::string& cfgPath,
                                         std::vector<std::string>& errors)
{
    DebugConsole console;
    if (!console.LoadConfig(cfgPath)) {
        errors.push_back("Cannot load: " + cfgPath);
        return false;
    }
    return errors.empty();
}

bool DebugConsoleModule::Start()
{
    if (impl_->running) return true;
    impl_->running = impl_->console.Start();
    return impl_->running;
}

void DebugConsoleModule::Stop()
{
    if (!impl_->running) return;
    impl_->console.Stop();
    impl_->running = false;
}

bool DebugConsoleModule::IsRunning() const
{
    return impl_->running;
}

REGISTER_MODULE("debug_console", DebugConsoleModule)
