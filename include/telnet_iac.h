#ifndef TELNET_IAC_H
#define TELNET_IAC_H

//=============================================================================
// telnet_iac.h — telnet IAC 协商字节过滤
//
// 纯文本 TCP 服务（如 DebugConsole）不响应 telnet 协商命令，把 IAC 序列
// 从输入流剥离后再交给命令解析。原来内嵌在 debug_console.cxx，提到独立
// 头文件供 tests/test_debug_console_telnet.cxx 直接测试。
//
// H7 修复：现在有两个 API：
//   * TelnetIacFilter (类)：stateful，跨 recv() 调用能正确处理被切成两半
//     的 IAC 序列。DebugConsole 每个 client 保留一个实例。
//   * filter_telnet_iac (free function)：无状态一次性过滤，用于测试与
//     兼容整块数据的旧调用点。等价于 "TelnetIacFilter{}.Feed(...)" 一次
//     喂完整 buffer。
//
// telnet 协议参考：RFC 854（IAC = Interpret As Command = 0xFF）
//=============================================================================

#include <cstddef>
#include <cstdint>
#include <string>

namespace pmpc {

// stateful 过滤器：跨 Feed() 调用保留 telnet 命令解析状态。
// 用法：
//   TelnetIacFilter f;
//   while (recv) { auto text = f.Feed(buf, n); ... }
class TelnetIacFilter {
public:
    // 喂一段字节流；返回其中的普通文本（已剥离 IAC 命令）。
    // 若 IAC 序列跨包切分，状态保留到下次 Feed 才产出对应字节。
    std::string Feed(const uint8_t* data, size_t len) {
        std::string out;
        for (size_t i = 0; i < len; i++) {
            const uint8_t b = data[i];
            switch (state_) {
            case State::Normal:
                if (b == 0xFF) state_ = State::GotIac;
                else           out += static_cast<char>(b);
                break;
            case State::GotIac:
                // 已看到 IAC，接下来是 cmd 字节
                if (b == 0xFF) {
                    // 转义的 0xFF —— 数据字节
                    out += '\xFF';
                    state_ = State::Normal;
                } else if (b == 0xFA) {
                    // SB 子协商开始
                    state_ = State::InSubneg;
                } else if (b >= 0xFB && b <= 0xFE) {
                    // WILL/WONT/DO/DONT: 后面还有 1 字节 option
                    state_ = State::AwaitOption;
                } else {
                    // 其他 2 字节命令 (NOP/BREAK/IP/AO/AYT/EC/EL/GA/SE)：
                    // 已被 IAC + cmd 消耗完，返回 Normal
                    state_ = State::Normal;
                }
                break;
            case State::AwaitOption:
                // WILL/WONT/DO/DONT 的 option 字节 —— 丢弃
                state_ = State::Normal;
                break;
            case State::InSubneg:
                // 在 SB 子协商内：等待 IAC 出现
                if (b == 0xFF) state_ = State::SubnegSawIac;
                break;
            case State::SubnegSawIac:
                if (b == 0xF0) {
                    // IAC SE —— 子协商结束
                    state_ = State::Normal;
                } else if (b == 0xFF) {
                    // SB 内的转义 0xFF 数据字节 —— 仍在子协商内，回到 InSubneg
                    state_ = State::InSubneg;
                } else {
                    // 子协商内嵌套的其他 IAC 命令：按 telnet 规范，SB 内的
                    // IAC 只能是 IAC IAC (数据) 或 IAC SE (结束)。这里保守
                    // 处理，退回 InSubneg 继续找 SE。
                    state_ = State::InSubneg;
                }
                break;
            }
        }
        return out;
    }

    // 重置状态到初始（Normal）。
    void Reset() { state_ = State::Normal; }

    // 当前是否处于中间状态（有 pending 字节等下一批 Feed）。
    // 测试与调试用；生产不必看。
    bool HasPending() const { return state_ != State::Normal; }

private:
    enum class State {
        Normal,       // 普通字节流
        GotIac,       // 刚收到 0xFF，等命令字节
        AwaitOption,  // WILL/WONT/DO/DONT 后等 option
        InSubneg,     // 在 IAC SB ... 里
        SubnegSawIac, // SB 内收到 IAC，等 SE / IAC / 别的
    };
    State state_ = State::Normal;
};

// 无状态兼容 API：等价于 "一次性 Feed" 一整块数据。适合测试与整包处理场景。
// 【注意】跨调用不保留状态；跨 recv 分包场景必须用 TelnetIacFilter 类。
inline std::string filter_telnet_iac(const uint8_t* data, size_t len) {
    TelnetIacFilter f;
    return f.Feed(data, len);
}

} // namespace pmpc

#endif // TELNET_IAC_H
