#ifndef TELNET_IAC_H
#define TELNET_IAC_H

//=============================================================================
// telnet_iac.h — telnet IAC 协商字节过滤
//
// 纯文本 TCP 服务（如 DebugConsole）不响应 telnet 协商命令，把 IAC 序列
// 从输入流剥离后再交给命令解析。这个函数原来内嵌在 debug_console.cxx，
// 现在提到独立头文件供 tests/test_debug_console_telnet.cxx 直接测试。
//
// telnet 协议参考：RFC 854（IAC = Interpret As Command = 0xFF）
//
// 【已知局限】跨 recv 分包问题：本实现是无状态的，若一个 IAC 序列被拆到
// 两次 recv() 中，末尾的 0xFF 会被 break 丢弃；下一次 recv 从选项字节开始，
// 就会当成普通字符流入。这是 code review 的 H7 findings，方案里计划在
// 后续 stateful 重写时修复。见 CLAUDE.md「已知陷阱 / 修复历史」bug #3。
//=============================================================================

#include <cstddef>
#include <cstdint>
#include <string>

namespace pmpc {

inline std::string filter_telnet_iac(const uint8_t* data, size_t len)
{
    std::string out;
    for (size_t i = 0; i < len; i++) {
        if (data[i] == 0xFF) {
            // IAC 序列：至少 2 字节
            if (i + 1 >= len) break;
            uint8_t cmd = data[i + 1];
            if (cmd == 0xFF) {
                // IAC IAC（转义的 0xFF）— 保留一个
                out += '\xFF';
                i += 1;
            } else if (cmd == 0xF0) {
                // SE — 2 字节
                i += 1;
            } else if (cmd == 0xFA) {
                // SB … SE 子协商 — 变长，跳过直到 IAC SE
                i += 2;
                while (i + 1 < len && !(data[i] == 0xFF && data[i + 1] == 0xF0))
                    i++;
                if (i + 1 < len) i += 1; // 跳过 SE
            } else if (cmd >= 0xFB && cmd <= 0xFE) {
                // WILL(0xFB)/WONT(0xFC)/DO(0xFD)/DONT(0xFE) — 3 字节: IAC + cmd + opt
                i += 2;
            } else {
                // 其余 2 字节 IAC 命令: NOP(0xF1)/BREAK(0xF3)/IP(0xF4)/AO(0xF5)/
                // AYT(0xF6)/EC(0xF7)/EL(0xF8)/GA(0xF9) 等
                i += 1;
            }
        } else {
            out += static_cast<char>(data[i]);
        }
    }
    return out;
}

} // namespace pmpc

#endif // TELNET_IAC_H
