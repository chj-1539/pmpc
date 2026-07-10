//=============================================================================
// test_soe_queue.cxx — 线程安全 SOE 事件队列单元测试
// 涵盖: Push / PopAll / QueryByTime / Clear / 多线程安全
//=============================================================================

#include "mini_gtest.h"
#include "soe_queue.h"
#include <thread>
#include <atomic>
#include <vector>

class SOEQueueTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Use a local queue for isolated per-test state
        // (g_soeQueue is the global singleton; we avoid it here for isolation)
    }

    // CP56time2a uses year-2000 (uint8_t), so timestamps must be year >= 2000.
    static constexpr std::time_t BASE_T = 946684800; // 2000-01-01 00:00:00 UTC

    // Helper to build an SOE event
    SOEEvent MakeEvent(uint16_t soeId, std::time_t t, uint8_t status) {
        SOEEvent ev;
        ev.channel   = 1;
        ev.device    = 1;
        ev.soeId     = soeId;
        ev.timestamp = CP56time2a::FromTimeT(BASE_T + t);
        ev.status    = status;
        return ev;
    }

    SOEQueue queue_;
};

// ==================== 基础 Push / PopAll ====================

TEST_F(SOEQueueTest, PushAndPopAll) {
    queue_.Push(MakeEvent(100, 1000, 1));
    EXPECT_EQ(queue_.PendingCount(), 1);

    auto results = queue_.PopAll();
    ASSERT_EQ(results.size(), 1);
    EXPECT_EQ(results[0].channel, 1);
    EXPECT_EQ(results[0].device, 1);
    EXPECT_EQ(results[0].soeId, 100);
    EXPECT_EQ(results[0].status, 1);
}

TEST_F(SOEQueueTest, PopAllEmptiesQueue) {
    queue_.Push(MakeEvent(1, 1000, 1));
    queue_.PopAll();
    EXPECT_EQ(queue_.PendingCount(), 0);
}

TEST_F(SOEQueueTest, PopAllReturnsAllThenEmpty) {
    for (int i = 0; i < 10; i++)
        queue_.Push(MakeEvent(static_cast<uint16_t>(i + 1), i * 100, 1));

    auto result = queue_.PopAll();
    EXPECT_EQ(result.size(), 10);
    EXPECT_EQ(queue_.PendingCount(), 0);
}

// ==================== 顺序保持 ====================

TEST_F(SOEQueueTest, OrderPreserved) {
    for (int i = 0; i < 5; i++)
        queue_.Push(MakeEvent(static_cast<uint16_t>(i + 1), i * 100, 1));

    auto results = queue_.PopAll();
    ASSERT_EQ(results.size(), 5);
    for (int i = 0; i < 5; i++)
        EXPECT_EQ(results[i].soeId, i + 1);
}

// ==================== PendingCount ====================

TEST_F(SOEQueueTest, PendingCount) {
    EXPECT_EQ(queue_.PendingCount(), 0);
    queue_.Push(MakeEvent(1, 0, 1));
    EXPECT_EQ(queue_.PendingCount(), 1);
    queue_.Push(MakeEvent(2, 0, 1));
    EXPECT_EQ(queue_.PendingCount(), 2);
    queue_.PopAll();
    EXPECT_EQ(queue_.PendingCount(), 0);
}

// ==================== QueryByTime ====================

TEST_F(SOEQueueTest, QueryByTime) {
    for (int i = 0; i < 5; i++)
        queue_.Push(MakeEvent(static_cast<uint16_t>(i + 1), i * 100, 1));

    // Query with wide range to verify events exist
    auto results = queue_.QueryByTime(0, 9999999999LL);
    ASSERT_EQ(results.size(), 5);  // All events should be found

    // Now query middle range (absolute time = BASE_T + offset)
    // Loop: i=0→soeId=1,t=0; i=1→soeId=2,t=100; i=2→soeId=3,t=200; ...
    results = queue_.QueryByTime(BASE_T + 50, BASE_T + 250);
    ASSERT_EQ(results.size(), 2);  // t=BASE_T+100 and t=BASE_T+200
    EXPECT_EQ(results[0].soeId, 2);  // i=1: soeId=2, t=100
    EXPECT_EQ(results[1].soeId, 3);  // i=2: soeId=3, t=200
}

