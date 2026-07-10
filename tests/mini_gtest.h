//=============================================================================
// mini_gtest.h — 自包含 Google Test 兼容层 (单头文件, 无需外部依赖)
// 提供 TEST / TEST_F / EXPECT_* / ASSERT_* / TEST_P 等宏
// 支持: 测试注册、自动发现、失败计数、彩色输出、--gtest_filter
//=============================================================================
//
// 用法:
//   #include "mini_gtest.h"
//
//   TEST(MySuite, MyTest) {
//       EXPECT_EQ(1 + 1, 2);
//       ASSERT_TRUE(true);
//   }
//
//   int main(int argc, char** argv) {
//       ::testing::InitGoogleTest(&argc, argv);
//       return RUN_ALL_TESTS();
//   }
//
// 如果测试文件独自成可执行文件, 无需额外提供 main()
// (mini_gtest.h 会生成 main 除非定义了 MINI_GTEST_NO_MAIN)

#ifndef MINI_GTEST_H
#define MINI_GTEST_H

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <map>
#include <functional>
#include <algorithm>
#include <iostream>
#include <sstream>
#include <chrono>

// ==================== 颜色输出 (Windows) ====================
// Win10+ 终端原生支持 ANSI 转义码, 无需 <windows.h> 介入
#ifdef _WIN32
#include <io.h>  // _isatty
#endif

namespace testing {
namespace internal {

inline bool IsColorTerm() {
#ifdef _WIN32
    // Windows Terminal / ConEmu / VSCode 等支持 ANSI
    return true;
#else
    const char* t = std::getenv("TERM");
    return t && std::strstr(t, "color") != nullptr;
#endif
}

inline const char* Green()  { return IsColorTerm() ? "\033[32m" : ""; }
inline const char* Red()    { return IsColorTerm() ? "\033[31m" : ""; }
inline const char* Yellow() { return IsColorTerm() ? "\033[33m" : ""; }
inline const char* Cyan()   { return IsColorTerm() ? "\033[36m" : ""; }
inline const char* Reset()  { return IsColorTerm() ? "\033[0m"  : ""; }

// ==================== 测试结果记录 ====================

struct TestResult {
    std::string suiteName;
    std::string testName;
    bool        passed;
    int         failures;
    std::string failuresStr;
    int64_t     elapsedMs;
};

inline std::vector<TestResult>& Results() {
    static std::vector<TestResult> r;
    return r;
}

// ==================== 断言日志 ====================

class TestLog {
public:
    static TestLog& Instance() {
        static TestLog inst;
        return inst;
    }

    void Reset() {
        failures_ = 0;
        stream_.str("");
        stream_.clear();
    }

    void Fail(const char* file, int line, const std::string& msg) {
        failures_++;
        stream_ << "    " << file << ":" << line << ": " << msg << "\n";
    }

    int FailCount() const { return failures_; }
    std::string Str() const { return stream_.str(); }

private:
    int failures_ = 0;
    std::ostringstream stream_;
};

// ==================== 测试基类 ====================

class Test {
public:
    virtual void SetUp() {}
    virtual void TearDown() {}
    virtual void TestBody() = 0;
    virtual ~Test() = default;

