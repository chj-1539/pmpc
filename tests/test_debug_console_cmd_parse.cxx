//=============================================================================
// test_debug_console_cmd_parse.cxx
//
// 模拟用户场景：输入几个"错误"指令后，正确指令是否仍然有效。
// 测试 SplitArgs 与命令分发逻辑，确保无副作用从一次错误传播到下一次。
//=============================================================================

#include "mini_gtest.h"
#include "telnet_iac.h"
#include <string>
#include <vector>
#include <sstream>
#include <algorithm>
#include <cctype>

namespace {

// ── 把 SplitArgs / ToLower / Trim 复制过来（它们是 free functions / static） ──

static std::string ToLower(const std::string& s) {
    std::string r = s;
    for (auto& c : r) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return r;
}

static std::string Trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

static std::vector<std::string> SplitArgs(const std::string& line) {
    std::vector<std::string> args;
    std::istringstream iss(line);
    std::string token;
    while (iss >> token) args.push_back(token);
    return args;
}

// ── 模拟 ProcessCommand 的 cmd 识别逻辑 ──

enum class CmdMatch {
    HELP, SET, GET, AUTO, ROLE, STATUS, CHANNELS, DEVICES,
    RELOAD, START, STOP, UPLOAD, LOG, CLS, UNKNOWN
};

static CmdMatch MatchCmd(const std::string& line) {
    std::string trimmed = Trim(line);
    if (trimmed.empty()) return CmdMatch::UNKNOWN;

    auto args = SplitArgs(trimmed);
    if (args.empty()) return CmdMatch::UNKNOWN;

    std::string cmd = ToLower(args[0]);

    if (cmd == "help" || cmd == "h" || cmd == "?") return CmdMatch::HELP;
    if (cmd == "set") return CmdMatch::SET;
    if (cmd == "get") return CmdMatch::GET;
    if (cmd == "auto") return CmdMatch::AUTO;
    if (cmd == "role") return CmdMatch::ROLE;
    if (cmd == "status" || cmd == "st") return CmdMatch::STATUS;
    if (cmd == "channels" || cmd == "ch") return CmdMatch::CHANNELS;
    if (cmd == "devices" || cmd == "dev") return CmdMatch::DEVICES;
    if (cmd == "reload") return CmdMatch::RELOAD;
    if (cmd == "start") return CmdMatch::START;
    if (cmd == "stop") return CmdMatch::STOP;
    if (cmd == "upload") return CmdMatch::UPLOAD;
    if (cmd == "log") return CmdMatch::LOG;
    if (cmd == "cls" || cmd == "clear") return CmdMatch::CLS;
    return CmdMatch::UNKNOWN;
}

// ── 测试用例 ──

TEST(CmdParseTest, CorrectCommandAfterWrongCommands) {
    // 模拟用户先输入几个错误指令再输入正确指令
    std::vector<std::string> wrongCmds = {
        "get di",           // 缺少参数
        "set di 1",         // 缺少参数
        "hlp",              // 拼写错误
        "statu",            // 拼写错误
        "channls",          // 拼写错误
        "auto di 1",        // 缺少参数
        "set xyz 1 1 1 1",  // 错误子类型
        "get do",           // 不支持的子类型
        "log xyz",          // 错误子命令
        "reload",           // 缺少目标
        "",                 // 空命令
        "   ",              // 空白
        "\r\n",             // 仅有换行
    };

    // 先按顺序验证每个错误指令的匹配（不执行，只验证匹配逻辑无异常）
    for (const auto& cmd : wrongCmds) {
        CmdMatch match = MatchCmd(cmd);
        // 不应崩溃，应正常返回某个匹配
        EXPECT_NE(match, CmdMatch::HELP); // 至少不是 help
    }

    // 现在验证"连续错误后，正确指令仍能被识别"
    // 模拟 SplitArgs 对象被反复创建（实际 ProcessCommand 每次调用都创建新的）
    for (int round = 0; round < 5; round++) {
        // 先喂一批错误指令
        for (const auto& wrong : wrongCmds) {
            (void)MatchCmd(wrong);
        }

        // 再喂正确指令 —— 应正确识别
        struct { std::string input; CmdMatch expected; } correct[] = {
            {"help",           CmdMatch::HELP},
            {"h",              CmdMatch::HELP},
            {"?",              CmdMatch::HELP},
            {"set di 1 1 1 1", CmdMatch::SET},
            {"get di 1 1",     CmdMatch::GET},
            {"status",         CmdMatch::STATUS},
            {"st",             CmdMatch::STATUS},
            {"channels",       CmdMatch::CHANNELS},
            {"ch",             CmdMatch::CHANNELS},
            {"devices 1",      CmdMatch::DEVICES},
            {"role",           CmdMatch::ROLE},
            {"role master",    CmdMatch::ROLE},
            {"auto di 1 1 1 500", CmdMatch::AUTO},
            {"auto stop all",  CmdMatch::AUTO},
            {"reload point",   CmdMatch::RELOAD},
            {"start modbus_tcp_master", CmdMatch::START},
            {"stop modbus_tcp_master",  CmdMatch::STOP},
            {"log start",      CmdMatch::LOG},
            {"upload start",   CmdMatch::UPLOAD},
            {"cls",            CmdMatch::CLS},
            {"clear",          CmdMatch::CLS},
        };

        for (const auto& c : correct) {
            CmdMatch result = MatchCmd(c.input);
            EXPECT_EQ(result, c.expected);
        }
    }
}

TEST(CmdParseTest, SplitArgsIsStateless) {
    // 验证 SplitArgs 每次调用相互独立，无全局状态污染
    auto bad1 = SplitArgs("get di");
    ASSERT_EQ(bad1.size(), (size_t)2);

    auto bad2 = SplitArgs("set di 1 1 1 abc");
    ASSERT_EQ(bad2.size(), (size_t)6);

    // 验证正确指令解析不受前面影响
    auto good = SplitArgs("get di 1 1 3");
    ASSERT_EQ(good.size(), (size_t)5);
    EXPECT_EQ(good[0], "get");
    EXPECT_EQ(good[1], "di");
    EXPECT_EQ(good[2], "1");
    EXPECT_EQ(good[3], "1");
    EXPECT_EQ(good[4], "3");
}

TEST(CmdParseTest, TelnetIacFilter_StaysUnstuckAfterBogusInput) {
    // 验证 TelnetIacFilter 在接收各种"错误"数据（包括可能触发 IAC
    // 状态的字节序列）后，之后的普通数据仍能正确通过。
    pmpc::TelnetIacFilter filter;

    // 模拟"错误命令"场景：用户输入各种字节序列，包括可能
    // 触发 IAC 特殊状态机的字节
    struct { const uint8_t* data; size_t len; } bogusInputs[] = {
        // 一个裸 IAC 字节（不完整命令）
        {(const uint8_t*)"\xFF", 1},
        // IAC WILL 后缺 option 字节
        {(const uint8_t*)"\xFF\xFB", 2},
        // IAC SB 但不带 SE（malformed）
        {(const uint8_t*)"\xFF\xFA\x18\x00", 4},
        // 带转义 IAC IAC 的数据
        {(const uint8_t*)"\xFF\xFF\xFF\xFD\x01", 5},
        // 用户打的"错误指令"
        {(const uint8_t*)"hlp\r\n", 5},
        {(const uint8_t*)"get di\r\n", 8},
        {(const uint8_t*)"x\xFF\xFAyz\r\n", 8}, // 命令中夹杂 IAC SB
    };

    for (auto& bi : bogusInputs) {
        (void)filter.Feed(bi.data, bi.len);
    }

    // 此时发送一个明确的 help 命令 —— 它应该能完整通过
    const uint8_t helpCmd[] = "help\r\n";
    std::string out = filter.Feed(helpCmd, 6);

    // 如果 filter 卡在 InSubneg 或其它状态，"help\r\n" 会被吞掉
    // 期望至少能看到 "help"（\r 可能被 IAC 处理影响）
    EXPECT_NE(out.find("help"), std::string::npos);
}

TEST(CmdParseTest, TelnetIacFilter_UnterminatedSbDoesNotPermanentlyStuck) {
    // 更严格测试：IAC SB 不带 SE 后发大量正常数据 ——
    // 所有正常数据都被吞掉（因为 SB 未终止），但 filter 不应崩溃
    pmpc::TelnetIacFilter filter;

    // IAC SB NAWS 无 SE —— malformed, 此后 filter 在 InSubneg
    const uint8_t sbNoSe[] = {0xFF, 0xFA, 0x1F, 0x00, 0x50, 0x00, 0x19};
    std::string out1 = filter.Feed(sbNoSe, sizeof(sbNoSe));
    EXPECT_TRUE(out1.empty());
    EXPECT_TRUE(filter.HasPending()); // InSubneg

    // 此时想发 "help\r\n" —— 会被吞掉，因为还在 Subneg 内
    const uint8_t helpCmd1[] = "help\r\n";
    std::string out2 = filter.Feed(helpCmd1, 6);
    EXPECT_TRUE(out2.empty()); // 被吞，这是合理的

    // 再发 IAC SE 补完 subneg —— filter 应回到 Normal
    const uint8_t se[] = {0xFF, 0xF0};
    std::string out3 = filter.Feed(se, sizeof(se));
    EXPECT_TRUE(out3.empty());
    EXPECT_FALSE(filter.HasPending());

    // 至此 filter 应恢复正常
    const uint8_t helpCmd2[] = "help\r\n";
    std::string out4 = filter.Feed(helpCmd2, 6);
    EXPECT_NE(out4.find("help"), std::string::npos);
}

} // namespace
