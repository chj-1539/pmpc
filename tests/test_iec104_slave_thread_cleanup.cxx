//=============================================================================
// test_iec104_slave_thread_cleanup.cxx
//
// 回归 M4 (code review)：Iec104Slave::AcceptLoop 每接 10 个新连接就跑
// 一次 cleanup，试图 erase 已完成的 client 线程。老代码用
//   if (it->joinable()) { ++it; continue; }
// 判断"已完成"——但 std::thread::joinable() 只表示 thread 对象绑定 OS
// 线程，即使那线程 return 之后仍是 true（除非 join 或 detach）。所以
// erase 分支永远走不到，clientThreads_ 只增不减，长期运行会累积到 OS
// 线程句柄耗尽。
//
// 修复：ClientThreadEntry 携带 shared_ptr<atomic<bool>> done 标志，
// 工作线程 return 前置位；抽出 Iec104Slave::CleanupFinishedClientThreads
// 静态 helper 做基于 done 的回收。本测试直接驱动 helper。
//=============================================================================

#include "mini_gtest.h"
#include "iec104_slave.h"
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

namespace {

using Entry = Iec104Slave::ClientThreadEntry;

Entry MakeEntry(bool finishedImmediately) {
    Entry e;
    e.done = std::make_shared<std::atomic<bool>>(false);
    auto done = e.done;
    if (finishedImmediately) {
        e.thr = std::thread([done]() { done->store(true); });
    } else {
        // 长跑线程：睡 5 秒（测试期间不结束）
        e.thr = std::thread([done]() {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            done->store(true);
        });
    }
    return e;
}

TEST(Iec104SlaveThreadCleanupTest, RemovesOnlyFinishedThreads) {
    std::vector<Entry> threads;
    threads.push_back(MakeEntry(/*finishedImmediately=*/true));
    threads.push_back(MakeEntry(/*finishedImmediately=*/false));
    threads.push_back(MakeEntry(/*finishedImmediately=*/true));

    // 等已完成的线程真的置位 done（sleep 让 std::thread 有时间跑完）
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    size_t removed = Iec104Slave::CleanupFinishedClientThreads(threads);
    EXPECT_EQ(removed, static_cast<size_t>(2));
    EXPECT_EQ(threads.size(), static_cast<size_t>(1));

    // 剩下的应是那个长跑的（done 尚未置位）
    ASSERT_TRUE(threads.front().done != nullptr);
    EXPECT_FALSE(threads.front().done->load());

    // 收尾：让剩下的线程 join 掉，避免测试进程卡死
    threads.front().done->store(true);   // 提前让 helper 下次能回收
    if (threads.front().thr.joinable()) threads.front().thr.join();
}

TEST(Iec104SlaveThreadCleanupTest, EmptyVectorIsNoOp) {
    std::vector<Entry> threads;
    size_t removed = Iec104Slave::CleanupFinishedClientThreads(threads);
    EXPECT_EQ(removed, static_cast<size_t>(0));
    EXPECT_TRUE(threads.empty());
}

TEST(Iec104SlaveThreadCleanupTest, AllFinishedAllRemoved) {
    std::vector<Entry> threads;
    for (int i = 0; i < 5; ++i)
        threads.push_back(MakeEntry(true));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    size_t removed = Iec104Slave::CleanupFinishedClientThreads(threads);
    EXPECT_EQ(removed, static_cast<size_t>(5));
    EXPECT_TRUE(threads.empty());
}

TEST(Iec104SlaveThreadCleanupTest, NoneFinishedNoneRemoved) {
    std::vector<Entry> threads;
    for (int i = 0; i < 3; ++i)
        threads.push_back(MakeEntry(false));

    size_t removed = Iec104Slave::CleanupFinishedClientThreads(threads);
    EXPECT_EQ(removed, static_cast<size_t>(0));
    EXPECT_EQ(threads.size(), static_cast<size_t>(3));

    // 清理
    for (auto& e : threads) {
        e.done->store(true);
        if (e.thr.joinable()) e.thr.join();
    }
}

} // namespace
