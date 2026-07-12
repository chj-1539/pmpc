//=============================================================================
// pemp_server.cxx — PEMP2.0 TCP 通讯服务实现
//
// 每连接独立副本线程，消息循环结构：
//   1. try_recv 后台命令 (500ms 超时)
//   2. 收到命令 → handle_request → 处理并回复
//   3. 超时 → 主动上传模式(ACTIVE)下按配置间隔发送 DI/AI/SOE
//   4. 一发一答约束：发送主动数据后必须等 ACK 或超时
//=============================================================================

#include "pemp_server.h"
#include "soe_queue.h"
#include <iostream>
#include <cstring>
#include <climits>
#include <algorithm>
#include <mutex>
#include <set>

// 前置声明：这些 static helper 定义在文件下半部（与 do_upload_di/ai 语义
// 相关），但 handle_call_telemetry / do_upload_ai 都会用。
static bool    CheckPempIds8Bit(uint16_t chId, uint16_t devNo);
static int32_t QuantizeAiToInt32Warn(uint16_t chId, uint16_t devNo,
                                     uint16_t pt, double v);

// ─── 常量 ──────────────────────────────────────────────────────────────────
static constexpr auto RECV_TIMEOUT  = std::chrono::milliseconds(500);
static constexpr auto ACK_TIMEOUT   = std::chrono::milliseconds(1500);

// ─── 工作状态 ──────────────────────────────────────────────────────────────
// bit0 (0x01): 自检正常
// bit1 (0x02): 主动上传中
// bit2 (0x04): 主机
static constexpr uint8_t STATE_SELF_CHECK_OK = 0x01;
static constexpr uint8_t STATE_UPLOAD_ACTIVE = 0x02;
static constexpr uint8_t STATE_IS_MASTER     = 0x04;

// ==================== 构造 / 析构 ====================

PempServer::PempServer(uint16_t port, int diUploadMs, int aiUploadMs)
    : port_(port), diUploadMs_(diUploadMs), aiUploadMs_(aiUploadMs)
{
}

PempServer::~PempServer()
{
    stop();
}

// ==================== 启停 ====================

bool PempServer::start()
{
    // 确保先清理旧状态（上次 start 可能部分失败）
    for (auto& sock : listenSocks_)
        try { sock.close(); } catch (...) {}
    listenSocks_.clear();
    acceptThreads_.clear();

    running_ = true;
    auto& b = binds_;
    if (b.empty()) {
        PempBind pb;
        pb.port = port_;
        b.push_back(pb);
    }
    for (auto& bind : b) {
        try {
            socket sock;
            sock.bind(socket_addr("0.0.0.0", bind.port, protocol_type::tcp));
            sock.listen(128);
            listenSocks_.push_back(std::move(sock));
            if (bind.allowedIP.empty())
                std::cout << "[PempServer] 监听 ALL:" << bind.port
                          << " (安全建议: 配置 allowedIP 限制访问来源)" << std::endl;
            else
                std::cout << "[PempServer] 监听 " << bind.allowedIP
                          << ":" << bind.port << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "[PempServer] 绑定端口 " << bind.port << " 失败: " << e.what() << std::endl;
            // 清理已成功绑定的端口
            for (auto& sock : listenSocks_)
                try { sock.close(); } catch (...) {}
            listenSocks_.clear();
            running_ = false;
            return false;
        }
    }
    if (listenSocks_.empty()) { running_ = false; return false; }
    for (size_t i = 0; i < listenSocks_.size(); i++)
        acceptThreads_.emplace_back([this, i]() { accept_loop(binds_[i], listenSocks_[i]); });

    // ── 订阅 DI 变化事件 → SOE 队列 ──
    if (diSubToken_ == 0) {
        diSubToken_ = EventBus::Subscribe<DIChange>([](const DIChange& e) {
            // 点号 1 是通讯状态指示，不生成 SOE
            if (e.point == 1) return;
            // 无时间戳的不推
            if (e.tsMs == 0) return;

            SOEEvent soe;
            soe.channel   = static_cast<uint8_t>(e.channel & 0xFF);
            soe.device    = static_cast<uint8_t>(e.device & 0xFF);
            soe.soeId     = e.point;
            soe.timestamp = CP56time2a::FromMs(e.tsMs);
            soe.status    = e.value ? 0x02 : 0x01;  // 02=合(ON), 01=开(OFF)
            g_soeQueue.Push(soe);
        });
        std::cout << "[PempServer] DI→SOE 订阅已注册" << std::endl;
    }

    return true;
}

