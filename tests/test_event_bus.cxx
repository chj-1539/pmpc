//=============================================================================
// test_event_bus.cxx — EventBus 类型安全事件总线单元测试
// 涵盖: 订阅/发布/取消/重入安全/异常安全
//=============================================================================

#include "mini_gtest.h"
#include "event_bus.h"
#include <atomic>
#include <thread>
#include <vector>

// ==================== 基础发布/订阅 ====================

TEST(EventBusTest, SubscribeAndPublish) {
    std::atomic<int> received{0};
    auto token = EventBus::Subscribe<DIChange>([&](const DIChange& e) {
        received = e.value ? 1 : 0;
    });

    EventBus::Publish(DIChange{1, 1, 1, true, 100});
    EXPECT_EQ(received.load(), 1);

    EventBus::Unsubscribe<DIChange>(token);
}

TEST(EventBusTest, PublishMultipleEvents) {
    std::atomic<int> count{0};
    auto token = EventBus::Subscribe<DIChange>([&](const DIChange&) {
        count++;
    });

    EventBus::Publish(DIChange{1, 1, 1, true, 0});
    EventBus::Publish(DIChange{1, 1, 2, false, 0});
    EventBus::Publish(DIChange{1, 1, 3, true, 0});
    EXPECT_EQ(count.load(), 3);

    EventBus::Unsubscribe<DIChange>(token);
}

// ==================== 取消订阅 ====================

TEST(EventBusTest, UnsubscribeStopsEvents) {
    std::atomic<int> count{0};
    auto token = EventBus::Subscribe<DIChange>([&](const DIChange&) { count++; });

    EventBus::Publish(DIChange{1, 1, 1, false, 0});
    EXPECT_EQ(count.load(), 1);

    EventBus::Unsubscribe<DIChange>(token);

    EventBus::Publish(DIChange{1, 1, 1, true, 0});
    EXPECT_EQ(count.load(), 1);  // No increment after unsubscribe
}

// ==================== 多订阅者 ====================

TEST(EventBusTest, MultipleSubscribers) {
    std::atomic<int> count1{0}, count2{0};
    auto t1 = EventBus::Subscribe<AIChange>([&](const AIChange&) { count1++; });
    auto t2 = EventBus::Subscribe<AIChange>([&](const AIChange&) { count2++; });

    EventBus::Publish(AIChange{1, 1, 1, 100.0, 0});
    EXPECT_EQ(count1.load(), 1);
    EXPECT_EQ(count2.load(), 1);

    EventBus::Unsubscribe<AIChange>(t1);
    EventBus::Unsubscribe<AIChange>(t2);
}

// ==================== 不同类型事件 ====================

TEST(EventBusTest, DifferentEventTypes) {
    std::atomic<int> diCount{0}, aiCount{0};
    auto t1 = EventBus::Subscribe<DIChange>([&](const DIChange&) { diCount++; });
    auto t2 = EventBus::Subscribe<AIChange>([&](const AIChange&) { aiCount++; });

    EventBus::Publish(DIChange{1, 1, 1, true, 0});
    EXPECT_EQ(diCount.load(), 1);
    EXPECT_EQ(aiCount.load(), 0);

    EventBus::Publish(AIChange{1, 1, 1, 50.0, 0});
    EXPECT_EQ(diCount.load(), 1);
    EXPECT_EQ(aiCount.load(), 1);

    EventBus::Unsubscribe<DIChange>(t1);
    EventBus::Unsubscribe<AIChange>(t2);
}

// ==================== 全部预定义类型 ====================

TEST(EventBusTest, AllPredefinedEventTypes) {
    std::atomic<int> count{0};
    auto d  = EventBus::Subscribe<DIChange>([&](const DIChange&) { count++; });
    auto a  = EventBus::Subscribe<AIChange>([&](const AIChange&) { count++; });
    auto od = EventBus::Subscribe<DOChange>([&](const DOChange&) { count++; });
    auto oa = EventBus::Subscribe<AOChange>([&](const AOChange&) { count++; });

    EventBus::Publish(DIChange{1, 1, 1, true, 0});
    EventBus::Publish(AIChange{1, 1, 1, 1.0, 0});
    EventBus::Publish(DOChange{1, 1, 1, true, false, 0});
    EventBus::Publish(AOChange{1, 1, 1, 1.0, 0});

    EXPECT_EQ(count.load(), 4);

    EventBus::Unsubscribe<DIChange>(d);
    EventBus::Unsubscribe<AIChange>(a);
    EventBus::Unsubscribe<DOChange>(od);
    EventBus::Unsubscribe<AOChange>(oa);
}

