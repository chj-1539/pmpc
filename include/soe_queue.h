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

    /// 把一批事件回填到队列头部，保持它们相互的原有顺序。
    /// 用途：发送失败时 requeue，让 SOE 顺序不被破坏；见 pemp_server::do_upload_soe。
    /// 若队列后续又累积了新事件，这批老事件仍排在最前，下次上传优先处理。
    void PushFrontBatch(const std::vector<SOEEvent>& events);

    /// 取走所有待上传的 SOE（清空待上传队列，返回拷贝）
    /// 用于 52H 主动上传后清空
    std::vector<SOEEvent> PopAll();

    /// 查询指定时间范围内的 SOE（不移除）
    /// 用于 04H 召唤历史 SOE
    std::vector<SOEEvent> QueryByTime(std::time_t begin, std::time_t end) const;

    /// 当前待上传数量
    size_t PendingCount() const;

    /// 设置队列上限。超过上限时 Push 丢弃最老事件（默认 0 = 无限制）。
    /// P2-2（第二轮）：当 PempServer 未连主站时，DI 高频翻转会无限入队，
    /// 长时间运行后耗尽内存。
    void SetMaxSize(size_t max) { maxSize_ = max; }
    size_t GetMaxSize() const { return maxSize_; }

    /// 清空全部
    void Clear();

    /// 添加一条模拟 SOE（用于测试）
    void PushSimulated(uint8_t ch, uint8_t dev, uint16_t soeId, uint8_t status);

private:
    mutable std::mutex mtx_;
    std::deque<SOEEvent> events_;     // 待上传队列（按插入顺序）
    size_t maxSize_ = 0;             // P2-2: 0 = 无限制
};

/// 全局 SOE 队列实例（供线程共享）
extern SOEQueue g_soeQueue;

#endif // SOE_QUEUE_H