    // 获取当前测试信息
    static std::string CurrentSuiteName;
    static std::string CurrentTestName;
};

inline std::string Test::CurrentSuiteName;
inline std::string Test::CurrentTestName;

// ==================== TEST_P 参数化测试支持 ====================

/// 参数化测试基类: 继承该类可获得 GetParam()
template<typename T>
class TestWithParam : public Test {
public:
    using ParamType = T;
    const T& GetParam() const { return param_; }
    void SetParam(const T& p) { param_ = p; }
private:
    T param_ = T();
};

// 参数集注册表
struct ParamSetEntry {
    std::string suiteName;
    size_t count;
    std::function<void(Test*, size_t)> applier;
};

inline std::vector<ParamSetEntry>& ParamSets() {
    static std::vector<ParamSetEntry> ps;
    return ps;
}

// ==================== 测试注册 ====================

using TestFactory = std::function<Test*()>;

struct TestEntry {
    std::string suiteName;
    std::string testName;
    TestFactory factory;
    int         paramSetIdx;  // -1 = 非参数化测试; >=0 时指向 ParamSets 中的索引
    size_t      paramIdx;    // 如果是参数化测试, 第几个参数
};

inline std::vector<TestEntry>& Registry() {
    static std::vector<TestEntry> reg;
    return reg;
}

inline bool Register(const std::string& suite, const std::string& name,
                     TestFactory factory) {
    Registry().push_back({suite, name, std::move(factory), -1, 0});
    return true;
}

// ==================== --gtest_filter 支持 ====================

inline std::string GTestFilter;

/// 简单的 glob 匹配 (* 匹配任意字符序列, 包括 '.')
inline bool GlobMatch(const std::string& s, const std::string& p) {
    if (p == "*") return true;
    size_t si = 0, pi = 0;
    while (pi < p.size() && si <= s.size()) {
        if (p[pi] == '*') {
            // 跳过连续 '*'
            while (pi + 1 < p.size() && p[pi + 1] == '*') pi++;
            if (pi + 1 == p.size()) return true; // 尾随 * 匹配剩余全部
            // 尝试在 s 的每个位置匹配剩下的 pattern
            for (size_t i = si; i <= s.size(); i++) {
                if (GlobMatch(s.substr(i), p.substr(pi + 1)))
                    return true;
            }
            return false;
        }
        if (si < s.size() && (p[pi] == '?' || p[pi] == s[si])) {
            si++; pi++;
        } else {
            return false;
        }
    }
    return si == s.size() && pi == p.size();
}

/// 检查测试名是否匹配过滤器 (支持 : 分隔多个模式, OR 逻辑)
inline bool MatchesFilter(const std::string& fullName, const std::string& filter) {
    if (filter.empty()) return true;
    size_t start = 0, pos;
    while ((pos = filter.find(':', start)) != std::string::npos) {
        std::string pattern = filter.substr(start, pos - start);
        if (GlobMatch(fullName, pattern)) return true;
        start = pos + 1;
    }
    std::string pattern = filter.substr(start);
    return GlobMatch(fullName, pattern);
}

// ==================== 测试运行器 ====================

inline int RunAllTests() {
    int total    = 0;
    int passed   = 0;
    int failed   = 0;

    // 按 suite 分组
    std::vector<std::string> suites;
    for (const auto& entry : Registry()) {
        if (std::find(suites.begin(), suites.end(), entry.suiteName) == suites.end())
            suites.push_back(entry.suiteName);
    }

    auto startAll = std::chrono::steady_clock::now();

    for (const auto& suite : suites) {
        int suiteTotal = 0, suitePassed = 0;

        // 统计本 suite 需要跑的测试数
        for (const auto& entry : Registry()) {
            if (entry.suiteName != suite) continue;
            std::string fullName = suite + "." + entry.testName;
            if (!MatchesFilter(fullName, GTestFilter)) continue;
            suiteTotal++;
        }
        if (suiteTotal == 0) continue;

        std::cout << Yellow() << "[----------] " << Reset()
                  << suiteTotal << " test(s) from " << suite << "\n";

        for (const auto& entry : Registry()) {
            if (entry.suiteName != suite) continue;
            std::string fullName = suite + "." + entry.testName;
            if (!MatchesFilter(fullName, GTestFilter)) continue;

            total++;
            Test::CurrentSuiteName = entry.suiteName;
            Test::CurrentTestName  = entry.testName;

            TestLog::Instance().Reset();

            auto t0 = std::chrono::steady_clock::now();
            Test* test = entry.factory();
            // 如果是参数化测试, 设置参数
            if (entry.paramSetIdx >= 0) {
                ParamSets()[entry.paramSetIdx].applier(test, entry.paramIdx);
            }
            test->SetUp();
            test->TestBody();
            test->TearDown();
            delete test;
            auto t1 = std::chrono::steady_clock::now();

            int64_t elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
            int failures = TestLog::Instance().FailCount();

            TestResult result;
            result.suiteName   = suite;
            result.testName    = entry.testName;
            result.passed      = (failures == 0);
            result.failures    = failures;
            result.failuresStr = TestLog::Instance().Str();
            result.elapsedMs   = elapsed;
            Results().push_back(result);

            if (failures == 0) {
                std::cout << Green() << "[       OK ] " << Reset();
                passed++;
                suitePassed++;
            } else {
                std::cout << Red() << "[  FAILED  ] " << Reset();
                failed++;
            }
            std::cout << entry.testName << " (" << elapsed << " ms)\n";

            if (failures > 0) {
                std::cout << TestLog::Instance().Str();
            }
        }

        std::cout << Yellow() << "[----------] " << Reset()
                  << suitePassed << "/" << suiteTotal << " passed\n\n";
    }

    auto endAll = std::chrono::steady_clock::now();
    int64_t totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(endAll - startAll).count();

    // 汇总
    if (failed > 0) {
        std::cout << Red()   << "[  FAILED  ] " << Reset()
                  << failed << " test(s)\n";
    }
    std::cout << Green() << "[==========] " << Reset()
              << total << " test(s), " << passed << " passed, "
              << failed << " failed, " << totalMs << " ms total\n";

    return failed > 0 ? 1 : 0;
}

// ==================== 初始化 ====================

inline void InitGoogleTest(int* argc, char** argv) {
    if (argc && argv) {
        for (int i = 1; i < *argc; i++) {
            std::string arg(argv[i]);
            if (arg.find("--gtest_filter=") == 0) {
                GTestFilter = arg.substr(std::string("--gtest_filter=").size());
                // 从 argv 中移除该参数 (压缩)
                for (int j = i; j < *argc - 1; j++) argv[j] = argv[j + 1];
                (*argc)--;
                i--;
            }
        }
    }
}

} // namespace internal
} // namespace testing

