//=============================================================================
// test_data_recorder_atomic.cxx
//
// 回归 DR-1（第二轮代码审查）：DataRecorder::mysql_ 从普通 MYSQL* 改为
// std::atomic<MYSQL*>。快速路径 On*Change 里的 `if (!mysql_) return` 与
// TimerThread 里的 swap 之前是数据竞争 UB —— 现在通过 load(acquire) /
// exchange(acq_rel) 建立 happens-before。
//
// 因为 mysql.h 在 CI 环境不可用，这个测试用 std::atomic<void*> 验证
// atomic 指针在 pmpc 目标平台（Windows / MinGW g++）上的必要保证：
//   * 类型可原子读写（is_lock_free / is_always_lock_free）
//   * exchange 语义正确（拿回旧值 + 写入新值）
//   * store(release) / load(acquire) 建立 happens-before 让另一线程看
//     到写入完成 —— 用一个"写入相邻数据后 store 指针"的模式验证。
//
// 这不是直接测生产代码，但保证支撑 DR-1 修复的 C++ atomic 抽象在本机
// 编译器下有效；如果哪天平台变了（例如换成 ARM/MSVC），本测试会先炸。
//=============================================================================

#include "mini_gtest.h"
#include <atomic>
#include <thread>
#include <chrono>

// ── 基础语义 ───────────────────────────────────────────────────────────────

TEST(AtomicPointer, IsLockFreeOnHostPlatform) {
    // 关键前提：在 x86_64/aarch64 上 atomic<T*> 应该是 lock-free；否则
    // 每次 On*Change 快速路径会去抢内部 mutex，反而更糟。
    std::atomic<void*> p{nullptr};
    EXPECT_TRUE(p.is_lock_free());
    EXPECT_TRUE(std::atomic<void*>::is_always_lock_free);
}

TEST(AtomicPointer, InitialValueIsNull) {
    std::atomic<void*> p{nullptr};
    EXPECT_EQ(p.load(std::memory_order_acquire), nullptr);
}

TEST(AtomicPointer, StoreThenLoadYieldsSameValue) {
    std::atomic<void*> p{nullptr};
    int dummy = 0;
    p.store(&dummy, std::memory_order_release);
    EXPECT_EQ(p.load(std::memory_order_acquire), &dummy);
}

TEST(AtomicPointer, ExchangeReturnsOldValueAndStoresNew) {
    // DR-1 的 TimerThread 重连成功后:
    //   MYSQL* oldConn = mysql_.exchange(newConn, acq_rel);
    // 一步完成 swap，之前的两次锁 + 三次赋值被合并。
    int a = 0, b = 0;
    std::atomic<void*> p{&a};
    void* prev = p.exchange(&b, std::memory_order_acq_rel);
    EXPECT_EQ(prev, &a);
    EXPECT_EQ(p.load(std::memory_order_acquire), &b);
}

TEST(AtomicPointer, ExchangeFromNullYieldsNull) {
    int a = 0;
    std::atomic<void*> p{nullptr};
    void* prev = p.exchange(&a, std::memory_order_acq_rel);
    EXPECT_EQ(prev, nullptr);
    EXPECT_EQ(p.load(std::memory_order_acquire), &a);
}

// ── 关键场景：release/acquire 建立 happens-before ─────────────────────────

TEST(AtomicPointer, ReleaseAcquireHappensBeforeVisible) {
    // 类似 On*Change 快速路径 vs TimerThread swap：一个线程写数据后 store
    // pointer(release)；另一个线程 load pointer(acquire) 后应看到之前写的
    // 数据。这里连续跑 1000 轮无 mismatch 认为语义正确。
    struct Payload { int a = 0, b = 0, c = 0; };
    for (int trial = 0; trial < 1000; ++trial) {
        Payload payload;
        std::atomic<Payload*> p{nullptr};
        std::atomic<bool> ready{false};
        int observed_a = -1, observed_b = -1, observed_c = -1;

        std::thread writer([&]{
            payload.a = 42;
            payload.b = 43;
            payload.c = 44;
            p.store(&payload, std::memory_order_release);
        });
        std::thread reader([&]{
            Payload* q = nullptr;
            // 忙等：真实场景下 On*Change 拿到锁后进行 load
            while (!(q = p.load(std::memory_order_acquire))) {
                std::this_thread::yield();
            }
            observed_a = q->a;
            observed_b = q->b;
            observed_c = q->c;
            ready.store(true, std::memory_order_release);
        });
        writer.join();
        reader.join();

        EXPECT_EQ(observed_a, 42);
        EXPECT_EQ(observed_b, 43);
        EXPECT_EQ(observed_c, 44);
        EXPECT_TRUE(ready.load(std::memory_order_acquire));
    }
}

// ── 反面：普通指针在竞争下可能读到"半写入"（本测试仅记录，永不断言）───

// （无实测；C++ standard 说是 UB —— 结果不确定。这里只是留个注释指向
// 修复动机。）