void PempServer::stop()
{
    running_ = false;

    // ── 取消 EventBus 订阅 ──
    if (diSubToken_ != 0) {
        EventBus::Unsubscribe<DIChange>(diSubToken_);
        diSubToken_ = 0;
        std::cout << "[PempServer] DI→SOE 订阅已取消" << std::endl;
    }

    for (auto& sock : listenSocks_)
        try { sock.close(); } catch (...) {}
    for (auto& t : acceptThreads_)
        if (t.joinable()) t.join();
    listenSocks_.clear();
    acceptThreads_.clear();
    // L6 修复：以前 detach 客户端线程，之后 ~PempServer/delete server 会
    // 使已 detach 的线程访问被析构的成员（use-after-free）。
    // client_handler 的 recv 超时是 500ms 且循环检查 running_，最坏 500ms
    // 内自行退出，可以安全 join。
    {
        std::lock_guard<std::mutex> lock(clientMtx_);
        for (auto& t : clientThreads_)
            if (t.joinable()) t.join();
        clientThreads_.clear();
    }
}

// ==================== 接收连接循环 ====================

void PempServer::accept_loop(const PempBind& bind, socket& listen_sock)
{
    while (running_)
    {
        try {
            socket_addr peer;
            socket client = listen_sock.accept(&peer);
            if (!running_) break;

            // IP 过滤
            if (!bind.allowedIP.empty() && peer.host() != bind.allowedIP) {
                continue;
            }

            std::cout << "[PempServer] 主机接入: "
                      << peer.to_string() << std::endl;
            {
                std::lock_guard<std::mutex> lock(clientMtx_);
                clientThreads_.emplace_back(&PempServer::client_handler, this,
                    std::move(client), peer);
            }
        }
        catch (const socket_error& e) {
            if (running_)
                std::cerr << "[PempServer] accept 错误: "
                          << e.what() << std::endl;
        }
    }
}

// ==================== 客户端消息循环 ====================