// ==================== 公共宏 ====================

#define TEST(Suite, Name)                                                          \
class PMPC_T_##Suite##_##Name : public ::testing::internal::Test {                \
public:                                                                            \
    void TestBody() override;                                                      \
};                                                                                 \
static bool PMPC_R_##Suite##_##Name = ::testing::internal::Register(              \
    #Suite, #Name, []() { return new PMPC_T_##Suite##_##Name(); });               \
void PMPC_T_##Suite##_##Name::TestBody()

#define TEST_F(Fixture, Name)                                                      \
class PMPC_TF_##Fixture##_##Name : public Fixture {                               \
public:                                                                            \
    void TestBody() override;                                                      \
};                                                                                 \
static bool PMPC_RF_##Fixture##_##Name = ::testing::internal::Register(           \
    #Fixture, #Name, []() { return new PMPC_TF_##Fixture##_##Name(); });          \
void PMPC_TF_##Fixture##_##Name::TestBody()

// ==================== TEST_P 参数化测试宏 ====================

#define TEST_P(Suite, Name)                                                        \
class PMPC_TP_##Suite##_##Name : public Suite {                                   \
public:                                                                            \
    void TestBody() override;                                                      \
};                                                                                 \
static bool PMPC_RP_##Suite##_##Name = ::testing::internal::Register(             \
    #Suite, #Name, []() { return new PMPC_TP_##Suite##_##Name(); });              \
void PMPC_TP_##Suite##_##Name::TestBody()

#define INSTANTIATE_TEST_SUITE_P(Prefix, Suite, ParamValuesExpr)                  \
static bool PMPC_ISP_##Suite = []() {                                              \
    auto _vals = ParamValuesExpr;                                                  \
    size_t _cnt = _vals.size();                                                    \
    int _psIdx = (int)::testing::internal::ParamSets().size();                     \
    ::testing::internal::ParamSets().push_back({                                   \
        #Suite, _cnt,                                                              \
        [](::testing::internal::Test* _t, size_t _i) {                             \
            using _ParamT = typename Suite::ParamType;                              \
            auto& _v = _vals[_i];                                                  \
            static_cast<Suite*>(_t)->SetParam(_v);                                 \
        }                                                                          \
    });                                                                            \
    /* 为已注册的该 Suite 的 TEST_P 设置参数索引 */                                  \
    for (size_t _pi = 0; _pi < _cnt; _pi++) {                                      \
        for (auto& _e : ::testing::internal::Registry()) {                         \
            if (_e.suiteName == #Suite && _e.paramSetIdx < 0) {                    \
                _e.paramSetIdx = _psIdx;                                           \
                _e.paramIdx = _pi;                                                 \
            }                                                                      \
        }                                                                          \
    }                                                                              \
    return true;                                                                   \
}()

// ==================== 非致命断言 (继续执行) ====================

