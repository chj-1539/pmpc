//=============================================================================
// pmpc_data_mgr.cxx — 四遥数据管理器实现
// 包含：辅助函数、Get/Set 接口、CheckAllPointChange
//
// 锁策略（每设备独立锁 + 结构锁双层保护）：
//   structMtx_      — 仅保护 channels_ 向量结构，查找渠道/设备时短暂持有
//   Device::devMtx  — 保护本设备所有点位数据，Get/Set 期间持有
//   WithDeviceLocked 同时持两锁，安全返回设备引用后再操作
// 效果：设备 1 的读写完全不影响设备 2，即使在同一通道内也是真正并行。
//=============================================================================

#include "pmpc.h"
#include "event_bus.h"
#include "str_util.h"
#include <iostream>
#include <sstream>
#include <algorithm>

// ==================== 单例 ====================

RemoteDataMgr& RemoteDataMgr::Instance()
{
    static RemoteDataMgr inst;
    return inst;
}

// ==================== 内部辅助函数 ====================

uint16_t RemoteDataMgr::ParseChannelId(const std::string& sec)
{
    size_t pos = sec.find('_');
    if (pos == std::string::npos)
        return 0;

    try {
        int id = std::stoi(sec.substr(pos + 1));
        if (id <= 0 || id > 65535)
            return 0;
        return static_cast<uint16_t>(id);
    }
    catch (...) {
        return 0;
    }
}

uint16_t RemoteDataMgr::ParseDevNo(const std::string& key)
{
    size_t pos = key.find('_');
    if (pos == std::string::npos)
        return 0;

    try {
        int no = std::stoi(key.substr(pos + 1));
        if (no <= 0 || no > 65535)
            return 0;
        return static_cast<uint16_t>(no);
    }
    catch (...) {
        return 0;
    }
}

Channel* RemoteDataMgr::FindCh(uint16_t chId)
{
    for (auto& ch : channels_)
        if (ch.chId == chId)
            return &ch;
    return nullptr;
}

size_t RemoteDataMgr::FindChIdx(uint16_t chId) const
{
    for (size_t i = 0; i < channels_.size(); i++)
        if (channels_[i].chId == chId)
            return i;
    return SIZE_MAX;
}

Device* RemoteDataMgr::FindDev(Channel* ch, uint16_t devNo)
{
    if (!ch) return nullptr;
    for (auto& dev : ch->devList)
        if (dev.devNo == devNo)
            return &dev;
    return nullptr;
}

void RemoteDataMgr::ClearAll()
{
    // 由 LoadConfig 调用，调用者已持有 structMtx_ 锁。
    // 各设备 devMtx 此时无竞争（子线程尚未启动），无需额外加锁。
    channels_.clear();
}

// ==================== 遍历查询（供 TCP Server 等模块使用） ====================

std::vector<uint16_t> RemoteDataMgr::GetChannelIds()
{
    std::lock_guard<std::mutex> lock(structMtx_);
    std::vector<uint16_t> ids;
    ids.reserve(channels_.size());
    for (const auto& ch : channels_)
        ids.push_back(ch.chId);
    return ids;
}

std::vector<uint16_t> RemoteDataMgr::GetDeviceIds(uint16_t ch)
{
    std::lock_guard<std::mutex> lock(structMtx_);
    Channel* pCh = FindCh(ch);
    if (!pCh) return {};
    std::vector<uint16_t> ids;
    ids.reserve(pCh->devList.size());
    for (const auto& dev : pCh->devList)
        ids.push_back(dev.devNo);
    return ids;
}

bool RemoteDataMgr::GetDiList(uint16_t ch, uint16_t dev, std::vector<DiPoint>& out)
{
    return WithDeviceLocked(ch, dev, [&](Device& d) -> bool {
        out = d.diList;
        return true;
    });
}

bool RemoteDataMgr::GetAiList(uint16_t ch, uint16_t dev, std::vector<AiPoint>& out)
{
    return WithDeviceLocked(ch, dev, [&](Device& d) -> bool {
        out = d.aiList;
        return true;
    });
}

bool RemoteDataMgr::GetDoList(uint16_t ch, uint16_t dev, std::vector<DoPoint>& out)
{
    return WithDeviceLocked(ch, dev, [&](Device& d) -> bool {
        out = d.doList;
        return true;
    });
}

bool RemoteDataMgr::GetAoList(uint16_t ch, uint16_t dev, std::vector<AoPoint>& out)
{
    return WithDeviceLocked(ch, dev, [&](Device& d) -> bool {
        out = d.aoList;
        return true;
    });
}

// ==================== 写接口（每设备独立锁） ====================

