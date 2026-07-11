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

// ---- H7 已修复：跨包分片由 stateful TelnetIacFilter 处理 ----
// 保留原用例的 stateful 版本（用 TelnetIacFilter 而非 free function），
// 断言跨 recv 边界的 IAC 序列被正确处理。

// stateful 版本 helper：喂两段 chunk，返回拼接后的过滤结果
// 注：不能写 `f.Feed(a) + f.Feed(b)`，因 operator+ 的两侧求值顺序未指定
// (C++17 仍未 sequenced)，g++ 实测会先跑第二个。
std::string FeedTwoChunks(const std::vector<uint8_t>& a,
                          const std::vector<uint8_t>& b) {
    pmpc::TelnetIacFilter f;
    std::string out;
    out += f.Feed(a.data(), a.size());
    out += f.Feed(b.data(), b.size());
    return out;
}

TEST(TelnetIacStatefulTest, TrailingIacAtChunkBoundaryDeferred) {
    // 场景：recv#1 收到 [IAC]，recv#2 收到 [DO, ECHO, 'x']。stateful 过滤
    // 器应记住"上一字节是 IAC"，第二 recv 首字节作 cmd 处理。期望："x"
    std::vector<uint8_t> chunk1{IAC};
    std::vector<uint8_t> chunk2{DO_, OPT_ECHO, 'x'};
    EXPECT_STR_EQ(FeedTwoChunks(chunk1, chunk2).c_str(), "x");
}

TEST(TelnetIacStatefulTest, IacCmdOptionSplitAcrossThreeChunks) {
    // IAC DO ECHO 恰好被切三段：[IAC] [DO] [ECHO,'y']
    pmpc::TelnetIacFilter f;
    std::vector<uint8_t> c1{IAC};
    std::vector<uint8_t> c2{DO_};
    std::vector<uint8_t> c3{OPT_ECHO, 'y'};
    std::string out;
    out += f.Feed(c1.data(), c1.size());
    out += f.Feed(c2.data(), c2.size());
    out += f.Feed(c3.data(), c3.size());
    EXPECT_STR_EQ(out.c_str(), "y");
}

TEST(TelnetIacStatefulTest, SubnegotiationSplitAcrossChunks) {
    // IAC SB TERM_TYPE 0 'x' 't' 'e' 'r' 'm' IAC SE 'r' 'u' 'n'
    // 被切成 3 段：header / body / SE+data
    pmpc::TelnetIacFilter f;
    std::vector<uint8_t> c1{IAC, SB, OPT_TERM_TYPE};
    std::vector<uint8_t> c2{0x00, 'x', 't', 'e', 'r', 'm'};
    std::vector<uint8_t> c3{IAC, SE, 'r', 'u', 'n'};
    std::string out;
    out += f.Feed(c1.data(), c1.size());
    out += f.Feed(c2.data(), c2.size());
    out += f.Feed(c3.data(), c3.size());
    EXPECT_STR_EQ(out.c_str(), "run");
}

TEST(TelnetIacStatefulTest, EscapedIacInDataSplitAcrossChunks) {
    // 数据流里 0xFF 由 IAC IAC 转义。跨包切开：[IAC] [IAC, 'z']
    pmpc::TelnetIacFilter f;
    std::vector<uint8_t> c1{'a', IAC};
    std::vector<uint8_t> c2{IAC, 'z'};
    std::string out;
    out += f.Feed(c1.data(), c1.size());
    out += f.Feed(c2.data(), c2.size());
    // 期望：'a' + 0xFF + 'z'
    ASSERT_EQ(out.size(), static_cast<size_t>(3));
    EXPECT_EQ(static_cast<uint8_t>(out[0]), 'a');
    EXPECT_EQ(static_cast<uint8_t>(out[1]), 0xFF);
    EXPECT_EQ(static_cast<uint8_t>(out[2]), 'z');
}

TEST(TelnetIacStatefulTest, HasPendingReflectsInternalState) {
    pmpc::TelnetIacFilter f;
    EXPECT_FALSE(f.HasPending());
    std::vector<uint8_t> c1{IAC};
    f.Feed(c1.data(), c1.size());
    EXPECT_TRUE(f.HasPending());          // 等 cmd 字节
    std::vector<uint8_t> c2{DO_};
    f.Feed(c2.data(), c2.size());
    EXPECT_TRUE(f.HasPending());          // 等 option 字节
    std::vector<uint8_t> c3{OPT_ECHO};
    f.Feed(c3.data(), c3.size());
    EXPECT_FALSE(f.HasPending());          // 命令完成，回到 Normal
}

TEST(TelnetIacStatefulTest, ResetClearsInternalState) {
    pmpc::TelnetIacFilter f;
    std::vector<uint8_t> c1{IAC};
    f.Feed(c1.data(), c1.size());
    ASSERT_TRUE(f.HasPending());
    f.Reset();
    EXPECT_FALSE(f.HasPending());
    // Reset 后收到普通字节应作为数据
    std::vector<uint8_t> c2{'A'};
    std::string out = f.Feed(c2.data(), c2.size());
    EXPECT_STR_EQ(out.c_str(), "A");
}

} // namespace
