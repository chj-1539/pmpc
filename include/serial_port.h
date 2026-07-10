#ifndef SERIAL_PORT_H
#define SERIAL_PORT_H

#include <cstdint>
#include <memory>
#include <type_traits>
#include <string>
#include <chrono>
#include <system_error>
#include <stdexcept>

// ═══════════════════════════════════════════════════════════════════════════
//  错误码定义（serial_port 专用）
//  继承自 std::error_code 体系，可与 std::system_error 配合使用
// ═══════════════════════════════════════════════════════════════════════════

enum class serial_errc {
    invalid_port       = 1,
    open_failed        = 2,
    configure_failed   = 3,
    read_failed        = 4,
    write_failed       = 5,
    timeout            = 6,
    not_open           = 7,
    already_open       = 8,
};

template <>
struct std::is_error_code_enum<serial_errc> : std::true_type {};

std::error_code make_error_code(serial_errc e) noexcept;
const std::error_category& serial_category() noexcept;

// ─── 自定义异常类 ─────────────────────────────────────────────────────────
//  继承 std::system_error，携带错误码和可选的描述信息

class serial_port_error : public std::system_error {
public:
    explicit serial_port_error(serial_errc ec, const std::string& what = "")
        : std::system_error(make_error_code(ec), what) {}
};

// ═══════════════════════════════════════════════════════════════════════════
//  串口配置结构体
//  波特率、校验位、停止位、数据位、流控以及超时设置
// ═══════════════════════════════════════════════════════════════════════════

// ─── 波特率枚举 ──────────────────────────────────────────────────────────

enum class baud_rate {
    br_110     = 110,
    br_300     = 300,
    br_600     = 600,
    br_1200    = 1200,
    br_2400    = 2400,
    br_4800    = 4800,
    br_9600    = 9600,
    br_14400   = 14400,
    br_19200   = 19200,
    br_38400   = 38400,
    br_57600   = 57600,
    br_115200  = 115200,
    br_230400  = 230400,
    br_460800  = 460800,
    br_921600  = 921600,
};

// ─── 校验位枚举 ──────────────────────────────────────────────────────────
//  none  — 无校验
//  odd   — 奇校验
//  even  — 偶校验
//  mark  — 恒为 1（标记）
//  space — 恒为 0（空格）

enum class parity {
    none,
    odd,
    even,
    mark,
    space,
};

// ─── 停止位枚举 ──────────────────────────────────────────────────────────
//  one            — 1 位停止位
//  one_point_five — 1.5 位停止位（常用于异步通信）
//  two            — 2 位停止位

enum class stop_bits {
    one,
    one_point_five,
    two,
};

// ─── 流控制枚举 ──────────────────────────────────────────────────────────
//  none     — 无流控
//  software — 软件流控（XON / XOFF）
//  hardware — 硬件流控（RTS / CTS）

enum class flow_control {
    none,
    software,   // XON / XOFF
    hardware,   // RTS / CTS
};

// ─── 串口配置结构体 ─────────────────────────────────────────────────────
//  统一管理串口的各项参数，包含合理默认值（115200-8-N-1）

struct serial_config {
    baud_rate    baud          = baud_rate::br_115200;
    parity       parity_check  = parity::none;
    stop_bits    stop          = stop_bits::one;
    int          data_bits     = 8;
    flow_control flow          = flow_control::none;

    // 读取超时（0 = 非阻塞，-1 = 无限阻塞，>0 = 等待指定毫秒数）
    std::chrono::milliseconds read_timeout{-1};
    // 写入超时（0 = 非阻塞，-1 = 无限阻塞，>0 = 等待指定毫秒数）
    std::chrono::milliseconds write_timeout{-1};
};

// ═══════════════════════════════════════════════════════════════════════════
//  串口类（核心 API）
//  RAII 封装，提供打开/关闭、配置、读写、清空缓冲区等操作
//  支持移动语义，禁止拷贝
// ═══════════════════════════════════════════════════════════════════════════

class serial_port {
public:
    // ── 构造 / 析构 ─────────────────────────────────────────────────────────
    //  默认构造 + RAII：析构时自动关闭端口
    //  支持移动构造和移动赋值（禁止拷贝构造和拷贝赋值）

    serial_port() noexcept;

    explicit serial_port(const std::string& port_name, serial_config cfg = {});

    ~serial_port() noexcept;

    // 仅支持移动语义，禁止拷贝
    serial_port(serial_port&& other) noexcept;
    serial_port& operator=(serial_port&& other) noexcept;

    serial_port(const serial_port&) = delete;
    serial_port& operator=(const serial_port&) = delete;

    // ── 打开 / 关闭 ─────────────────────────────────────────────────────────
    //  open() 打开指定端口并应用配置，已在类内异常安全
    //  close() 关闭端口并清理资源，不会抛出异常

    void open(const std::string& port_name, serial_config cfg = {});

    void close() noexcept;

    bool is_open() const noexcept;

    // ── 配置 ─────────────────────────────────────────────────────────────────
    //  可在打开后随时修改串口参数（波特率、校验位等）

    void configure(const serial_config& cfg);
    serial_config current_config() const noexcept;

    // ── 读写操作 ─────────────────────────────────────────────────────────────
    //  支持原始字节读取、按行读取、原始字节写入和字符串写入
    //  超时行为由 serial_config 中的 read_timeout / write_timeout 决定

    std::size_t read(std::uint8_t* buf, std::size_t len);

    std::string read_line(char delim = '\n');

    std::size_t write(const std::uint8_t* data, std::size_t len);

    std::size_t write(const std::string& s);

    // 清空缓冲区：丢弃已接收但未读取 / 已写入但未发送的数据
    void flush_input();
    void flush_output();
    void flush_both();

    // ── 端口名称 ─────────────────────────────────────────────────────────────

    const std::string& port_name() const noexcept { return port_name_; }

private:
    // PIMPL（指向实现的指针）：隐藏平台相关的句柄和状态
    // 使用 unique_ptr 管理生命周期，析构函数在 .cpp 中定义以允许不完整类型
    struct impl;
    std::unique_ptr<impl> pimpl_;

    std::string port_name_;
    serial_config config_{};

    void move_from(serial_port& other) noexcept;

    void apply_config(const serial_config& cfg);
};

#endif // SERIAL_PORT_H