void PempServer::client_handler(socket client, socket_addr peer)
{
    // ── 连接级工作状态 ──
    uint8_t  workState   = STATE_SELF_CHECK_OK | STATE_IS_MASTER;
    bool     waitingAck  = false;
    auto     ackWaitStart = std::chrono::steady_clock::now();
    auto     lastDiUpload = std::chrono::steady_clock::now();
    auto     lastAiUpload = std::chrono::steady_clock::now();

    // ── 帧解析器 ──
    FrameParser parser;

    // ── 接收缓冲区 ──
    uint8_t recvBuf[4096];

    // ── 接收超时设置 ──
    try { client.set_recv_timeout(RECV_TIMEOUT); } catch (...) {}

    std::cout << "[PempServer] 客户端线程启动: " << peer.to_string()
              << " diInterval=" << diUploadMs_ << "ms"
              << " aiInterval=" << aiUploadMs_ << "ms" << std::endl;

    while (g_running && running_)
    {
        try {
            // ── ACK 超时检查 ──
            if (waitingAck)
            {
                auto elapsed = std::chrono::steady_clock::now() - ackWaitStart;
                if (elapsed >= ACK_TIMEOUT)
                {
                    waitingAck = false;
                    std::cerr << "[PempServer] ACK超时 ("
                              << peer.to_string() << ")" << std::endl;
                }
            }

            // ── 接收数据 ──
            bool gotData = false;
            size_t n = 0;
            try {
                n = client.recv(recvBuf, sizeof(recvBuf));
                if (n == 0) break; // 对端关闭连接
                gotData = true;
            }
            catch (const socket_error& e) {
                if (e.code() == socket_errc::timeout ||
                    e.code() == socket_errc::would_block)
                {
                    // 超时是正常的，稍后检查主动上传
                }
                else if (e.code() == socket_errc::closed)
                {
                    break;
                }
                else {
                    throw;
                }
            }

            // ── 解析并处理接收到的帧 ──
            if (gotData)
            {
                auto frames = parser.Feed(recvBuf, n);
                for (const auto& frame : frames)
                {
                    if (!FrameParser::Validate(frame))
                    {
                        std::cerr << "[PempServer] 帧校验失败 ("
                                  << peer.to_string() << ")" << std::endl;
                        continue;
                    }
                    waitingAck = false;
                    handle_request(frame, client, workState);
                }
            }

            // ── 主动上传（仅 ACTIVE 状态 + 非等待 ACK） ──
            if ((workState & STATE_UPLOAD_ACTIVE) && !waitingAck)
            {
                auto now = std::chrono::steady_clock::now();

                // DI 上传
                if (diUploadMs_ > 0 &&
                    now - lastDiUpload >= std::chrono::milliseconds(diUploadMs_))
                {
                    if (do_upload_di(client))
                    {
                        waitingAck    = true;
                        ackWaitStart  = std::chrono::steady_clock::now();
                        lastDiUpload  = ackWaitStart;
                        continue;  // 等待 ACK，跳过本轮的 AI/SOE
                    }
                }

                // AI 上传
                if (aiUploadMs_ > 0 &&
                    now - lastAiUpload >= std::chrono::milliseconds(aiUploadMs_))
                {
                    if (do_upload_ai(client))
                    {
                        waitingAck    = true;
                        ackWaitStart  = std::chrono::steady_clock::now();
                        lastAiUpload  = ackWaitStart;
                        continue;
                    }
                }

                // SOE 上传（始终跟随 AI 或 DI 的间隔）
                if (g_soeQueue.PendingCount() > 0)
                {
                    if (do_upload_soe(client))
                    {
                        waitingAck    = true;
                        ackWaitStart  = std::chrono::steady_clock::now();
                    }
                }
            }
        }
        catch (const std::exception& e) {
            std::cerr << "[PempServer] 客户端异常 ("
                      << peer.to_string() << "): " << e.what() << std::endl;
            break;
        }
    }

    std::cout << "[PempServer] 主机断开: " << peer.to_string() << std::endl;
}

// ==================== 命令分发 ====================

void PempServer::handle_request(const std::vector<uint8_t>& frame,
                                socket& client, uint8_t& workState)
{
    FunCode fun = FrameParser::GetFun(frame);

    switch (fun)
    {
    case FunCode::QueryStatus:
    {
        auto resp = FrameBuilder::Make1Byte(FunCode::QueryStatus, workState);
        send_all(client, resp);
        break;
    }

    case FunCode::SwitchWorkState:
    {
        if (FrameParser::GetDataLen(frame) >= 1)
        {
            uint8_t req = FrameParser::ReadByte(frame, 0);
            switch (static_cast<WorkStateReq>(req))
            {
            case WorkStateReq::StartAutoUpload:
                workState |= STATE_UPLOAD_ACTIVE;
                std::cout << "[PempServer] 切换为主动上传模式" << std::endl;
                break;
            case WorkStateReq::StopAutoUpload:
                workState &= static_cast<uint8_t>(~STATE_UPLOAD_ACTIVE);
                std::cout << "[PempServer] 切换为非主动上传模式" << std::endl;
                break;
            case WorkStateReq::SwitchToMaster:
                workState |= STATE_IS_MASTER;
                break;
            case WorkStateReq::SwitchToStandby:
                workState &= static_cast<uint8_t>(~STATE_IS_MASTER);
                break;
            }
        }
        auto resp = FrameBuilder::Make1Byte(FunCode::SwitchWorkState, 0xFF);
        send_all(client, resp);
        break;
    }

    case FunCode::CallTelemetry:
        handle_call_telemetry(frame, client);
        break;

    case FunCode::CallHistorySOE:
        handle_call_history_soe(frame, client);
        break;

    case FunCode::ExecRemoteCtrl:
        handle_exec_remote_ctrl(frame, client);
        break;

    case FunCode::SyncClock:
        handle_sync_clock(frame, client);
        break;

    case FunCode::CallTeleInd:
        handle_call_tele_ind(frame, client);
        break;

    case FunCode::UploadTeleInd:
    case FunCode::UploadSOE:
    case FunCode::UploadTelemetry:
        break;

    default:
    {
        auto errFrame = FrameBuilder::MakeErrorFrame(fun, ErrorCode::UnknownFun);
        send_all(client, errFrame);
        break;
    }
    }
}

