#ifndef PEMP_SERVER_H
#define PEMP_SERVER_H

//=============================================================================
// pemp_server.h — PEMP2.0 TCP 通讯服务
//
// 通讯管理机角色 = 服务端，端口 4096
// 为每路连接维护独立工作状态 + 独立副本线程
// 消息循环：接收后台命令 → 处理 → 回复 | 定时主动上传
//=============================================================================

#include "socket.h"
#include "pmpc.h"
#include "protocol.h"
#include "event_bus.h"
#include <atomic>
#include <thread>
#include <chrono>
#include <vector>
#include <mutex>
#include <string>
#include <set>

struct PempBind {
    std::string allowedIP;
    uint16_t port;
};

class PempServer {
public:
    explicit PempServer(uint16_t port = 4096,
                        int diUploadMs = 5000,
                        int aiUploadMs = 5000);
    ~PempServer();

    void setBinds(const std::vector<PempBind>& binds) { binds_ = binds; }

    // 设置遥控密码。空字符串表示不校验（老行为，保持兼容）。
    // 非空时，客户端 06H 帧中携带的密码 bytes 必须逐字节匹配。
    // 见 CLAUDE.md「已知陷阱 / 修复历史」M3。
    void setRemoteCtrlPassword(const std::string& pw) { rcPassword_ = pw; }
    const std::string& remoteCtrlPassword() const { return rcPassword_; }

    bool start();
    void stop();
    bool is_running() const { return running_; }

    // 测试挂钩：供 tests/test_pemp_server_auth.cxx 直接驱动
    // handle_exec_remote_ctrl 而不必开完整 accept 循环。
    friend class PempServerTestAccess;

private:
    void accept_loop(const PempBind& bind, socket& listen_sock);
    void client_handler(socket client, socket_addr peer);

    // ── 命令分发 ──
    void handle_request(const std::vector<uint8_t>& frame,
                        socket& client, uint8_t& workState);

    // ── 主动上传 ──
    bool do_upload_di(socket& client);
    bool do_upload_ai(socket& client);
    bool do_upload_soe(socket& client);

    // ── 命令处理 ──
    void handle_query_status(socket& client);
    void handle_switch_work_state(const std::vector<uint8_t>& frame,
                                  socket& client);
    void handle_call_telemetry(const std::vector<uint8_t>& frame,
                               socket& client);
    void handle_call_history_soe(const std::vector<uint8_t>& frame,
                                 socket& client);
    void handle_exec_remote_ctrl(const std::vector<uint8_t>& frame,
                                 socket& client);
    void handle_sync_clock(const std::vector<uint8_t>& frame,
                           socket& client);
    void handle_call_tele_ind(const std::vector<uint8_t>& frame,
                              socket& client);

    // ── 辅助 ──
    static bool send_all(socket& s, const std::vector<uint8_t>& data);
    static bool recv_exact(socket& s, uint8_t* buf, size_t len);

    uint16_t port_;
    int diUploadMs_;
    int aiUploadMs_;
    std::vector<PempBind> binds_;
    std::string rcPassword_;   ///< 遥控密码；空 = 不校验（M3 修复）
    std::atomic<bool> running_{true};
    std::vector<socket> listenSocks_;
    std::vector<std::thread> acceptThreads_;
    std::vector<std::thread> clientThreads_;
    std::mutex clientMtx_;
    std::vector<std::weak_ptr<bool>> clientLiveFlags_;

    // ── EventBus 订阅（DI → SOE） ──
    size_t diSubToken_ = 0;  ///< 客户端存活标记，停止时关闭
};

#endif // PEMP_SERVER_H
