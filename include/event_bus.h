#ifndef EVENT_BUS_H
#define EVENT_BUS_H

//=============================================================================
// event_bus.h — 类型安全事件总线
//
// 用法:
//   1. 定义事件结构体
//      struct DIChange { uint16_t ch, dev, pt; bool val; uint64_t ts; };
//
//   2. 发布事件（采集模块调）
//      EventBus::Publish(DIChange{1,1,1,true, now});
//
//   3. 订阅事件（TCP推送/同步模块调）
//      auto token = EventBus::Subscribe<DIChange>([](const DIChange& e) {
//          // 处理变化...
//      });
//
//   4. 取消订阅
//      EventBus::Unsubscribe<DIChange>(token);
//=============================================================================

#include <functional>
#include <iostream>
#include <map>
#include <vector>
#include <mutex>
#include <typeindex>
#include <memory>

/// 预定义的四遥事件类型（所有驱动/模块共享）
struct DIChange {
    uint16_t channel;
    uint16_t device;
    uint16_t point;
    bool     value;
    uint64_t tsMs;
};

struct AIChange {
    uint16_t channel;
    uint16_t device;
    uint16_t point;
    double   value;
    uint64_t tsMs;
};

struct DOChange {
    uint16_t channel;
    uint16_t device;
    uint16_t point;
    bool     masterVal;
    bool     slaveVal;
    uint64_t tsMs;
};

struct AOChange {
    uint16_t channel;
    uint16_t device;
    uint16_t point;
    double   value;
    uint64_t tsMs;
};

class EventBus {
public:
    /// 订阅事件 T，返回 token 用于取消订阅
    template<typename T>
    static size_t Subscribe(std::function<void(const T&)> handler) {
        auto wrapper = [handler](const void* ev) { handler(*static_cast<const T*>(ev)); };
        std::lock_guard<std::mutex> lock(Mutex());
        size_t id = NextId();
        Handlers()[std::type_index(typeid(T))].push_back({id, std::move(wrapper)});
        return id;
    }

    /// 取消订阅
    template<typename T>
    static void Unsubscribe(size_t token) {
        std::lock_guard<std::mutex> lock(Mutex());
        auto& vec = Handlers()[std::type_index(typeid(T))];
        vec.erase(std::remove_if(vec.begin(), vec.end(),
                  [token](const Entry& e) { return e.id == token; }), vec.end());
    }

    /// 发布事件（线程安全，不持锁调用 handler，避免重入死锁）
    template<typename T>
    static void Publish(const T& event) {
        std::vector<Entry> handlers;
        {
            std::lock_guard<std::mutex> lock(Mutex());
            auto it = Handlers().find(std::type_index(typeid(T)));
            if (it != Handlers().end())
                handlers = it->second;  // 拷贝，防止 handler 中 Unsubscribe 导致迭代器失效
        }
        for (const auto& entry : handlers) {
            try {
                entry.handler(&event);
            } catch (const std::exception& e) {
                std::cerr << "[EventBus] Handler " << entry.id << " threw: " << e.what() << std::endl;
            } catch (...) {
                std::cerr << "[EventBus] Handler " << entry.id << " threw unknown exception" << std::endl;
            }
        }
    }

private:
    struct Entry {
        size_t id;
        std::function<void(const void*)> handler;
    };
    using HandlerMap = std::map<std::type_index, std::vector<Entry>>;

    static HandlerMap& Handlers() { static HandlerMap m; return m; }
    static std::mutex& Mutex() { static std::mutex m; return m; }
    static size_t& NextId() { static size_t id = 1; return id; }
};

#endif // EVENT_BUS_H
