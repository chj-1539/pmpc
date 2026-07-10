//=============================================================================
// soe_queue.cxx — 线程安全 SOE 事件队列实现
//=============================================================================

#include "soe_queue.h"

SOEQueue g_soeQueue;

void SOEQueue::Push(const SOEEvent& ev)
{
    std::lock_guard<std::mutex> lock(mtx_);
    events_.push_back(ev);
}

std::vector<SOEEvent> SOEQueue::PopAll()
{
    std::lock_guard<std::mutex> lock(mtx_);
    std::vector<SOEEvent> result;
    result.reserve(events_.size());
    for (auto& e : events_)
        result.push_back(std::move(e));
    events_.clear();
    return result;
}

std::vector<SOEEvent> SOEQueue::QueryByTime(std::time_t begin, std::time_t end) const
{
    std::lock_guard<std::mutex> lock(mtx_);
    std::vector<SOEEvent> result;
    for (const auto& e : events_)
    {
        std::time_t et = e.timestamp.ToTimeT();
        if (et >= begin && et <= end)
            result.push_back(e);
    }
    return result;
}

size_t SOEQueue::PendingCount() const
{
    std::lock_guard<std::mutex> lock(mtx_);
    return events_.size();
}

void SOEQueue::Clear()
{
    std::lock_guard<std::mutex> lock(mtx_);
    events_.clear();
}

void SOEQueue::PushSimulated(uint8_t ch, uint8_t dev, uint16_t soeId, uint8_t status)
{
    SOEEvent ev;
    ev.channel   = ch;
    ev.device    = dev;
    ev.soeId     = soeId;
    ev.timestamp = CP56time2a::Now();
    ev.status    = status;
    Push(ev);
}
