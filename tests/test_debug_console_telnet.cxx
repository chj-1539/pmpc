//=============================================================================
// test_debug_console_telnet.cxx
//
// 回归 CLAUDE.md bug #3（DebugConsole telnet IAC 协商过滤）。原来这个函数
// 内嵌在 debug_console.cxx 的匿名 namespace，未测试。切口 4 已把它提到
// include/telnet_iac.h 供直接单元测试。
//
// 同时用 DISABLED_ 前缀标记 H7（跨 recv 分包时丢字节）—— 无状态实现的
// 已知局限，后续 stateful 重写会解决。
//=============================================================================

#include "mini_gtest.h"
#include "telnet_iac.h"
#include <string>
#include <vector>

using pmpc::filter_telnet_iac;

namespace {

// 快捷：把 vector<uint8_t> 直接过滤成 std::string
std::string Filter(const std::vector<uint8_t>& bytes) {
    return filter_telnet_iac(bytes.data(), bytes.size());
}

// IAC 常量（RFC 854）
constexpr uint8_t IAC  = 0xFF;
constexpr uint8_t SE   = 0xF0;
constexpr uint8_t NOP  = 0xF1;
constexpr uint8_t SB   = 0xFA;
constexpr uint8_t WILL = 0xFB;
constexpr uint8_t WONT = 0xFC;
constexpr uint8_t DO_  = 0xFD;
constexpr uint8_t DONT = 0xFE;
constexpr uint8_t OPT_ECHO       = 0x01;
constexpr uint8_t OPT_SUPP_GA    = 0x03;
constexpr uint8_t OPT_TERM_TYPE  = 0x18;

TEST(TelnetIacTest, PreservesPrintableAscii) {
    std::vector<uint8_t> in{'h', 'e', 'l', 'l', 'o', '\r', '\n'};
    EXPECT_STR_EQ(Filter(in).c_str(), "hello\r\n");
}

TEST(TelnetIacTest, StripsIacDoEcho) {
    // Windows telnet 首次连接常发的 IAC DO ECHO
    std::vector<uint8_t> in{IAC, DO_, OPT_ECHO, 'x'};
    EXPECT_STR_EQ(Filter(in).c_str(), "x");
}

TEST(TelnetIacTest, StripsIacWontWill) {
    // IAC WILL SUPPRESS-GO-AHEAD 后跟命令
    std::vector<uint8_t> in{IAC, WILL, OPT_SUPP_GA, 'g', 'e', 't'};
    EXPECT_STR_EQ(Filter(in).c_str(), "get");
}

TEST(TelnetIacTest, StripsIacWontDont) {
    std::vector<uint8_t> in{IAC, WONT, OPT_ECHO, IAC, DONT, OPT_ECHO, 'a'};
    EXPECT_STR_EQ(Filter(in).c_str(), "a");
}

TEST(TelnetIacTest, StripsSubnegotiation) {
    // IAC SB TERMINAL-TYPE IS "xterm" IAC SE
    std::vector<uint8_t> in{IAC, SB, OPT_TERM_TYPE, 0x00,
                            'x', 't', 'e', 'r', 'm',
                            IAC, SE, 'r', 'u', 'n'};
    EXPECT_STR_EQ(Filter(in).c_str(), "run");
}

TEST(TelnetIacTest, StripsNop) {
    // IAC NOP 是 2 字节命令
    std::vector<uint8_t> in{'a', IAC, NOP, 'b'};
    EXPECT_STR_EQ(Filter(in).c_str(), "ab");
}

TEST(TelnetIacTest, EscapedIacKeepsOneFF) {
    // IAC IAC 在协议里代表数据里的 0xFF；应保留一个 0xFF
    std::vector<uint8_t> in{'x', IAC, IAC, 'y'};
    auto out = Filter(in);
    EXPECT_EQ(out.size(), static_cast<size_t>(3));
    EXPECT_EQ(static_cast<uint8_t>(out[0]), 'x');
    EXPECT_EQ(static_cast<uint8_t>(out[1]), 0xFF);
    EXPECT_EQ(static_cast<uint8_t>(out[2]), 'y');
}

TEST(TelnetIacTest, MixedSequence) {
    // 典型 Windows telnet 首次连接：连发多个协商 + 用户命令
    std::vector<uint8_t> in{IAC, DO_,  OPT_ECHO,
                            IAC, WILL, OPT_SUPP_GA,
                            IAC, DONT, OPT_ECHO,
                            'l', 's', '\r', '\n'};
    EXPECT_STR_EQ(Filter(in).c_str(), "ls\r\n");
}

TEST(TelnetIacTest, EmptyInput) {
    EXPECT_STR_EQ(Filter({}).c_str(), "");
}

TEST(TelnetIacTest, IacOnlyDoesNotCrash) {
    // 只有一个 IAC 字节（残缺协商，末端）— 应被丢弃且不越界
    std::vector<uint8_t> in{IAC};
    EXPECT_STR_EQ(Filter(in).c_str(), "");
}

TEST(TelnetIacTest, SubnegotiationWithoutSeEnds) {
    // IAC SB 没有配对 IAC SE 就到流末尾 — 应静默吞掉后续所有字节
    // （现实中不会出现，测防御性行为）
    std::vector<uint8_t> in{'x', IAC, SB, OPT_TERM_TYPE, 'a', 'b', 'c'};
    EXPECT_STR_EQ(Filter(in).c_str(), "x");
}

// ---- H7：跨包分片是当前实现的已知缺陷（无状态）----
// 若 IAC 序列被拆到两次 recv() 中，末尾裸 0xFF 会被丢，下一次 recv 从
// 选项字节开始就成了普通字符。真正的修复需要让 filter_telnet_iac 成为
// stateful 类（记住"上一个字节是 IAC"）。这条测试用 DISABLED_ 前缀，
// 修好后去掉前缀就翻转为绿。
TEST(TelnetIacTest, DISABLED_TrailingIacAtChunkBoundaryIsDeferred) {
    // 场景：第一个 recv 收到 [IAC]，第二个 recv 收到 [DO, ECHO, 'x']。
    // 无状态实现会把两次分别过滤成 "" + "\xFD\x01x"（第二个 recv 里
    // 首字节被当作数据），期望应为 "x"。
    std::vector<uint8_t> chunk1{IAC};
    std::vector<uint8_t> chunk2{DO_, OPT_ECHO, 'x'};
    std::string out = Filter(chunk1) + Filter(chunk2);
    EXPECT_STR_EQ(out.c_str(), "x");
    // TODO(H7): 修好 stateful 后去掉 DISABLED_ 前缀。
}

} // namespace
