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
#include <mutex>
#include <vector>

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

// ── 心跳 / 同步帧的纯编解码 + 角色决策（对外暴露供单元测试） ──
// 见 tests/test_redundancy_frame_codec.cxx 和 test_redundancy_role_transitions.cxx。
namespace pmpc {
namespace redundancy {

// 心跳帧解析结果
struct HeartbeatInfo {
    RedundRole role     = RedundRole::Idle;
    uint64_t   tsMs     = 0;
    uint16_t   priority = 0;   ///< 修复 C4：0 表示旧格式帧未携带 priority
};

// 心跳帧格式：
//   [START(1) | FUN(1) | LEN(2 little-endian) | ROLE(1) | TS(8) | (PRI(2))? | END(1)]
// 兼容层：
//   老格式：LEN=8 (只覆盖 TS)，总长 14 字节，无 priority；老代码定长 recv 14。
//   新格式：LEN=11 (payload=ROLE+TS+PRI 全部)，总长 16 字节。
// ParseHeartbeatFrame 两种都接受，BuildHeartbeatFrame 只产生新格式。
std::vector<uint8_t> BuildHeartbeatFrame(RedundRole role, uint16_t priority, uint64_t tsMs);
bool                 ParseHeartbeatFrame(const uint8_t* data, size_t len,
                                         HeartbeatInfo& out);

// 同步帧格式：
//   [START(1) | FUN(1) | LEN(2 little-endian) | TYPE(1) | CH(2) | DEV(2) |
//    PT(2) | VALUE(8, IEEE754 double) | TS(8) | END(1)]
// 定长 28 字节，LEN 固定为 23。
std::vector<uint8_t> BuildSyncFrame(const ChangeEvent& ev);
bool                 ParseSyncFrame(const uint8_t* data, size_t len, ChangeEvent& ev);

// 角色决策纯函数：给定当前状态和输入信号，返回下一步应该切到的角色。
// 输入不带自身/远端 io 或时间读，方便表驱动测试；
// SetRole 的副作用（syncChannel_.Start* / roleCb_ / collectCb_）由调用方处理。
//
// 语义（与原 CheckFailover 一致）：
//   ┌─ peerAlive == false（心跳丢失）─────────────────────────────────────
//   │  若 missedHeartbeats >= missedLimit 且 role ∈ {Standby, Idle} → Master
//   │  否则维持 role
//   └─ peerAlive == true ─────────────────────────────────────────────────
//      若 role == Idle：
//        peerRole==Master   → Standby
//        peerRole 其他      → 优先级高的升主；等优先级用 tie-breaker
//                             （字典序小的升主，避免双 Standby 死锁）
//      若 role == Master 且 peerRole == Master：
//        优先级低的降为 Standby；等优先级用 tie-breaker 决定谁降
//      其它情况维持
//
// 修复 C4：CheckFailover 里对 peerPriority 的引用之前没有心跳字段承载，
// 现在这个函数明确接收 peerPriority；调用方从 ParseHeartbeatFrame 拿到值。
// 修复 RD-1（第二轮）：双 Idle 且 priority 相等时旧逻辑双方都 Standby →
// 永不 failover。新增 localTieBreaker/peerTieBreaker 参数（一般传本机名
// 与对端 IP），priority 相等时按字典序解。
RedundRole DecideRole(RedundRole role, RedundRole peerRole, bool peerAlive,
                      int missedHeartbeats, int missedLimit,
                      uint16_t localPriority, uint16_t peerPriority,
                      const std::string& localTieBreaker = "",
                      const std::string& peerTieBreaker = "");

} // namespace redundancy
} // namespace pmpc

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
    // RD-5（第二轮）: peerPriority_ / missedHeartbeats_ 都被 HeartbeatLoop
    // 写 + CheckFailover 读，普通 int/uint16 是数据竞争 UB。改 atomic 让
    // 存取自身对齐+可见性有保证；DecideRole 传值时 .load()。
    std::atomic<uint16_t> peerPriority_{0};
    std::atomic<int> missedHeartbeats_{0};
    RedundRole lastRole_ = RedundRole::Idle;
    uint64_t lastHbTime_ = 0;
    uint64_t startupCompleteTime_ = 0;
    socket hbListenSock_;
    socket hbSendSock_;
    std::mutex hbMtx_;          ///< 保护 hbSendSock_ 的并发访问
    // C5 修复：CheckFailover（心跳线程）和 RequestRoleChange（debug_console
    // 线程）都会调 SetRole；未加锁时可能并发进入并同时启动 syncChannel_，
    // 线程 / socket 泄漏。roleMtx_ 序列化 SetRole。
    std::mutex roleMtx_;
    std::thread hbSendThr_;
    std::thread hbListenThr_;
    SyncChannel syncChannel_;
    RoleCallback roleCb_;
    CollectControl collectCb_;  // 采集控制回调
};

#endif // REDUNDANCY_H
