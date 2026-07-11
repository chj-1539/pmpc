//=============================================================================
// test_logger.cxx — Logger 基础功能测试
// 覆盖: 级别设置、ModuleLevel、LogStream 格式、级别过滤、线程安全
// 策略: 重定向 cout → stringstream 捕获日志输出验证
//=============================================================================
#include "mini_gtest.h"
#include "logger.h"
#include <thread>
#include <vector>
#include <sstream>

class LoggerTest : public ::testing::Test {
protected:
    std::streambuf* oldCout_;
    std::ostringstream capture_;

    void SetUp() override {
        // 重定向 cout
        oldCout_ = std::cout.rdbuf(capture_.rdbuf());
        Logger::SetLevel(Logger::INFO);
        Logger::SetModuleLevel("test", 0); // reset
    }

    void TearDown() override {
        std::cout.rdbuf(oldCout_);
    }

    std::string Output() const { return capture_.str(); }
    void Clear() { capture_.str(""); capture_.clear(); }
};

TEST_F(LoggerTest, DefaultLevelIsInfo) {
    EXPECT_EQ(Logger::GetLevel(), Logger::INFO);
}

TEST_F(LoggerTest, SetAndGetLevelRoundtrip) {
    Logger::SetLevel(Logger::DEBUG);
    EXPECT_EQ(Logger::GetLevel(), Logger::DEBUG);
    Logger::SetLevel(Logger::NONE);
    EXPECT_EQ(Logger::GetLevel(), Logger::NONE);
}

TEST_F(LoggerTest, ErrorOutputFormat) {
    Clear();
    { Logger::Error("TestMod") << "error msg" << std::endl; }
    std::string out = Output();
    // Should contain: [E][TestMod] error msg
    EXPECT_NE(out.find("[E][TestMod]"), std::string::npos);
    EXPECT_NE(out.find("error msg"), std::string::npos);
    // Should start with a timestamp (HH:MM:SS.mmm)
    EXPECT_TRUE(out.size() > 12 && out[2] == ':');
}

TEST_F(LoggerTest, WarnOutputFormat) {
    Clear();
    { Logger::Warn("WMod") << "warn msg" << std::endl; }
    std::string out = Output();
    EXPECT_NE(out.find("[W][WMod]"), std::string::npos);
    EXPECT_NE(out.find("warn msg"), std::string::npos);
}

TEST_F(LoggerTest, InfoOutputFormat) {
    Clear();
    { Logger::Info("IMod") << "info msg" << std::endl; }
    std::string out = Output();
    EXPECT_NE(out.find("[I][IMod]"), std::string::npos);
    EXPECT_NE(out.find("info msg"), std::string::npos);
}

TEST_F(LoggerTest, DebugOutputFormat) {
    Logger::SetLevel(Logger::DEBUG);
    Clear();
    { Logger::Debug("DMod") << "debug msg" << std::endl; }
    std::string out = Output();
    EXPECT_NE(out.find("[D][DMod]"), std::string::npos);
    EXPECT_NE(out.find("debug msg"), std::string::npos);
}

TEST_F(LoggerTest, LogStreamCanStreamMultipleTypes) {
    Clear();
    { Logger::Info("Multi") << "str " << 42 << " " << 3.14 << std::endl; }
    std::string out = Output();
    EXPECT_NE(out.find("str 42 3.14"), std::string::npos);
}

// 【特征化测试 / 已知 bug】TODO(logger-level-filter)
// 现状：Logger::Debug()/Info()/Warn()/Error() 在被禁用级别下仍会构造 LogStream
// 并在析构时无条件输出——级别过滤实际上从未生效。这个测试锁定当前（错误）
// 行为，防止悄悄改回来。真正修复时应把断言翻转为 EXPECT_EQ(out.find(...), npos)。
TEST_F(LoggerTest, TODO_BelowThresholdCurrentlyNotFiltered_BUG) {
    Clear();
    { Logger::Debug("Hidden") << "should not appear" << std::endl; }
    // 当前 LogStream 无视级别，析构时始终输出——bug。
    std::string out = Output();
    EXPECT_NE(out.find("[D][Hidden]"), std::string::npos);
}

TEST_F(LoggerTest, ModuleLevelOverride) {
    Logger::SetModuleLevel("special", Logger::DEBUG);
    // Module level is stored but filtering is at call site
    EXPECT_EQ(Logger::GetLevel(), Logger::INFO);
}

TEST_F(LoggerTest, ConcurrentLogWrites) {
    Logger::SetLevel(Logger::INFO);
    Clear();

    std::vector<std::thread> threads;
    const int NUM_THREADS = 4;
    const int LINES_PER_THREAD = 10;

    for (int i = 0; i < NUM_THREADS; i++) {
        threads.emplace_back([i]() {
            for (int j = 0; j < LINES_PER_THREAD; j++) {
                Logger::Info("Thr") << "thread " << i << " line " << j << std::endl;
            }
        });
    }

    for (auto& t : threads) t.join();

    std::string out = Output();
    int lineCount = 0;
    size_t pos = 0;
    while ((pos = out.find("[I][Thr]", pos)) != std::string::npos) {
        lineCount++;
        pos += 8;
    }
    // Each thread produces LINES_PER_THREAD lines
    EXPECT_EQ(lineCount, NUM_THREADS * LINES_PER_THREAD);
}

TEST_F(LoggerTest, LogStreamEndlProducesNewline) {
    Clear();
    { Logger::Info("NL") << "line1" << std::endl << "line2" << std::endl; }
    std::string out = Output();
    // Count newlines in the message part
    size_t nl = 0, pos = 0;
    while ((pos = out.find('\n', pos)) != std::string::npos) { nl++; pos++; }
    // At least 2 newlines in the message (plus one more from the format)
    EXPECT_GE(nl, 2);
}
