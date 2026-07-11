//=============================================================================
// test_module_manager_hotreload.cxx
//
// 回归 CLAUDE.md bug #1：ModuleManager::entries_ 包含全部（含禁用）模块，
// 而 modules_ 只包含已启用且创建成功的；两者索引不一致。老代码 ReloadModule
// 复用 FindModule 返回的 modules_ 索引去查 entries_，一旦中间有禁用条目就
// 会取错。修复：ReloadModule 按名查 entries_。
//
// 本测试通过在 ModuleFactory 里注册若干测试专用假模块，然后用一份中间
// 有禁用条目的 app_modules.ini 触发 ModuleManager::LoadConfig，最后调
// ReloadModule("m3") —— 若查错 entry（拿到 m2 的 cfg_file）就会失败。
//=============================================================================

#include "mini_gtest.h"
#include "module_manager.h"
#include "module_factory.h"
#include <atomic>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

// module_manager.cxx 里的 ModbusTcpMaster/PempServer/RedundancyManager PIMPL
// 会用到 g_running；测试进程没有 main.cxx，这里补一个。
std::atomic<bool> g_running{true};

namespace {

// ─── 假模块：只记账，不做任何 IO ───────────────────────────────────
// 每个 fake module 用不同名字注册；SharedCounters 收集调用历史，供测试断言。
struct FakeCounter {
    int loadCalls  = 0;
    int startCalls = 0;
    int stopCalls  = 0;
    std::string lastCfg;
};

// 全部假模块共享的记账表（进程级）。测试之间用 GetCounters().clear() 清。
inline std::map<std::string, FakeCounter>& GetCounters() {
    static std::map<std::string, FakeCounter> m;
    return m;
}

class FakeAppModule : public AppModule {
public:
    explicit FakeAppModule(std::string n) : name_(std::move(n)) {}
    const char* Name() const override { return name_.c_str(); }
    bool LoadConfig(const std::string& cfgPath) override {
        GetCounters()[name_].loadCalls++;
        GetCounters()[name_].lastCfg = cfgPath;
        return true;
    }
    bool ValidateConfig(const std::string&, std::vector<std::string>&) override { return true; }
    bool Start() override {
        GetCounters()[name_].startCalls++;
        running_ = true;
        return true;
    }
    void Stop() override {
        GetCounters()[name_].stopCalls++;
        running_ = false;
    }
    bool IsRunning() const override { return running_; }
private:
    std::string name_;
    bool        running_ = false;
};

// 用宏一次性造 5 个假模块类型（m1..m5），全部注册到 ModuleFactory
#define REGISTER_FAKE(NAME)                                             \
    class Fake_##NAME : public FakeAppModule {                          \
    public: Fake_##NAME() : FakeAppModule(#NAME) {}                      \
    };                                                                  \
    static bool _reg_fake_##NAME = []{                                   \
        ModuleFactory::Register(#NAME,                                   \
            []{ return std::unique_ptr<AppModule>(new Fake_##NAME()); });\
        return true;                                                     \
    }();

REGISTER_FAKE(m1)
REGISTER_FAKE(m2)
REGISTER_FAKE(m3)
REGISTER_FAKE(m4)
REGISTER_FAKE(m5)

#undef REGISTER_FAKE

class ModuleManagerHotReloadTest : public ::testing::Test {
protected:
    void SetUp() override {
        GetCounters().clear();
        cfgPaths_.clear();
    }
    void TearDown() override {
        for (auto& p : cfgPaths_) std::remove(p.c_str());
        for (auto& p : appPaths_) std::remove(p.c_str());
    }

    // 生成一份模块配置文件（内容不重要，只需存在）
    std::string MakeModuleCfg(const std::string& moduleName) {
        std::string path = "test_mm_" + moduleName + "_cfg.ini";
        std::ofstream f(path);
        f << "; " << moduleName << " test config\n"
          << "[global]\nname=" << moduleName << "\n";
        f.close();
        cfgPaths_.push_back(path);
        return path;
    }

    // 写 app_modules.ini：列表格式 (name, enable, cfg 文件)
    std::string MakeAppModulesIni(const std::vector<std::tuple<std::string, bool, std::string>>& mods) {
        std::string path = "test_mm_app_modules_" +
                            std::to_string(appPaths_.size()) + ".ini";
        std::ofstream f(path);
        f << "[global]\nwatch_interval=2\n\n";
        for (auto& [name, enable, cfg] : mods) {
            f << "[" << name << "]\n"
              << "enable=" << (enable ? "true" : "false") << "\n"
              << "auto_reload=false\n";
            if (!cfg.empty()) f << "cfg_file=" << cfg << "\n";
            f << "\n";
        }
        f.close();
        appPaths_.push_back(path);
        return path;
    }

    std::vector<std::string> cfgPaths_;
    std::vector<std::string> appPaths_;
};

// bug #1 关键：中间禁用的 entry 使得 modules_ 与 entries_ 索引错位。
// ReloadModule("m3") 若走错索引，会加载 m4 的 cfg（或 m2/m5 的 cfg，看方向）。
// 本测试用 5 个 entry，中间两个禁用：
//   entries_ = [m1(on), m2(off), m3(on), m4(off), m5(on)]
//   modules_ = [m1,             m3,             m5]
//   modules_[1] == m3，其对应 entries_[2]（非 entries_[1]==m2）
TEST_F(ModuleManagerHotReloadTest, ReloadDoesNotShiftIndexWhenDisabledEntriesPresent) {
    auto cfg1 = MakeModuleCfg("m1");
    auto cfg2 = MakeModuleCfg("m2");   // 虽禁用，但 cfg 文件存在
    auto cfg3 = MakeModuleCfg("m3");
    auto cfg4 = MakeModuleCfg("m4");   // 禁用
    auto cfg5 = MakeModuleCfg("m5");
    auto app = MakeAppModulesIni({
        {"m1", true,  cfg1},
        {"m2", false, cfg2},
        {"m3", true,  cfg3},
        {"m4", false, cfg4},
        {"m5", true,  cfg5},
    });

    ModuleManager mgr;
    ASSERT_TRUE(mgr.LoadConfig(app));

    // 只启用的 3 个应实例化并调 LoadConfig 一次
    EXPECT_EQ(GetCounters()["m1"].loadCalls, 1);
    EXPECT_EQ(GetCounters()["m2"].loadCalls, 0);   // 未启用
    EXPECT_EQ(GetCounters()["m3"].loadCalls, 1);
    EXPECT_EQ(GetCounters()["m4"].loadCalls, 0);
    EXPECT_EQ(GetCounters()["m5"].loadCalls, 1);
    EXPECT_STR_EQ(GetCounters()["m3"].lastCfg.c_str(), cfg3.c_str());

    // 热重载 m3 —— bug #1 的核心：修复前若用 modules_[1] 的下标去查 entries_，
    // 会取到 entries_[1]（即 m2），从而用 m2 的 cfg 文件 reload。
    ASSERT_TRUE(mgr.ReloadModule("m3"));

    // m3 应重新 LoadConfig，且 cfg 路径仍是自己的 cfg3
    EXPECT_EQ(GetCounters()["m3"].loadCalls, 2);
    EXPECT_STR_EQ(GetCounters()["m3"].lastCfg.c_str(), cfg3.c_str());

    // m2/m4 不应被误 reload
    EXPECT_EQ(GetCounters()["m2"].loadCalls, 0);
    EXPECT_EQ(GetCounters()["m4"].loadCalls, 0);
}

// 尾部有禁用条目也不能干扰：reload m5（最后一个 enabled）
TEST_F(ModuleManagerHotReloadTest, ReloadLastEnabledWorksWithTrailingDisabled) {
    auto cfg1 = MakeModuleCfg("m1");
    auto cfg5 = MakeModuleCfg("m5");
    auto cfg2 = MakeModuleCfg("m2");
    auto app = MakeAppModulesIni({
        {"m1", true,  cfg1},
        {"m5", true,  cfg5},
        {"m2", false, cfg2},
    });

    ModuleManager mgr;
    ASSERT_TRUE(mgr.LoadConfig(app));

    ASSERT_TRUE(mgr.ReloadModule("m5"));
    EXPECT_EQ(GetCounters()["m5"].loadCalls, 2);
    EXPECT_STR_EQ(GetCounters()["m5"].lastCfg.c_str(), cfg5.c_str());
    EXPECT_EQ(GetCounters()["m2"].loadCalls, 0);
}

// 请求 reload 一个不存在的模块 → false，不影响其他
TEST_F(ModuleManagerHotReloadTest, ReloadUnknownModuleReturnsFalse) {
    auto cfg1 = MakeModuleCfg("m1");
    auto app = MakeAppModulesIni({ {"m1", true, cfg1} });

    ModuleManager mgr;
    ASSERT_TRUE(mgr.LoadConfig(app));

    EXPECT_FALSE(mgr.ReloadModule("nonexistent"));
    EXPECT_EQ(GetCounters()["m1"].loadCalls, 1);   // 未被误动
}

// Reload 保持模块的 running 状态：如果 reload 前 IsRunning 为 true，reload
// 后应仍在跑（stop → load → start）
TEST_F(ModuleManagerHotReloadTest, ReloadPreservesRunningState) {
    auto cfg1 = MakeModuleCfg("m1");
    auto cfg2 = MakeModuleCfg("m2");
    auto cfg3 = MakeModuleCfg("m3");
    auto app = MakeAppModulesIni({
        {"m1", true,  cfg1},
        {"m2", false, cfg2},   // 禁用，制造错位
        {"m3", true,  cfg3},
    });

    ModuleManager mgr;
    ASSERT_TRUE(mgr.LoadConfig(app));
    mgr.StartModule("m3");
    ASSERT_EQ(GetCounters()["m3"].startCalls, 1);

    ASSERT_TRUE(mgr.ReloadModule("m3"));

    EXPECT_EQ(GetCounters()["m3"].stopCalls,  1);   // reload 内部 stop 一次
    EXPECT_EQ(GetCounters()["m3"].loadCalls,  2);   // 首次 + reload
    EXPECT_EQ(GetCounters()["m3"].startCalls, 2);   // 首次 + reload 后重启

    auto* m3 = mgr.GetModule("m3");
    ASSERT_TRUE(m3 != nullptr);
    EXPECT_TRUE(m3->IsRunning());
}

} // namespace
