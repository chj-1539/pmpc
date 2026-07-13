//=============================================================================
// test_debug_console_race.cxx
//
// 回归第二轮 P1 簇 F（DC-1 / DC-3）：DebugConsole 关键并发缺陷。
//
// DC-1: 老代码 CreateAutoTask 先 std::thread(...) 启动线程，再 push_back
//       进 autoTasks_。工作线程首轮遍历 autoTasks_ 用 thread::id 找自己：
//       若 push_back 慢一步 → found=false → 立即 return。表现：用户敲
//       `auto di 1 1 2 100` 得到"已启动"，但 DI 永不翻转。
//       修复：AutoToggleThread / AutoSweepThread 直接接收
//       std::atomic<bool>* stopFlag（指向堆上的 AutoTask::stopFlag），
//       不再遍历 vector 找自己。CreateAutoTask 先 push、再启动、再回填 thr。
//
// DC-3: 老代码每 10 次 accept 遍历 clientThreads_，用 joinable() 判"能
//       否清理"—— 但 joinable() 对**未 join 的活线程**也是 true！把当前
//       正在跑的 ClientThread detach 掉，Stop() 只 join 剩余的，被 detach
//       的线程仍访问 this 与 running_ → 析构后 UAF。
//       修复：每个 ClientThread 关联一个 shared_ptr<atomic<bool>> done；
//       返回前置位 true（RAII guard 保证异常路径也置位）；cleanup 只
//       join+erase done=true 的。
//
// 本文件只测**关键行为**：
//   1. atomic<bool>* 从锁外正常读写（DC-1 stopFlag 语义）
//   2. shared_ptr<atomic<bool>> done 语义（DC-3）
//   3. DetachTaskLocked 已能匹配以"未 push thread"的 AutoTask 场景
//      （即 CreateAutoTask 现在的 push-first 顺序不破坏 M8 修复的 stop 路径）
//
// 完整端到端"起 telnet 连接、发命令、断连"集成测试需要真 socket，超出本簇。
//=============================================================================

#include "mini_gtest.h"
#include "debug_console.h"
#include <atomic>
#include <memory>
#include <thread>
#include <chrono>
#include <vector>

// ── DC-1: stopFlag 从锁外读，工作线程能立刻响应 stop ──────────────────────

