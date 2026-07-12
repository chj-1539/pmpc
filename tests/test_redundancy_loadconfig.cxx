//=============================================================================
// test_redundancy_loadconfig.cxx
//
// 回归 L5 (code review)：RedundancyManager::LoadConfig 曾在打不开配置文件
// 时 return true 并保留硬编码默认值 peerIp_="127.0.0.1"，两台机器裸上
// 生产都用默认 config 就会互相视对方为 localhost，形成同机双主。
//
// 修复：文件打不开时 return false，让 ModuleManager 跳过 redundancy 模块
// 而不是启动一个错配的实例。
//=============================================================================

#include "mini_gtest.h"
#include "redundancy.h"
#include <cstdio>
#include <fstream>

namespace {

TEST(RedundancyLoadConfigTest, MissingFileReturnsFalse) {
    RedundancyManager m;
    // 不存在的文件路径
    EXPECT_FALSE(m.LoadConfig("nonexistent_redundancy_cfg_zzz.ini"));
    // Role 仍为默认（Idle），未 start
    EXPECT_EQ(static_cast<int>(m.GetRole()), static_cast<int>(RedundRole::Idle));
    EXPECT_FALSE(m.IsRunning());
}

TEST(RedundancyLoadConfigTest, ValidFileReturnsTrue) {
    // 建一个最小合法 config
    const char* path = "test_redundancy_l5_cfg.ini";
    {
        std::ofstream f(path);
        f << "local_name=box_a\n"
          << "peer_ip=192.168.1.100\n"
          << "heartbeat_port=7503\n"
          << "sync_port=7502\n"
          << "priority=100\n";
    }
    RedundancyManager m;
    EXPECT_TRUE(m.LoadConfig(path));
    // localName 已被读入
    EXPECT_STR_EQ(m.GetLocalName().c_str(), "box_a");

    std::remove(path);
}

TEST(RedundancyLoadConfigTest, EmptyFileReturnsTrueButUsesDefaults) {
    // 空文件：可打开但没内容 —— 保留默认值。这不是致命错误，
    // 但结合 return false 的 missing-file 分支来看，"能打开的文件默认值
    // OK / 打不开 = 禁用" 的策略是一致的：ops 至少要提供 empty 文件表明
    // 意图。
    const char* path = "test_redundancy_l5_empty.ini";
    { std::ofstream f(path); }
    RedundancyManager m;
    EXPECT_TRUE(m.LoadConfig(path));
    std::remove(path);
}

} // namespace