// ==================== 03H 召唤遥测 ====================

void PempServer::handle_call_telemetry(const std::vector<uint8_t>& frame,
                                       socket& client)
{
    if (FrameParser::GetDataLen(frame) < 2) {
        auto err = FrameBuilder::MakeErrorFrame(FunCode::CallTelemetry,
                                                 ErrorCode::BadCommand);
        send_all(client, err);
        return;
    }
    uint8_t ch  = FrameParser::ReadByte(frame, 0);
    uint8_t dev = FrameParser::ReadByte(frame, 1);

    auto& mgr = RemoteDataMgr::Instance();
    std::vector<AiPoint> aiList;
    if (!mgr.GetAiList(ch, dev, aiList))
    {
        auto err = FrameBuilder::MakeErrorFrame(FunCode::CallTelemetry,
                                                 ErrorCode::DeviceError);
        send_all(client, err);
        return;
    }

    size_t count = aiList.size();
    std::vector<int32_t> values(count);
    for (size_t i = 0; i < count; i++)
        values[i] = QuantizeAiToInt32Warn(ch, dev, aiList[i].pointNo,
                                          aiList[i].value);   // H3

    auto resp = FrameBuilder::MakeTelemetryFrame(
        FunCode::CallTelemetry, ch, dev, values.data(),
        static_cast<uint16_t>(count));

    send_all(client, resp);
}

// ==================== 04H 召唤历史 SOE ====================

void PempServer::handle_call_history_soe(const std::vector<uint8_t>& frame,
                                         socket& client)
{
    if (FrameParser::GetDataLen(frame) < 14) {
        auto err = FrameBuilder::MakeErrorFrame(FunCode::CallHistorySOE,
                                                 ErrorCode::BadCommand);
        send_all(client, err);
        return;
    }
    CP56time2a beginTime = FrameParser::ReadCP56(frame, 0);
    CP56time2a endTime   = FrameParser::ReadCP56(frame, 7);

    auto events = g_soeQueue.QueryByTime(beginTime.ToTimeT(), endTime.ToTimeT());

    uint16_t count = static_cast<uint16_t>(
        std::min(events.size(), size_t(65535)));
    auto resp = FrameBuilder::MakeSOEFrame(FunCode::CallHistorySOE,
                                           events.data(), count);
    send_all(client, resp);
}

// ==================== 06H 执行遥控 ====================

