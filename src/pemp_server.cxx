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
#include <algorithm>

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
    // 分离客户端线程而非 join，避免阻塞等待 recv 超时
    // 客户端线程会在 recv 失败或检测到 running_=false 后自行退出
    {
        std::lock_guard<std::mutex> lock(clientMtx_);
        for (auto& t : clientThreads_)
            if (t.joinable()) t.detach();
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
        values[i] = static_cast<int32_t>(aiList[i].value);

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

    (void)pwdLen;

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

            std::vector<AiPoint> aiList;
            if (!mgr.GetAiList(chId, devNo, aiList) || aiList.empty())
                continue;

            size_t count = aiList.size();
            std::vector<int32_t> values(count);
            for (size_t i = 0; i < count; i++)
                values[i] = static_cast<int32_t>(aiList[i].value);

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
    return send_all(client, frame);
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