bool RemoteDataMgr::SetDi(uint16_t ch, uint16_t dev, uint16_t pt,
                          bool val, uint64_t ts, bool /*change*/)
{
    bool shouldPublish = false;
    DIChange ev{};
    bool found = WithDeviceLocked(ch, dev, [&](Device& pDev) -> bool {
        for (auto& ptRef : pDev.diList)
        {
            if (ptRef.pointNo == pt)
            {
                bool oldVal = ptRef.value;
                ptRef.value = val;
                ptRef.tsMs  = ts;
                if (pt != 1 && oldVal != val) {
                    ev = DIChange{ch, dev, pt, val, ts};
                    shouldPublish = true;
                }
                return true;
            }
        }
        return false;
    });
    // 在 devMtx 之外发布事件，避免 EventBus handler 反向持锁导致 ABBA 死锁
    if (shouldPublish) EventBus::Publish(ev);
    return found;
}

bool RemoteDataMgr::SetAi(uint16_t ch, uint16_t dev,
                          uint16_t pt, double val)
{
    bool shouldPublish = false;
    AIChange ev{};
    bool found = WithDeviceLocked(ch, dev, [&](Device& pDev) -> bool {
        for (auto& ap : pDev.aiList)
        {
            if (ap.pointNo == pt)
            {
                ap.value = val;
                uint64_t n = NowMs();
                ev = AIChange{ch, dev, pt, val, n};
                shouldPublish = true;
                return true;
            }
        }
        return false;
    });
    if (shouldPublish) EventBus::Publish(ev);
    return found;
}

bool RemoteDataMgr::SetDoMaster(uint16_t ch, uint16_t dev,
                                uint16_t pt, bool val)
{
    bool wasZero = false;
    bool shouldPublish = false;
    DOChange ev{};
    bool found = WithDeviceLocked(ch, dev, [&](Device& pDev) -> bool {
        for (auto& dpt : pDev.doList)
        {
            if (dpt.pointNo == pt)
            {
                wasZero = !dpt.masterVal;
                dpt.masterVal = val;
                uint64_t n = NowMs();
                ev = DOChange{ch, dev, pt, val, dpt.slaveVal, n};
                shouldPublish = true;
                return true;
            }
        }
        return false;
    });
    if (shouldPublish) EventBus::Publish(ev);

    // devMtx 已释放，0→1 跃变入脉冲队列，到期自动复位
    if (found && wasZero && val)
    {
        uint64_t now = NowMs();
        std::lock_guard<std::mutex> lock(pulseMtx_);
        pulseQueue_.push_back({ch, dev, pt, now});
    }

    return found;
}

bool RemoteDataMgr::SetDoSlave(uint16_t ch, uint16_t dev,
                               uint16_t pt, bool val)
{
    bool shouldPublish = false;
    DOChange ev{};
    bool found = WithDeviceLocked(ch, dev, [&](Device& pDev) -> bool {
        for (auto& dpt : pDev.doList)
        {
            if (dpt.pointNo == pt)
            {
                dpt.slaveVal = val;
                uint64_t n = NowMs();
                ev = DOChange{ch, dev, pt, dpt.masterVal, val, n};
                shouldPublish = true;
                return true;
            }
        }
        return false;
    });
    if (shouldPublish) EventBus::Publish(ev);
    return found;
}

bool RemoteDataMgr::SetAo(uint16_t ch, uint16_t dev,
                          uint16_t pt, double val)
{
    bool shouldPublish = false;
    AOChange ev{};
    bool found = WithDeviceLocked(ch, dev, [&](Device& pDev) -> bool {
        for (auto& ap : pDev.aoList)
        {
            if (ap.pointNo == pt)
            {
                ap.value = val;
                uint64_t n = NowMs();
                ev = AOChange{ch, dev, pt, val, n};
                shouldPublish = true;
                return true;
            }
        }
        return false;
    });

    if (shouldPublish) EventBus::Publish(ev);

    // devMtx 已释放，AO 变化入列（仅追踪，不复位）
    if (found)
    {
        std::lock_guard<std::mutex> lock(aoQueueMtx_);
        aoChangeQueue_.push_back({ch, dev, pt, ev.value, ev.tsMs});
        if (aoChangeQueue_.size() > 10000)
            aoChangeQueue_.pop_front();
    }

    return found;
}

// ==================== 读接口（每设备独立锁） ====================

bool RemoteDataMgr::GetDi(uint16_t ch, uint16_t dev, uint16_t pt, DiPoint& out)
{
    return WithDeviceLocked(ch, dev, [&](Device& pDev) -> bool {
        for (auto& ptRef : pDev.diList)
            if (ptRef.pointNo == pt) { out = ptRef; return true; }
        return false;
    });
}

bool RemoteDataMgr::GetAi(uint16_t ch, uint16_t dev, uint16_t pt, AiPoint& out)
{
    return WithDeviceLocked(ch, dev, [&](Device& pDev) -> bool {
        for (auto& ap : pDev.aiList)
            if (ap.pointNo == pt) { out = ap; return true; }
        return false;
    });
}