#define EXPECT_TRUE(expr)                                                          \
do {                                                                               \
    if (!(expr)) {                                                                 \
        ::testing::internal::TestLog::Instance().Fail(                             \
            __FILE__, __LINE__, "EXPECT_TRUE(" #expr ") FAILED");                 \
    }                                                                              \
} while(0)

#define EXPECT_FALSE(expr)                                                         \
do {                                                                               \
    if ((expr)) {                                                                  \
        ::testing::internal::TestLog::Instance().Fail(                             \
            __FILE__, __LINE__, "EXPECT_FALSE(" #expr ") FAILED - was true");     \
    }                                                                              \
} while(0)

#define EXPECT_EQ(a, b)                                                            \
do {                                                                               \
    auto _a = (a); auto _b = (b);                                                 \
    if (!(_a == _b)) {                                                             \
        ::testing::internal::TestLog::Instance().Fail(                             \
            __FILE__, __LINE__, "EXPECT_EQ(" #a ", " #b ") FAILED");              \
    }                                                                              \
} while(0)

#define EXPECT_NE(a, b)                                                            \
do {                                                                               \
    auto _a = (a); auto _b = (b);                                                 \
    if (!(_a != _b)) {                                                             \
        ::testing::internal::TestLog::Instance().Fail(                             \
            __FILE__, __LINE__, "EXPECT_NE(" #a ", " #b ") FAILED");              \
    }                                                                              \
} while(0)

#define EXPECT_GT(a, b)                                                            \
do {                                                                               \
    auto _a = (a); auto _b = (b);                                                 \
    if (!(_a > _b)) {                                                              \
        ::testing::internal::TestLog::Instance().Fail(                             \
            __FILE__, __LINE__, "EXPECT_GT(" #a ", " #b ") FAILED");              \
    }                                                                              \
} while(0)

#define EXPECT_GE(a, b)                                                            \
do {                                                                               \
    auto _a = (a); auto _b = (b);                                                 \
    if (!(_a >= _b)) {                                                             \
        ::testing::internal::TestLog::Instance().Fail(                             \
            __FILE__, __LINE__, "EXPECT_GE(" #a ", " #b ") FAILED");              \
    }                                                                              \
} while(0)

#define EXPECT_DOUBLE_EQ(a, b)                                                     \
do {                                                                               \
    auto _a = (a); auto _b = (b);                                                 \
    if (std::fabs(_a - _b) > 1e-6) {                                              \
        std::ostringstream _msg;                                                   \
        _msg << "EXPECT_DOUBLE_EQ(" #a ", " #b ") FAILED: "                       \
             << _a << " != " << _b << " (diff=" << std::fabs(_a - _b) << ")";     \
        ::testing::internal::TestLog::Instance().Fail(                             \
            __FILE__, __LINE__, _msg.str());                                       \
    }                                                                              \
} while(0)

#define EXPECT_NEAR(a, b, eps)                                                     \
do {                                                                               \
    auto _a = (a); auto _b = (b); auto _e = (eps);                                \
    auto _d = (_a > _b) ? (_a - _b) : (_b - _a);                                  \
    if (_d > _e) {                                                                 \
        std::ostringstream _msg;                                                   \
        _msg << "EXPECT_NEAR(" #a ", " #b ", " #eps ") FAILED: "                  \
             << _a << " - " << _b << " = " << _d << " > " << _e;                  \
        ::testing::internal::TestLog::Instance().Fail(                             \
            __FILE__, __LINE__, _msg.str());                                       \
    }                                                                              \
} while(0)

#define EXPECT_NO_THROW(stmt)                                                      \
do {                                                                               \
    try { stmt; }                                                                  \
    catch (const std::exception& _e) {                                             \
        std::ostringstream _msg;                                                   \
        _msg << "EXPECT_NO_THROW(" #stmt ") threw: " << _e.what();                \
        ::testing::internal::TestLog::Instance().Fail(                             \
            __FILE__, __LINE__, _msg.str());                                       \
    } catch (...) {                                                                \
        ::testing::internal::TestLog::Instance().Fail(                             \
            __FILE__, __LINE__, "EXPECT_NO_THROW(" #stmt ") threw unknown");      \
    }                                                                              \
} while(0)

#define EXPECT_THROW(stmt, exc)                                                    \
do {                                                                               \
    bool _caught = false;                                                          \
    try { stmt; }                                                                  \
    catch (const exc&) { _caught = true; }                                         \
    catch (...) {}                                                                 \
    if (!_caught) {                                                                \
        ::testing::internal::TestLog::Instance().Fail(                             \
            __FILE__, __LINE__,                                                    \
            "EXPECT_THROW(" #stmt ", " #exc ") did not throw");                   \
    }                                                                              \
} while(0)

#define EXPECT_THROW_ANY(stmt)                                                     \
do {                                                                               \
    bool _caught = false;                                                          \
    try { stmt; }                                                                  \
    catch (...) { _caught = true; }                                                \
    if (!_caught) {                                                                \
        ::testing::internal::TestLog::Instance().Fail(                             \
            __FILE__, __LINE__,                                                    \
            "EXPECT_THROW_ANY(" #stmt ") did not throw");                         \
    }                                                                              \
} while(0)

#define EXPECT_STR_EQ(a, b)                                                        \
do {                                                                               \
    const char* _a = (a);                                                          \
    const char* _b = (b);                                                          \
    if (std::strcmp(_a, _b) != 0) {                                                \
        std::ostringstream _msg;                                                   \
        _msg << "EXPECT_STR_EQ(" #a ", " #b ") FAILED\n"                          \
             << "    actual:   \"" << (_a ? _a : "(null)") << "\"\n"              \
             << "    expected: \"" << (_b ? _b : "(null)") << "\"";                \
        ::testing::internal::TestLog::Instance().Fail(                             \
            __FILE__, __LINE__, _msg.str());                                       \
    }                                                                              \
} while(0)

// ==================== 致命断言 (失败则 return) ====================

#define ASSERT_TRUE(expr)                                                          \
do {                                                                               \
    if (!(expr)) {                                                                 \
        ::testing::internal::TestLog::Instance().Fail(                             \
            __FILE__, __LINE__, "ASSERT_TRUE(" #expr ") FAILED");                 \
        return;                                                                    \
    }                                                                              \
} while(0)

#define ASSERT_FALSE(expr)                                                         \
do {                                                                               \
    if ((expr)) {                                                                  \
        ::testing::internal::TestLog::Instance().Fail(                             \
            __FILE__, __LINE__, "ASSERT_FALSE(" #expr ") FAILED - was true");     \
        return;                                                                    \
    }                                                                              \
} while(0)

#define ASSERT_EQ(a, b)                                                            \
do {                                                                               \
    auto _a = (a); auto _b = (b);                                                 \
    if (!(_a == _b)) {                                                             \
        ::testing::internal::TestLog::Instance().Fail(                             \
            __FILE__, __LINE__, "ASSERT_EQ(" #a ", " #b ") FAILED");              \
        return;                                                                    \
    }                                                                              \
} while(0)

#define ASSERT_NE(a, b)                                                            \
do {                                                                               \
    auto _a = (a); auto _b = (b);                                                 \
    if (!(_a != _b)) {                                                             \
        ::testing::internal::TestLog::Instance().Fail(                             \
            __FILE__, __LINE__, "ASSERT_NE(" #a ", " #b ") FAILED");              \
        return;                                                                    \
    }                                                                              \
} while(0)

#define ASSERT_GT(a, b)                                                            \
do {                                                                               \
    auto _a = (a); auto _b = (b);                                                 \
    if (!(_a > _b)) {                                                              \
        ::testing::internal::TestLog::Instance().Fail(                             \
            __FILE__, __LINE__, "ASSERT_GT(" #a ", " #b ") FAILED");              \
        return;                                                                    \
    }                                                                              \
} while(0)

#define ASSERT_GE(a, b)                                                            \
do {                                                                               \
    auto _a = (a); auto _b = (b);                                                 \
    if (!(_a >= _b)) {                                                             \
        ::testing::internal::TestLog::Instance().Fail(                             \
            __FILE__, __LINE__, "ASSERT_GE(" #a ", " #b ") FAILED");              \
        return;                                                                    \
    }                                                                              \
} while(0)

#define ASSERT_NO_THROW(stmt)                                                      \
do {                                                                               \
    try { stmt; }                                                                  \
    catch (const std::exception& _e) {                                             \
        std::ostringstream _msg;                                                   \
        _msg << "ASSERT_NO_THROW(" #stmt ") threw: " << _e.what();                \
        ::testing::internal::TestLog::Instance().Fail(                             \
            __FILE__, __LINE__, _msg.str());                                       \
        return;                                                                    \
    } catch (...) {                                                                \
        ::testing::internal::TestLog::Instance().Fail(                             \
            __FILE__, __LINE__, "ASSERT_NO_THROW(" #stmt ") threw unknown");      \
        return;                                                                    \
    }                                                                              \
} while(0)

#define ASSERT_THROW(stmt, exc)                                                    \
do {                                                                               \
    bool _caught = false;                                                          \
    try { stmt; }                                                                  \
    catch (const exc&) { _caught = true; }                                         \
    catch (...) {}                                                                 \
    if (!_caught) {                                                                \
        ::testing::internal::TestLog::Instance().Fail(                             \
            __FILE__, __LINE__,                                                    \
            "ASSERT_THROW(" #stmt ", " #exc ") did not throw");                   \
        return;                                                                    \
    }                                                                              \
} while(0)

// ==================== 便捷别名 ====================

namespace testing {
using Test = internal::Test;
template<typename T> using TestWithParam = internal::TestWithParam<T>;
using internal::InitGoogleTest;
using internal::RunAllTests;
using internal::Registry;
} // namespace testing

#define RUN_ALL_TESTS() ::testing::internal::RunAllTests()

// ==================== 默认 main (除非定义了 MINI_GTEST_NO_MAIN) ====================

#ifndef MINI_GTEST_NO_MAIN
int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
#endif

#endif // MINI_GTEST_H