// ==================== 重入安全 ====================

TEST(EventBusTest, SubscribeInsideHandler) {
    std::atomic<int> innerCount{0};
    size_t innerToken = 0;

    auto outerToken = EventBus::Subscribe<DIChange>([&](const DIChange&) {
        innerToken = EventBus::Subscribe<AIChange>([&](const AIChange&) {
            innerCount++;
        });
    });

    EventBus::Publish(DIChange{1, 1, 1, true, 0});
    ASSERT_NE(innerToken, 0u);

    // The inner subscription should now be active
    EventBus::Publish(AIChange{1, 1, 1, 1.0, 0});
    EXPECT_EQ(innerCount.load(), 1);

    EventBus::Unsubscribe<AIChange>(innerToken);
    EventBus::Unsubscribe<DIChange>(outerToken);
}

TEST(EventBusTest, UnsubscribeInsideHandler) {
    // Unsubscribing current handler inside itself should not crash
    size_t selfToken = 0;
    std::atomic<int> count{0};

    selfToken = EventBus::Subscribe<DIChange>([&](const DIChange&) {
        count++;
        EventBus::Unsubscribe<DIChange>(selfToken);
    });

    EventBus::Publish(DIChange{1, 1, 1, true, 0});
    EXPECT_EQ(count.load(), 1);

    // Second publish should not fire (handler was unsubscribed)
    EventBus::Publish(DIChange{1, 1, 1, true, 0});
    EXPECT_EQ(count.load(), 1);
}

// ==================== 异常安全 ====================

TEST(EventBusTest, ExceptionInHandlerDoesNotAffectOthers) {
    std::atomic<int> goodCount{0};
    auto t1 = EventBus::Subscribe<DOChange>([&](const DOChange&) {
        throw std::runtime_error("test exception");
    });
    auto t2 = EventBus::Subscribe<DOChange>([&](const DOChange&) {
        goodCount++;
    });

    EXPECT_NO_THROW({
        EventBus::Publish(DOChange{1, 1, 1, true, false, 0});
    });

    EXPECT_EQ(goodCount.load(), 1);

    EventBus::Unsubscribe<DOChange>(t1);
    EventBus::Unsubscribe<DOChange>(t2);
}

// ==================== 无订阅者 ====================

TEST(EventBusTest, PublishWithNoSubscribersNoOp) {
    EXPECT_NO_THROW({
        EventBus::Publish(DIChange{1, 1, 1, true, 0});
    });
}

// ==================== 多线程并发 ====================

TEST(EventBusTest, ConcurrentPublish) {
    std::atomic<int> count{0};
    auto token = EventBus::Subscribe<DIChange>([&](const DIChange&) {
        count++;
    });

    constexpr int THREADS = 4;
    constexpr int PER_THREAD = 1000;
    std::vector<std::thread> threads;

    for (int t = 0; t < THREADS; t++) {
        threads.emplace_back([&]() {
            for (int i = 0; i < PER_THREAD; i++) {
                EventBus::Publish(DIChange{1, 1, 1, true, 0});
            }
        });
    }

    for (auto& t : threads) t.join();
    EXPECT_EQ(count.load(), THREADS * PER_THREAD);

    EventBus::Unsubscribe<DIChange>(token);
}

TEST(EventBusTest, ConcurrentSubscribeUnsubscribe) {
    std::atomic<int> count{0};
    constexpr int ITERATIONS = 500;
    std::vector<size_t> tokens;

    // Subscribe once
    auto mainToken = EventBus::Subscribe<DIChange>([&](const DIChange&) {
        count++;
    });

    // Spawn threads that rapidly subscribe/unsubscribe
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; t++) {
        threads.emplace_back([&]() {
            for (int i = 0; i < ITERATIONS; i++) {
                auto tok = EventBus::Subscribe<AIChange>([](const AIChange&) {});
                EventBus::Unsubscribe<AIChange>(tok);
            }
        });
    }

    // While they're running, publish events
    for (int i = 0; i < 100; i++) {
        EventBus::Publish(DIChange{1, 1, 1, true, 0});
    }

    for (auto& t : threads) t.join();
    EXPECT_GT(count.load(), 0);

    EventBus::Unsubscribe<DIChange>(mainToken);
}
