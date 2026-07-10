#ifndef SOE_QUEUE_H
#define SOE_QUEUE_H

//=============================================================================
// soe_queue.h — 线程安全 SOE 事件队列
// 用于通讯管理机存储新产生的 SOE，等待主动上传 (52H) 或历史召唤 (04H)
//=============================================================================

#include "protocol.h"
#include <vector>
#include <mutex>
#include <deque>
#include <ctime>

class SOEQueue {
public:
    SOEQueue() = default;

    /// 添加一条 SOE 事件
    void Push(const SOEEvent& ev);

    /// 取走所有待上传的 SOE（清空待上传队列，返回拷贝）
    /// 用于 52H 主动上传后清空
    std::vector<SOEEvent> PopAll();

    /// 查询指定时间范围内的 SOE（不移除）
    /// 用于 04H 召唤历史 SOE
    std::vector<SOEEvent> QueryByTime(std::time_t begin, std::time_t end) const;

    /// 当前待上传数量
    size_t PendingCount() const;

    /// 清空全部
    void Clear();

    /// 添加一条模拟 SOE（用于测试）
    void PushSimulated(uint8_t ch, uint8_t dev, uint16_t soeId, uint8_t status);

private:
    mutable std::mutex mtx_;
    std::deque<SOEEvent> events_;     // 待上传队列（按插入顺序）
};

/// 全局 SOE 队列实例（供线程共享）
extern SOEQueue g_soeQueue;

#endif // SOE_QUEUE_H
