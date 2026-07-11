//=============================================================================
// test_debug_console_lifecycle.cxx
//
// 回归 M8：DebugConsole::StopAutoTask 曾死锁——
//   持 autoMtx_ 后 join() 工作线程，工作线程每轮循环都要抢 autoMtx_ →
//   工作线程等锁、Stop 线程等 join，两个都不动，必然死锁。
//
// 同类问题存在于 Stop() 和 CmdAutoStop("all") 分支。
//
// 修复策略：抽出静态模板 DetachTaskLocked / DetachAllTasksLocked，
//   在锁内标记 stopFlag、把 unique_ptr 移出容器；锁外再 join。
//   AutoTask::stopFlag 是 atomic，从锁外可安全读写。
//
// 本测试直接驱动这两个模板：构造 std::vector<unique_ptr<AutoTask>>，
// 起真实工作线程模拟"每轮抢 autoMtx_"，然后调用 DetachTaskLocked +
// join；若修复未生效，即使加超时也会死锁。这样测试不必拖 debug_console.cxx
// 一整棵传递依赖图。
//=============================================================================

#include "mini_gtest.h"
#include "debug_console.h"    // 只为拿到 DebugConsole::AutoTask + 两个 static 模板
#include <atomic>
#include <chrono>
#include <future>
#include <thread>
#include <mutex>
#include <vector>
#include <memory>

using AutoTask = DebugConsole::AutoTask;

namespace {

// 在指定 vector 上模拟 AutoToggleThread 的核心节奏：每轮循环都要抢 mtx_，
// 在锁内检查自己是否已被 erase / stopFlag 是否为 true。是则 return。
// 这就是原来死锁 pattern 的另一半：工作线程等锁。
void FakeWorker(std::mutex& mtx,
                std::vector<std::unique_ptr<AutoTask>>& vec,
                std::thread::id selfId)
{
    while (true) {
        {
            std::lock_guard<std::mutex> lock(mtx);
            bool found = false;
            for (auto& t : vec) {
                if (t->thr.get_id() == selfId) {
                    found = true;
                    if (t->stopFlag) return;
                    break;
                }
            }
            if (!found) return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

// 等待 fn() 在 timeoutMs 内完成；返回 true 表示按时完成，false 表示超时。
template <typename F>
bool RunWithTimeout(F&& fn, int timeoutMs) {
    std::atomic<bool> done{false};
    std::thread worker([&]() { fn(); done.store(true); });
    auto deadline = std::chrono::steady_clock::now()
                    + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (done.load()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (done.load()) { worker.join(); return true; }
    // 超时——为防测试进程卡死，detach 而不 join。
    worker.detach();
    return false;
}

// 起一个真实的假工作线程，写入 task->thr，返回 task 引用。
int SpawnFake(std::mutex& mtx,
              std::vector<std::unique_ptr<AutoTask>>& vec,
              int id)
{
    auto task = std::make_unique<AutoTask>();
    task->id         = id;
    task->type       = AutoTask::DI_TOGGLE;
    task->ch         = 1; task->dev = 1; task->pt = 2;
    task->intervalMs = 100;
    task->stopFlag   = false;

    // 用 promise/future 把线程 id 传回 lambda 内部，避免闭包捕获 task 引用
    // 后又 std::move
    std::promise<std::thread::id> idProm;
    auto idFut = idProm.get_future();
    task->thr = std::thread([&mtx, &vec, &idProm]() {
        auto selfId = std::this_thread::get_id();
        idProm.set_value(selfId);
        FakeWorker(mtx, vec, selfId);
    });
    (void)idFut.get();  // 阻塞到线程真正跑起来
    {
        std::lock_guard<std::mutex> lock(mtx);
        vec.push_back(std::move(task));
    }
    return id;
}

// M8 关键回归：DetachTaskLocked + 锁外 join 组合能在合理时间返回。
TEST(DebugConsoleLifecycleTest, DetachAndJoinReturnsPromptly) {
    std::mutex mtx;
    std::vector<std::unique_ptr<AutoTask>> vec;

    SpawnFake(mtx, vec, 1);
    // 等工作线程真正进入 while 循环并抢过一次锁
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    bool ok = RunWithTimeout([&]() {
        auto victim = DebugConsole::DetachTaskLocked(mtx, vec, 1);
        EXPECT_TRUE(!!victim);
        if (victim && victim->thr.joinable()) victim->thr.join();
    }, 3000);
    EXPECT_TRUE(ok);   // 修复前：false（死锁）
    EXPECT_EQ(vec.size(), static_cast<size_t>(0));
}

// 多任务同时 stop（DebugConsole::Stop / CmdAutoStop "all" 路径）
TEST(DebugConsoleLifecycleTest, DetachAllReturnsPromptly) {
    std::mutex mtx;
    std::vector<std::unique_ptr<AutoTask>> vec;

    SpawnFake(mtx, vec, 1);
    SpawnFake(mtx, vec, 2);
    SpawnFake(mtx, vec, 3);
    EXPECT_EQ(vec.size(), static_cast<size_t>(3));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    bool ok = RunWithTimeout([&]() {
        auto stopped = DebugConsole::DetachAllTasksLocked(mtx, vec);
        for (auto& t : stopped) if (t->thr.joinable()) t->thr.join();
    }, 5000);
    EXPECT_TRUE(ok);
    EXPECT_EQ(vec.size(), static_cast<size_t>(0));
}

// 不存在的 id：应立即返回空 unique_ptr，不 join、不阻塞。
TEST(DebugConsoleLifecycleTest, DetachMissingIdReturnsEmpty) {
    std::mutex mtx;
    std::vector<std::unique_ptr<AutoTask>> vec;
    bool ok = RunWithTimeout([&]() {
        auto v = DebugConsole::DetachTaskLocked(mtx, vec, 9999);
        EXPECT_FALSE(!!v);
    }, 1000);
    EXPECT_TRUE(ok);
}

} // namespace
