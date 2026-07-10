#ifndef REDUNDANCY_H
#define REDUNDANCY_H

//=============================================================================
// redundancy.h — 双机冗余管理器
//
// 功能：心跳检测 + 角色切换 + 数据同步 + DO脉冲复位
//   - Standby 模式：不启动采集，仅接收 DI/AI/AO 同步（不接收 DO）
//   - Master 模式：下发 DO 后自动脉冲复位
//=============================================================================

#include "pmpc.h"
#include "socket.h"
#include <string>
#include <atomic>
#include <thread>
#include <functional>
#include <map>
#include <memory>
#include <cstdint>
#include <deque>

enum class RedundRole : uint8_t {
    Idle    = 0,
    Standby = 0x01,
    Master  = 0x03,
};

inline const char* RoleName(RedundRole r) {
    switch (r) {
        case RedundRole::Master:  return "Master";
        case RedundRole::Standby: return "Standby";
        case RedundRole::Idle:    return "Idle";
        default: return "Unknown";
    }
}

struct ChangeEvent {
    enum Type : uint8_t {
        DI_CHANGE = 1, AI_CHANGE = 2,
        DO_MASTER_CHANGE = 3, DO_SLAVE_CHANGE = 4, AO_CHANGE = 5,
    };
    uint8_t  type;
    uint16_t channel;
    uint16_t device;
    uint16_t point;
    uint64_t value;
    double   dvalue;
    uint64_t tsMs;
    std::string ToString() const;
};

constexpr uint8_t  HB_START = 0x7B;
constexpr uint8_t  HB_FUN   = 0x55;
constexpr uint8_t  HB_END   = 0x7D;
constexpr uint8_t  SYNC_START = 0x7B;
constexpr uint8_t  SYNC_FUN   = 0x56;
constexpr uint8_t  SYNC_END   = 0x7D;

class SyncChannel {
public:
    SyncChannel() = default;
    ~SyncChannel() { Stop(); }
    bool StartMaster(uint16_t port);
    bool StartStandby(const std::string& host, uint16_t port);
    void Stop();
    bool SendChange(const ChangeEvent& ev);
    using ChangeCallback = std::function<void(const ChangeEvent&)>;
    void SetChangeCallback(ChangeCallback cb) { callback_ = cb; }
private:
    void AcceptLoop();
    void RecvLoop(socket s, socket_addr peer);
    std::vector<uint8_t> BuildSyncFrame(const ChangeEvent& ev);
    bool ParseSyncFrame(const uint8_t* data, size_t len, ChangeEvent& ev);
    uint16_t port_ = 7502;
    std::string peerHost_;
    std::atomic<bool> running_{false};
    socket listenSock_;
    socket clientSock_;
    std::mutex clientMtx_;
    std::thread acceptThr_;
    socket syncSock_;
    std::thread recvThr_;
    ChangeCallback callback_;
};

class RedundancyManager {
public:
    RedundancyManager();
    ~RedundancyManager();
    bool LoadConfig(const std::string& cfgPath);
    bool Start();
    void Stop();
    bool IsRunning() const { return running_; }
    RedundRole GetRole() const { return role_; }
    const std::string& GetLocalName() const { return localName_; }
    bool IsPeerAlive() const { return peerAlive_; }
    bool IsMaster()  const { return role_ == RedundRole::Master; }
    bool IsStandby() const { return role_ == RedundRole::Standby; }

    using RoleCallback = std::function<void(RedundRole oldRole, RedundRole newRole)>;
    void OnRoleChanged(RoleCallback cb) { roleCb_ = cb; }

    /// 注册采集启停控制回调: Standby→停止, Master→启动
    using CollectControl = std::function<void(bool start)>;
    void SetCollectControl(CollectControl cb) { collectCb_ = cb; }

    void OnSyncData(ChangeEvent::Type type, uint16_t ch, uint16_t dev,
                    uint16_t pt, uint64_t val, double dval);

    /// 调试接口：手动请求角色切换（由调试控制台调用）
    void RequestRoleChange(RedundRole newRole);
private:
    void HeartbeatLoop();
    void ListenLoop();
    void SetRole(RedundRole newRole);
    void CheckFailover();
    bool ParseConfigLine(const std::string& key, const std::string& val);

    std::string localName_ = "box_a";
    std::string peerIp_ = "127.0.0.1";
    uint16_t heartbeatPort_ = 7503;
    uint16_t syncPort_ = 7502;
    int heartbeatIntervalMs_ = 1000;
    int missedHeartbeatLimit_ = 5;
    bool autoFailback_ = true;
    int startupDelayMs_ = 3000;
    int priority_ = 100;

    std::atomic<bool> running_{false};
    std::atomic<bool> peerAlive_{false};
    std::atomic<RedundRole> role_{RedundRole::Idle};
    std::atomic<RedundRole> peerRole_{RedundRole::Idle};  ///< 对端当前角色（从心跳帧解析）
    uint16_t peerPriority_{0};          ///< 对端优先级（从心跳帧解析）
    RedundRole lastRole_ = RedundRole::Idle;
    int missedHeartbeats_ = 0;
    uint64_t lastHbTime_ = 0;
    uint64_t startupCompleteTime_ = 0;
    socket hbListenSock_;
    socket hbSendSock_;
    std::mutex hbMtx_;          ///< 保护 hbSendSock_ 的并发访问
    std::thread hbSendThr_;
    std::thread hbListenThr_;
    SyncChannel syncChannel_;
    RoleCallback roleCb_;
    CollectControl collectCb_;  // 采集控制回调
};

#endif // REDUNDANCY_H