void PempServer::handle_exec_remote_ctrl(const std::vector<uint8_t>& frame,
                                         socket& client)
{
    uint16_t dataLen = FrameParser::GetDataLen(frame);
    if (dataLen < 5) {
        auto err = FrameBuilder::MakeErrorFrame(FunCode::ExecRemoteCtrl,
                                                 ErrorCode::BadCommand);
        send_all(client, err);
        return;
    }
    uint8_t  ch      = FrameParser::ReadByte(frame, 0);
    uint8_t  dev     = FrameParser::ReadByte(frame, 1);
    uint16_t pt      = FrameParser::ReadU16(frame, 2);
    uint8_t  pwdLen  = FrameParser::ReadByte(frame, 4);

    // 密码校验（M3 修复）。若 PempServer 未配置密码则跳过（保持老行为）；
    // 否则要求帧内 pwdLen 与 rcPassword_ 长度一致且字节逐个匹配。
    // 帧内密码从 offset 5 开始；dataLen 必须够容纳这些字节。
    if (!rcPassword_.empty()) {
        bool pwdOk = false;
        if (pwdLen == rcPassword_.size() &&
            static_cast<size_t>(dataLen) >= 5u + static_cast<size_t>(pwdLen)) {
            pwdOk = true;
            for (size_t i = 0; i < pwdLen; i++) {
                if (FrameParser::ReadByte(frame, 5 + i) !=
                    static_cast<uint8_t>(rcPassword_[i])) { pwdOk = false; break; }
            }
        }
        if (!pwdOk) {
            FrameBuilder fb;
            fb.Begin(FunCode::ExecRemoteCtrl);
            fb.Append(ch);
            fb.Append(dev);
            fb.AppendU16(pt);
            fb.Append(static_cast<uint8_t>(CtrlResult::Failed));
            auto resp = fb.End();
            send_all(client, resp);
            return;
        }
    }
    (void)pwdLen;  // 密码未配置时保留老行为

    uint16_t doPt = pt;
    auto& mgr = RemoteDataMgr::Instance();

    // 检查设备通讯状态（DI 点号 1 由 master 模块管理：ON=在线 OFF=离线）
    {
        DiPoint commPt;
        if (mgr.GetDi(ch, dev, 1, commPt) && !commPt.value)
        {
            FrameBuilder fb;
            fb.Begin(FunCode::ExecRemoteCtrl);
            fb.Append(ch);
            fb.Append(dev);
            fb.AppendU16(pt);
            fb.Append(static_cast<uint8_t>(CtrlResult::Failed));
            auto resp = fb.End();
            send_all(client, resp);
            return;
        }
    }

    bool ok = mgr.SetDoMaster(ch, dev, doPt, true);

    FrameBuilder fb;
    fb.Begin(FunCode::ExecRemoteCtrl);
    fb.Append(ch);
    fb.Append(dev);
    fb.AppendU16(pt);
    fb.Append(ok ? static_cast<uint8_t>(CtrlResult::Success)
                 : static_cast<uint8_t>(CtrlResult::Failed));
    auto resp = fb.End();
    send_all(client, resp);
}

// ==================== 08H 同步时钟 ====================

void PempServer::handle_sync_clock(const std::vector<uint8_t>& frame,
                                   socket& client)
{
    uint16_t dataLen = FrameParser::GetDataLen(frame);
    if (dataLen < 7) {
        auto err = FrameBuilder::MakeErrorFrame(FunCode::SyncClock,
                                                 ErrorCode::BadCommand);
        send_all(client, err);
        return;
    }

    CP56time2a remoteTime = FrameParser::ReadCP56(frame, 0);
    (void)remoteTime;

    auto resp = FrameBuilder::Make1Byte(FunCode::SyncClock, 0xFF);
    send_all(client, resp);
}

// ==================== 09H 召唤遥信 ====================

void PempServer::handle_call_tele_ind(const std::vector<uint8_t>& frame,
                                      socket& client)
{
    uint16_t dataLen = FrameParser::GetDataLen(frame);
    if (dataLen < 2) {
        auto err = FrameBuilder::MakeErrorFrame(FunCode::CallTeleInd,
                                                 ErrorCode::BadCommand);
        send_all(client, err);
        return;
    }
    uint8_t ch  = FrameParser::ReadByte(frame, 0);
    uint8_t dev = FrameParser::ReadByte(frame, 1);

    auto& mgr = RemoteDataMgr::Instance();
    std::vector<DiPoint> diList;
    if (!mgr.GetDiList(ch, dev, diList))
    {
        auto err = FrameBuilder::MakeErrorFrame(FunCode::CallTeleInd,
                                                 ErrorCode::DeviceError);
        send_all(client, err);
        return;
    }

    // 打包遥信：8 个 DI 点 / 字节
    // bit0 = point1, bit1 = point2, ..., bit7 = point8
    uint16_t totalPoints = static_cast<uint16_t>(diList.size());
    uint16_t byteCount   = static_cast<uint16_t>((totalPoints + 7) / 8);

    std::vector<uint8_t> packedBytes(byteCount, 0);
    for (uint16_t i = 0; i < totalPoints; i++)
    {
        if (diList[i].value)
        {
            uint16_t byteIdx = i / 8;
            uint8_t  bitIdx  = static_cast<uint8_t>(i % 8);
            packedBytes[byteIdx] |= static_cast<uint8_t>(1 << bitIdx);
        }
    }

    auto resp = FrameBuilder::MakeTeleIndFrame(
        FunCode::CallTeleInd, ch, dev, totalPoints,
        packedBytes.data(), byteCount);

    send_all(client, resp);
}