bool RemoteDataMgr::GetDo(uint16_t ch, uint16_t dev, uint16_t pt, DoPoint& out)
{
    return WithDeviceLocked(ch, dev, [&](Device& pDev) -> bool {
        for (auto& dpt : pDev.doList)
            if (dpt.pointNo == pt) { out = dpt; return true; }
        return false;
    });
}

bool RemoteDataMgr::GetAo(uint16_t ch, uint16_t dev, uint16_t pt, AoPoint& out)
{
    return WithDeviceLocked(ch, dev, [&](Device& pDev) -> bool {
        for (auto& ap : pDev.aoList)
            if (ap.pointNo == pt) { out = ap; return true; }
        return false;
    });
}

// ==================== CheckAllPointChange（安全快照模式） ====================

void RemoteDataMgr::CheckAllPointChange()
{
    uint64_t nowTs = NowMs();

    // 取 (chId, devNo) 快照而非 Device* 指针，避免 LoadConfig 热重载时悬空
    struct DevSnap { uint16_t chId; uint16_t devNo; };
    std::vector<DevSnap> snap;
    {
        std::lock_guard<std::mutex> lock(structMtx_);
        for (auto& ch : channels_)
            for (auto& dev : ch.devList)
                snap.push_back({ch.chId, dev.devNo});
    }

    // 通过 WithDeviceLocked 安全遍历（structMtx_ + devMtx 双重保护）
    for (auto& s : snap)
    {
        WithDeviceLocked(s.chId, s.devNo, [&](Device& dev) -> bool {

            // -- DI --
            for (auto& pt : dev.diList)
            {
                if (pt.pointNo == 1) continue;
                if (pt.value != pt.lastVal)
                {
                    std::cout << "[DI变化] 通道:" << s.chId
                              << " 设备:" << s.devNo
                              << " 点位:" << pt.pointNo
                              << " 旧值:" << pt.lastVal
                              << " 新值:" << pt.value
                              << " 时间戳:" << nowTs << std::endl;
                    pt.lastVal = pt.value;
                }
            }

            // -- AI --
            for (auto& pt : dev.aiList)
            {
                if (pt.value != pt.lastVal)
                {
                    std::cout << "[AI更新] 通道:" << s.chId
                              << " 设备:" << s.devNo
                              << " 点位:" << pt.pointNo
                              << " 旧值:" << pt.lastVal
                              << " 新值:" << pt.value
                              << " 时间戳:" << nowTs << std::endl;
                    pt.lastVal = pt.value;
                }
            }

            // -- DO --
            for (auto& pt : dev.doList)
            {
                if (pt.masterVal != pt.lastMaster)
                {
                    std::cout << "[DO主站值变化] 通道:" << s.chId
                              << " 设备:" << s.devNo
                              << " 点位:" << pt.pointNo
                              << " 旧:" << pt.lastMaster
                              << " 新:" << pt.masterVal << std::endl;
                    pt.lastMaster = pt.masterVal;
                }
                if (pt.slaveVal != pt.lastSlave)
                {
                    std::cout << "[DO反馈值变化] 通道:" << s.chId
                              << " 设备:" << s.devNo
                              << " 点位:" << pt.pointNo
                              << " 旧:" << pt.lastSlave
                              << " 新:" << pt.slaveVal << std::endl;
                    pt.lastSlave = pt.slaveVal;
                }
            }

            // -- AO --
            for (auto& pt : dev.aoList)
            {
                if (pt.value != pt.lastVal)
                {
                    std::cout << "[AO调节更新] 通道:" << s.chId
                              << " 设备:" << s.devNo
                              << " 点位:" << pt.pointNo
                              << " 旧:" << pt.lastVal
                              << " 新:" << pt.value << std::endl;
                    pt.lastVal = pt.value;
                }
            }

            return true;
        });
    }

    // DO 脉冲复位检查（到期自动复位）
    CheckPulseQueue();
}

// ==================== DO 脉冲复位检测 ====================

void RemoteDataMgr::CheckPulseQueue()
{
    uint64_t now = NowMs();

    // 批量取出到期条目（FIFO 队首未到期即停止）
    std::deque<DoPulseEntry> expired;
    {
        std::lock_guard<std::mutex> lock(pulseMtx_);
        while (!pulseQueue_.empty())
        {
            if (now - pulseQueue_.front().enqueueMs >= doPulseMs_)
            {
                expired.push_back(std::move(pulseQueue_.front()));
                pulseQueue_.pop_front();
            }
            else
            {
                break;  // FIFO 有序：队首未到期 → 后面都未到期
            }
        }
    }

    // 逐个复位（SetDoMaster 调 false 不会再次入队，因 oldVal=false）
    for (const auto& e : expired)
        SetDoMaster(e.channel, e.device, e.point, false);
}