TEST_F(SOEQueueTest, QueryByTimeDoesNotRemove) {
    queue_.Push(MakeEvent(1, 500, 1));
    queue_.QueryByTime(0, 1000);
    EXPECT_EQ(queue_.PendingCount(), 1);
}

TEST_F(SOEQueueTest, QueryByTimeNoMatch) {
    queue_.Push(MakeEvent(1, 500, 1));
    auto results = queue_.QueryByTime(0, 100);
    EXPECT_TRUE(results.empty());
}

// ==================== Clear ====================

TEST_F(SOEQueueTest, Clear) {
    for (int i = 0; i < 5; i++)
        queue_.Push(MakeEvent(static_cast<uint16_t>(i), i, 1));

    EXPECT_EQ(queue_.PendingCount(), 5);
    queue_.Clear();
    EXPECT_EQ(queue_.PendingCount(), 0);
    EXPECT_TRUE(queue_.PopAll().empty());
}

TEST_F(SOEQueueTest, ClearEmptyQueue) {
    EXPECT_NO_THROW(queue_.Clear());
}

// ==================== PushSimulated ====================

TEST_F(SOEQueueTest, PushSimulated) {
    queue_.PushSimulated(2, 3, 999, 1);
    EXPECT_EQ(queue_.PendingCount(), 1);

    auto results = queue_.PopAll();
    ASSERT_EQ(results.size(), 1);
    EXPECT_EQ(results[0].channel, 2);
    EXPECT_EQ(results[0].device, 3);
    EXPECT_EQ(results[0].soeId, 999);
    EXPECT_EQ(results[0].status, 1);
}

// ==================== 空队列 ====================

TEST_F(SOEQueueTest, EmptyQueuePopAll) {
    auto results = queue_.PopAll();
    EXPECT_TRUE(results.empty());
}

TEST_F(SOEQueueTest, EmptyQueuePendingCount) {
    EXPECT_EQ(queue_.PendingCount(), 0);
}

TEST_F(SOEQueueTest, EmptyQueueQueryByTime) {
    auto results = queue_.QueryByTime(0, 999999);
    EXPECT_TRUE(results.empty());
}

// ==================== 多线程并发 ====================

TEST_F(SOEQueueTest, ConcurrentPushPop) {
    std::atomic<int> pushCount{0};
    constexpr int PUSHERS = 4;
    constexpr int PER_PUSHER = 500;

    std::vector<std::thread> pushers;
    for (int t = 0; t < PUSHERS; t++) {
        pushers.emplace_back([&, t]() {
            for (int i = 0; i < PER_PUSHER; i++) {
                queue_.Push(MakeEvent(
                    static_cast<uint16_t>(t * PER_PUSHER + i), 0, 1));
                pushCount++;
            }
        });
    }

    // Popper: runs until all pushers finish AND queue is drained
    std::atomic<bool> pushingDone{false};
    std::atomic<int> popCount{0};

    std::thread popper([&]() {
        while (true) {
            auto r = queue_.PopAll();
            popCount += static_cast<int>(r.size());
            if (pushingDone.load() && queue_.PendingCount() == 0)
                break;
            if (r.empty())
                std::this_thread::yield();  // avoid busy-spin
        }
    });

    for (auto& t : pushers) t.join();
    pushingDone = true;
    popper.join();

    EXPECT_EQ(pushCount.load(), PUSHERS * PER_PUSHER);
    EXPECT_EQ(popCount.load(), PUSHERS * PER_PUSHER);
    EXPECT_EQ(queue_.PendingCount(), 0);
}

TEST_F(SOEQueueTest, ConcurrentReadDoesNotDeadlock) {
    // Push some events, then read from multiple threads
    for (int i = 0; i < 100; i++)
        queue_.Push(MakeEvent(static_cast<uint16_t>(i), i, 1));

    std::vector<std::thread> readers;
    for (int t = 0; t < 4; t++) {
        readers.emplace_back([&]() {
            for (int i = 0; i < 50; i++) {
                queue_.PendingCount();
                queue_.QueryByTime(0, 100);
            }
        });
    }

    for (auto& t : readers) t.join();
    EXPECT_EQ(queue_.PendingCount(), 100);
}