TEST(AutoTaskStopFlag, WorkerSeesStopWithoutLock) {
    auto flag = std::make_shared<std::atomic<bool>>(false);
    std::atomic<int> ticks{0};
    std::atomic<bool> exited{false};

    // 模拟 AutoToggleThread 主循环：仅依赖 stopFlag，不再遍历任何 vector
    std::thread worker([&]{
        while (!flag->load(std::memory_order_acquire)) {
            ticks.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        exited.store(true, std::memory_order_release);
    });

    // 等它跑几个 tick
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_GE(ticks.load(), 1);

    // 置位 stop
    flag->store(true, std::memory_order_release);
    worker.join();
    EXPECT_TRUE(exited.load());
}

// ── DC-3: shared_ptr<atomic<bool>> done 让 cleanup 只清已退出的 ────────────

TEST(ClientThreadDoneFlag, OnlyDoneThreadsGetCleanedUp) {
    struct Client {
        std::shared_ptr<std::atomic<bool>> done;
        std::thread thr;
    };
    std::vector<Client> clients;
    // 起两个"活线程"（sleep 长时间）和一个"已退出线程"
    std::atomic<bool> keepAlive{true};
    for (int i = 0; i < 2; i++) {
        Client c;
        c.done = std::make_shared<std::atomic<bool>>(false);
        auto d = c.done;
        c.thr = std::thread([d, &keepAlive]{
            while (keepAlive.load()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            d->store(true, std::memory_order_release);
        });
        clients.push_back(std::move(c));
    }
    {
        Client c;
        c.done = std::make_shared<std::atomic<bool>>(false);
        auto d = c.done;
        c.thr = std::thread([d]{
            // 立刻 return，置位 done
            d->store(true, std::memory_order_release);
        });
        c.thr.join();
        clients.push_back(std::move(c));
    }

    // 模拟 AcceptLoop 的 cleanup：只清 done=true 的
    size_t survived = 0;
    for (size_t i = 0; i < clients.size(); ) {
        if (clients[i].done->load(std::memory_order_acquire)) {
            if (clients[i].thr.joinable()) clients[i].thr.join();
            clients.erase(clients.begin() + i);
        } else { ++i; survived++; }
    }
    EXPECT_EQ(survived, (size_t)2);   // 两个活线程保留
    EXPECT_EQ(clients.size(), (size_t)2);

    // 让活线程也退出，join
    keepAlive.store(false);
    for (auto& c : clients) if (c.thr.joinable()) c.thr.join();
}

TEST(ClientThreadDoneFlag, JoinableTrueForRunningThreadIsNotSafeSignal) {
    // 反面：老代码用 thr.joinable() 判"可清理"是错的 —— joinable() 对
    // 未 join 的活线程也返回 true，会把活线程 detach 掉。这个测试记录：
    std::atomic<bool> keepAlive{true};
    std::thread t([&]{ while (keepAlive.load()) std::this_thread::sleep_for(std::chrono::milliseconds(10)); });
    EXPECT_TRUE(t.joinable());   // 活线程也是 joinable → 老逻辑会 detach 掉
    keepAlive.store(false);
    t.join();
}

// ── DC-1 + M8 兼容：DetachTaskLocked 能匹配已 push 但 thread 尚未回填的 task ──

TEST(DetachTaskLocked, WorksBeforeThreadIsAssigned) {
    // 新 CreateAutoTask 的顺序: push → start thread → 回填 thr。若用户
    // 在"回填 thr 之前"就 stop，DetachTaskLocked 应该仍能找到 task 并
    // 置 stopFlag。thread 尚未 assign 时 thr.joinable() == false，
    // 调用方 join 会跳过；工作线程随后看到 stopFlag=true 自行退出。
    std::mutex mtx;
    std::vector<std::unique_ptr<DebugConsole::AutoTask>> vec;
    auto t = std::make_unique<DebugConsole::AutoTask>();
    t->id = 42;
    t->type = DebugConsole::AutoTask::DI_TOGGLE;
    // 未启动 thread，thr 是默认状态 —— joinable=false
    EXPECT_FALSE(t->thr.joinable());

    // 保存 stopFlag 指针（模拟 CreateAutoTask 里的 stopFlagPtr）
    std::atomic<bool>* fp = &t->stopFlag;
    vec.push_back(std::move(t));

    auto victim = DebugConsole::DetachTaskLocked(mtx, vec, 42);
    ASSERT_TRUE(victim != nullptr);
    EXPECT_EQ(victim->id, 42);
    EXPECT_TRUE(victim->stopFlag.load());
    // fp 仍指向合法内存（unique_ptr 只是 move 了所有权），加载不 crash
    EXPECT_TRUE(fp->load());
    EXPECT_EQ(vec.size(), (size_t)0);
}

TEST(DetachTaskLocked, MissingIdReturnsNullDoesNotAffectOthers) {
    std::mutex mtx;
    std::vector<std::unique_ptr<DebugConsole::AutoTask>> vec;
    auto t1 = std::make_unique<DebugConsole::AutoTask>();
    t1->id = 1;
    auto t2 = std::make_unique<DebugConsole::AutoTask>();
    t2->id = 2;
    vec.push_back(std::move(t1));
    vec.push_back(std::move(t2));

    auto victim = DebugConsole::DetachTaskLocked(mtx, vec, 99);
    EXPECT_TRUE(victim == nullptr);
    EXPECT_EQ(vec.size(), (size_t)2);
    EXPECT_FALSE(vec[0]->stopFlag.load());
    EXPECT_FALSE(vec[1]->stopFlag.load());
}