// ==================== 主动上传：51H 遥信 ====================

// H2 修复：PEMP 帧内 CH/DEV 只有 1 字节，规约上限 255。若配置里 chId/devNo
// 超过 255，静默 & 0xFF 截断会把不同通道打包成同一字节，客户端无法区分。
// 这个 helper 检查并 log 一次警告，返回 true 表示可以安全下发。
// 见 CLAUDE.md「已知陷阱 / 修复历史」H2 (code review)。
static bool CheckPempIds8Bit(uint16_t chId, uint16_t devNo)
{
    if (chId > 255 || devNo > 255) {
        std::cerr << "[PempServer] 跳过上传：chId=" << chId
                  << " devNo=" << devNo
                  << " 超出 PEMP 规约 8 位限制（1..255），"
                  << "请缩小点表 ID 或改用 IEC104" << std::endl;
        return false;
    }
    return true;
}

// H3 修复：PEMP 遥测帧 (53H / 03H) 的每个 AI 值是 int32。RemoteDataMgr
// 里 AI 存 double，直接 static_cast<int32_t>(v) 会：
//   1) 静默截去 fractional part（例：3.14 → 3）
//   2) 对 |v| > 2^31 - 1 undefined behavior，MSVC/g++ 上表现为回绕
// 修复策略：仍按规约用 int32 编码，但对每个"有损"点位 log 一次警告，让
// 运维知道需要在模板里加 scale/offset 或换协议。此 helper 也做 clamp 到
// int32 边界避免 UB。
// 见 CLAUDE.md「已知陷阱 / 修复历史」H3 (code review)。
static int32_t QuantizeAiToInt32Warn(uint16_t chId, uint16_t devNo,
                                     uint16_t pt, double v)
{
    // 一次性警告，每个 (ch,dev,pt) 组合只警告一次；避免高频轮询刷屏
    static std::mutex warnedMtx;
    static std::set<uint64_t> warned;

    auto emitOnce = [&](const char* reason) {
        uint64_t key = (static_cast<uint64_t>(chId) << 32) |
                       (static_cast<uint32_t>(devNo) << 16) |
                       pt;
        std::lock_guard<std::mutex> lk(warnedMtx);
        if (warned.insert(key).second) {
            std::cerr << "[PempServer] AI 上传精度警告 ch=" << chId
                      << " dev=" << devNo << " pt=" << pt
                      << " value=" << v << ": " << reason
                      << "（考虑在模板里 scale/offset 归一到 int32 范围）"
                      << std::endl;
        }
    };

    const double kInt32Max = 2147483647.0;   // 2^31 - 1
    const double kInt32Min = -2147483648.0;  // -2^31
    if (v > kInt32Max) { emitOnce("超出 int32 上限，已 clamp"); return INT32_MAX; }
    if (v < kInt32Min) { emitOnce("超出 int32 下限，已 clamp"); return INT32_MIN; }
    // 检查是否有 fractional part
    if (v != static_cast<double>(static_cast<int64_t>(v)))
        emitOnce("有小数部分被截断");
    return static_cast<int32_t>(v);
}

bool PempServer::do_upload_di(socket& client)
{
    auto& mgr = RemoteDataMgr::Instance();
    std::vector<uint16_t> chIds = mgr.GetChannelIds();
    for (uint16_t chId : chIds)
    {
        std::vector<uint16_t> devIds = mgr.GetDeviceIds(chId);
        for (uint16_t devNo : devIds)
        {
            if (!g_running) return false;
            if (!client.is_open()) return false;
            if (!CheckPempIds8Bit(chId, devNo)) continue;   // H2

            std::vector<DiPoint> diList;
            if (!mgr.GetDiList(chId, devNo, diList) || diList.empty())
                continue;

            uint16_t totalPoints = static_cast<uint16_t>(diList.size());
            uint16_t byteCount   = static_cast<uint16_t>((totalPoints + 7) / 8);

            std::vector<uint8_t> packedBytes(byteCount, 0);
            for (uint16_t i = 0; i < totalPoints; i++)
            {
                if (diList[i].value)
                {
                    uint16_t byteIdx = i / 8;
                    uint8_t  bitIdx  = static_cast<uint8_t>(i % 8);
                    packedBytes[byteIdx] |= static_cast<uint8_t>(1 << bitIdx);
                }
            }

            auto frame = FrameBuilder::MakeTeleIndFrame(
                FunCode::UploadTeleInd,
                static_cast<uint8_t>(chId & 0xFF),
                static_cast<uint8_t>(devNo & 0xFF),
                totalPoints, packedBytes.data(), byteCount);

            if (!send_all(client, frame))
                return false;
        }
    }
    return true;
}

// ==================== 主动上传：53H 遥测 ====================

bool PempServer::do_upload_ai(socket& client)
{
    auto& mgr = RemoteDataMgr::Instance();
    std::vector<uint16_t> chIds = mgr.GetChannelIds();
    for (uint16_t chId : chIds)
    {
        std::vector<uint16_t> devIds = mgr.GetDeviceIds(chId);
        for (uint16_t devNo : devIds)
        {
            if (!g_running) return false;
            if (!client.is_open()) return false;
            if (!CheckPempIds8Bit(chId, devNo)) continue;   // H2

            std::vector<AiPoint> aiList;
            if (!mgr.GetAiList(chId, devNo, aiList) || aiList.empty())
                continue;

            size_t count = aiList.size();
            std::vector<int32_t> values(count);
            for (size_t i = 0; i < count; i++)
                values[i] = QuantizeAiToInt32Warn(chId, devNo, aiList[i].pointNo,
                                                  aiList[i].value);   // H3

            auto frame = FrameBuilder::MakeTelemetryFrame(
                FunCode::UploadTelemetry,
                static_cast<uint8_t>(chId & 0xFF),
                static_cast<uint8_t>(devNo & 0xFF),
                values.data(), static_cast<uint16_t>(count));

            if (!send_all(client, frame))
                return false;
        }
    }
    return true;
}

// ==================== 主动上传：52H SOE ====================

bool PempServer::do_upload_soe(socket& client)
{
    if (g_soeQueue.PendingCount() == 0)
        return false;

    auto events = g_soeQueue.PopAll();
    uint16_t count = static_cast<uint16_t>(
        std::min(events.size(), size_t(65535)));
    auto frame = FrameBuilder::MakeSOEFrame(
        FunCode::UploadSOE, events.data(), count);
    // H1 修复：发送失败时把 events 回填到队列头，避免 SOE 静默丢失。
    // send_all 抛 socket_error 也算失败；用 try 包起来。
    bool sent = false;
    try {
        sent = send_all(client, frame);
    } catch (const std::exception& e) {
        std::cerr << "[PempServer] SOE 发送异常: " << e.what() << std::endl;
        sent = false;
    }
    if (!sent) {
        std::cerr << "[PempServer] SOE 上传失败，已回填 " << events.size()
                  << " 条到队列" << std::endl;
        g_soeQueue.PushFrontBatch(events);
        return false;
    }
    // 若发送成功但因 count 上限只包了前 65535 条，剩余的也应回填以便下次继续
    if (events.size() > 65535) {
        std::vector<SOEEvent> tail(events.begin() + 65535, events.end());
        g_soeQueue.PushFrontBatch(tail);
    }
    return true;
}

// ==================== 发送 / 接收辅助 ====================

bool PempServer::send_all(socket& s, const std::vector<uint8_t>& data)
{
    size_t total = 0;
    while (total < data.size())
    {
        size_t n = s.send(data.data() + total, data.size() - total);
        if (n == 0) return false;
        total += n;
    }
    return true;
}

bool PempServer::recv_exact(socket& s, uint8_t* buf, size_t len)
{
    size_t total = 0;
    while (total < len)
    {
        size_t n = s.recv(buf + total, len - total);
        if (n == 0) return false;
        total += n;
    }
    return true;
}
