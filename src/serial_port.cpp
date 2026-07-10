// ═══════════════════════════════════════════════════════════════════════════
//  serial_port 库的实现文件
//  通过预处理宏分平台编译：
//    - Windows: Win32 API (CreateFile, DCB, OVERLAPPED)
//    - POSIX:   termios + select (Linux / macOS / BSD)
// ═══════════════════════════════════════════════════════════════════════════

#include "serial_port.h"

#include <cstring>
#include <cerrno>
#include <system_error>

// ─── 平台检测 ───────────────────────────────────────────────────────────────
//  编译期自动检测操作系统，选择对应的 API 实现分支

#if defined(_WIN32)
#  define PLATFORM_WINDOWS
#  if !defined(NOMINMAX)
#    define NOMINMAX
#  endif
#  include <windows.h>
#elif defined(__linux__) || defined(__APPLE__) || defined(__unix__)
#  define PLATFORM_POSIX
#  include <termios.h>
#  include <unistd.h>
#  include <sys/select.h>
#  include <fcntl.h>
#  include <sys/file.h>
#else
#  error "Unsupported platform"
#endif

// ═══════════════════════════════════════════════════════════════════════════
//  错误类别实现
//  将 serial_errc 枚举映射到 std::error_category，使其能与标准异常体系协作
// ═══════════════════════════════════════════════════════════════════════════

namespace {

class serial_category_impl : public std::error_category {
public:
    const char* name() const noexcept override { return "serial_port"; }

    std::string message(int ev) const override {
        switch (static_cast<serial_errc>(ev)) {
        case serial_errc::invalid_port:     return "invalid port name";
        case serial_errc::open_failed:      return "failed to open port";
        case serial_errc::configure_failed: return "failed to configure port";
        case serial_errc::read_failed:      return "read failed";
        case serial_errc::write_failed:     return "write failed";
        case serial_errc::timeout:          return "I/O timed out";
        case serial_errc::not_open:         return "port is not open";
        case serial_errc::already_open:     return "port is already open";
        default:                            return "unknown serial error";
        }
    }
};

const serial_category_impl serial_category_instance;

} // anonymous namespace

std::error_code make_error_code(serial_errc e) noexcept {
    return {static_cast<int>(e), serial_category_instance};
}

const std::error_category& serial_category() noexcept {
    return serial_category_instance;
}

// ═══════════════════════════════════════════════════════════════════════════
//  平台相关的句柄定义和辅助函数
//  每个平台的 serial_port::impl 结构体持有该平台的底层句柄
// ═══════════════════════════════════════════════════════════════════════════

#ifdef PLATFORM_WINDOWS

// ── Windows 实现 ──────────────────────────────────────────────────────────
//  impl 封装 HANDLE，析构时自动关闭（RAII）

struct serial_port::impl {
    HANDLE fd = INVALID_HANDLE_VALUE;

    ~impl() {
        if (fd != INVALID_HANDLE_VALUE) {
            CloseHandle(fd);
            fd = INVALID_HANDLE_VALUE;
        }
    }
};

// 将 baud_rate 枚举值转换为 Windows DCB 结构所需的波特率常量
static int win_baud(baud_rate b) {
    switch (b) {
    case baud_rate::br_110:     return CBR_110;
    case baud_rate::br_300:     return CBR_300;
    case baud_rate::br_600:     return CBR_600;
    case baud_rate::br_1200:    return CBR_1200;
    case baud_rate::br_2400:    return CBR_2400;
    case baud_rate::br_4800:    return CBR_4800;
    case baud_rate::br_9600:    return CBR_9600;
    case baud_rate::br_14400:   return CBR_14400;
    case baud_rate::br_19200:   return CBR_19200;
    case baud_rate::br_38400:   return CBR_38400;
    case baud_rate::br_57600:   return CBR_57600;
    case baud_rate::br_115200:  return CBR_115200;
    default:                    return CBR_115200;
    }
}

// 将 "COM3" 形式的端口名转换为 Windows 设备路径 "\\.\COM3"
static std::string win_port_name(const std::string& name) {
    if (name.find(R"(\\.\)") != std::string::npos)
        return name;
    return R"(\\.\)" + name;
}

#else // POSIX (Linux / macOS / BSD)

// ── POSIX 实现 ────────────────────────────────────────────────────────────
//  impl 封装文件描述符，析构时自动 close（RAII）

struct serial_port::impl {
    int fd = -1;

    ~impl() {
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
    }
};

// 条件编译：检查当前平台是否定义 B460800 / B921600
// Linux 通常支持，macOS 和 BSD 可能不支持
#ifndef B460800
#  define B460800 0
#endif
#ifndef B921600
#  define B921600 0
#endif

// 将 baud_rate 枚举值转换为 POSIX termios 所需的 speed_t 常量
static speed_t posix_baud(baud_rate b) {
    switch (b) {
    case baud_rate::br_110:     return B110;
    case baud_rate::br_300:     return B300;
    case baud_rate::br_600:     return B600;
    case baud_rate::br_1200:    return B1200;
    case baud_rate::br_2400:    return B2400;
    case baud_rate::br_4800:    return B4800;
    case baud_rate::br_9600:    return B9600;
    case baud_rate::br_14400:   return B14400;
    case baud_rate::br_19200:   return B19200;
    case baud_rate::br_38400:   return B38400;
    case baud_rate::br_57600:   return B57600;
    case baud_rate::br_115200:  return B115200;
    case baud_rate::br_230400:  return B230400;
    case baud_rate::br_460800:  return static_cast<speed_t>(B460800);
    case baud_rate::br_921600:  return static_cast<speed_t>(B921600);
    default:                    return B115200;
    }
}

// 设置 POSIX termios 的输入输出波特率
static int posix_set_baud(struct termios& tio, speed_t speed) {
    if (cfsetispeed(&tio, speed) != 0 || cfsetospeed(&tio, speed) != 0)
        return -1;
    return 0;
}

#endif

// ═══════════════════════════════════════════════════════════════════════════
//  实现辅助函数
//  将 serial_config 中的配置参数应用到已打开的串口句柄
// ═══════════════════════════════════════════════════════════════════════════

void serial_port::apply_config(const serial_config& cfg) {
    auto& p = *pimpl_;

    // ── Windows 平台：使用 DCB 结构配置串口 ──────────────────────────────
#ifdef PLATFORM_WINDOWS
    DCB dcb{};
    dcb.DCBlength = sizeof(DCB);
    if (!GetCommState(p.fd, &dcb))
        throw serial_port_error(serial_errc::configure_failed,
                                "GetCommState failed");

    dcb.BaudRate  = static_cast<DWORD>(win_baud(cfg.baud));
    dcb.ByteSize  = static_cast<BYTE>(cfg.data_bits);
    dcb.fBinary   = TRUE;
    dcb.fDtrControl   = DTR_CONTROL_ENABLE;
    dcb.fRtsControl   = RTS_CONTROL_ENABLE;

    // 配置校验位
    switch (cfg.parity_check) {
    case parity::none:  dcb.Parity = NOPARITY;  dcb.fParity = FALSE; break;
    case parity::odd:   dcb.Parity = ODDPARITY;  dcb.fParity = TRUE;  break;
    case parity::even:  dcb.Parity = EVENPARITY; dcb.fParity = TRUE;  break;
    case parity::mark:  dcb.Parity = MARKPARITY; dcb.fParity = TRUE;  break;
    case parity::space: dcb.Parity = SPACEPARITY;dcb.fParity = TRUE;  break;
    }

    // 配置停止位
    switch (cfg.stop) {
    case stop_bits::one:            dcb.StopBits = ONESTOPBIT;   break;
    case stop_bits::one_point_five: dcb.StopBits = ONE5STOPBITS; break;
    case stop_bits::two:            dcb.StopBits = TWOSTOPBITS;  break;
    }

    // 配置流控制
    switch (cfg.flow) {
    case flow_control::none:
        dcb.fOutxCtsFlow = FALSE;
        dcb.fOutxDsrFlow = FALSE;
        dcb.fInX = FALSE;
        dcb.fOutX = FALSE;
        break;
    case flow_control::software:
        dcb.fOutxCtsFlow = FALSE;
        dcb.fOutxDsrFlow = FALSE;
        dcb.fInX = TRUE;
        dcb.fOutX = TRUE;
        break;
    case flow_control::hardware:
        dcb.fOutxCtsFlow = TRUE;
        dcb.fOutxDsrFlow = TRUE;
        dcb.fInX = FALSE;
        dcb.fOutX = FALSE;
        dcb.fRtsControl = RTS_CONTROL_HANDSHAKE;
        break;
    }

    if (!SetCommState(p.fd, &dcb))
        throw serial_port_error(serial_errc::configure_failed,
                                "SetCommState failed");

    // 配置超时：阻塞模式（<0）/ 非阻塞模式（==0）/ 限时模式（>0）
    COMMTIMEOUTS to{};
    if (cfg.read_timeout.count() < 0) {
        to.ReadIntervalTimeout         = 0;
        to.ReadTotalTimeoutConstant    = 0;
        to.ReadTotalTimeoutMultiplier  = 0;
    } else if (cfg.read_timeout.count() == 0) {
        to.ReadIntervalTimeout         = MAXDWORD;
        to.ReadTotalTimeoutConstant    = 0;
        to.ReadTotalTimeoutMultiplier  = 0;
    } else {
        to.ReadIntervalTimeout         = 0;
        to.ReadTotalTimeoutConstant    = static_cast<DWORD>(cfg.read_timeout.count());
        to.ReadTotalTimeoutMultiplier  = 0;
    }

    if (cfg.write_timeout.count() < 0) {
        to.WriteTotalTimeoutConstant   = 0;
        to.WriteTotalTimeoutMultiplier = 0;
    } else {
        to.WriteTotalTimeoutConstant   = static_cast<DWORD>(cfg.write_timeout.count());
        to.WriteTotalTimeoutMultiplier = 0;
    }

    if (!SetCommTimeouts(p.fd, &to))
        throw serial_port_error(serial_errc::configure_failed,
                                "SetCommTimeouts failed");

    SetupComm(p.fd, 4096, 4096);

    // ── POSIX 平台：使用 termios 配置串口 ─────────────────────────────────
#else // POSIX
    struct termios tio{};
    if (tcgetattr(p.fd, &tio) != 0)
        throw serial_port_error(serial_errc::configure_failed,
                                "tcgetattr failed");

    // 设置为 raw 模式（原始输入输出，不经终端驱动处理）
    cfmakeraw(&tio);

    // 设置波特率
    speed_t speed = posix_baud(cfg.baud);
    if (posix_set_baud(tio, speed) != 0)
        throw serial_port_error(serial_errc::configure_failed,
                                "failed to set baud rate");

    // 设置数据位（5/6/7/8）
    tio.c_cflag &= ~CSIZE;
    switch (cfg.data_bits) {
    case 5: tio.c_cflag |= CS5; break;
    case 6: tio.c_cflag |= CS6; break;
    case 7: tio.c_cflag |= CS7; break;
    case 8: tio.c_cflag |= CS8; break;
    default: tio.c_cflag |= CS8; break;
    }

    // 设置校验位
    tio.c_cflag &= ~(PARENB | PARODD);
    tio.c_iflag &= ~(INPCK | ISTRIP);
    switch (cfg.parity_check) {
    case parity::none:
        break;
    case parity::even:
        tio.c_cflag |= PARENB;
        tio.c_iflag |= INPCK;
        break;
    case parity::odd:
        tio.c_cflag |= PARENB | PARODD;
        tio.c_iflag |= INPCK;
        break;
    case parity::mark:
        tio.c_cflag |= PARENB | PARODD;
        break;
    case parity::space:
        tio.c_cflag |= PARENB;
        break;
    }

    // 设置停止位
    if (cfg.stop == stop_bits::two || cfg.stop == stop_bits::one_point_five) {
        tio.c_cflag |= CSTOPB;
    } else {
        tio.c_cflag &= ~static_cast<tcflag_t>(CSTOPB);
    }

    // 设置流控制
    switch (cfg.flow) {
    case flow_control::none:
        tio.c_cflag &= ~static_cast<tcflag_t>(CRTSCTS);
        tio.c_iflag &= ~static_cast<tcflag_t>(IXON | IXOFF | IXANY);
        break;
    case flow_control::software:
        tio.c_cflag &= ~static_cast<tcflag_t>(CRTSCTS);
        tio.c_iflag |= (IXON | IXOFF | IXANY);
        break;
    case flow_control::hardware:
        tio.c_cflag |= CRTSCTS;
        tio.c_iflag &= ~static_cast<tcflag_t>(IXON | IXOFF | IXANY);
        break;
    }

    // 配置读取超时（通过 VMIN / VTIME 实现）
    //   阻塞模式（<0）：等待至少 1 字节
    //   非阻塞模式（==0）：立即返回
    //   限时模式（>0）：等待最多 N 毫秒（转换为 0.1 秒单位的 VTIME）
    if (cfg.read_timeout.count() < 0) {
        tio.c_cc[VMIN]  = 1;
        tio.c_cc[VTIME] = 0;
    } else if (cfg.read_timeout.count() == 0) {
        tio.c_cc[VMIN]  = 0;
        tio.c_cc[VTIME] = 0;
    } else {
        int ds = static_cast<int>(cfg.read_timeout.count() / 100);
        if (ds < 0) ds = 0;
        if (ds > 255) ds = 255;
        tio.c_cc[VMIN]  = 0;
        tio.c_cc[VTIME] = static_cast<cc_t>(ds);
    }

    // 确保串口不被终端控制信号干扰
    tio.c_cflag |= (CLOCAL | CREAD);
    tio.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO | ECHOE | ISIG);
    tio.c_oflag &= ~static_cast<tcflag_t>(OPOST);

    if (tcsetattr(p.fd, TCSANOW, &tio) != 0)
        throw serial_port_error(serial_errc::configure_failed,
                                "tcsetattr failed");

    tcflush(p.fd, TCIOFLUSH);
#endif
}

// ─── 构造 / 析构 ─────────────────────────────────────────────────────────

serial_port::serial_port() noexcept = default;

serial_port::serial_port(const std::string& port_name, serial_config cfg)
    : pimpl_(std::make_unique<impl>())
{
    open(port_name, std::move(cfg));
}

serial_port::~serial_port() noexcept = default;

// ─── 移动语义 ───────────────────────────────────────────────────────────────
//  转移底层句柄所有权，源对象仍处于可析构的有效空状态

void serial_port::move_from(serial_port& other) noexcept {
    pimpl_ = std::move(other.pimpl_);
    port_name_ = std::move(other.port_name_);
    config_ = other.config_;
    other.port_name_.clear();
    other.config_ = serial_config{};
}

serial_port::serial_port(serial_port&& other) noexcept {
    move_from(other);
}

serial_port& serial_port::operator=(serial_port&& other) noexcept {
    if (this != &other) {
        close();
        move_from(other);
    }
    return *this;
}

// ─── 打开 / 关闭 / 状态查询 ───────────────────────────────────────────────

void serial_port::open(const std::string& port_name, serial_config cfg) {
    // 如果已打开则先关闭，确保重新打开时状态干净
    if (is_open())
        close();

    if (port_name.empty())
        throw serial_port_error(serial_errc::invalid_port, "port name is empty");

    if (!pimpl_)
        pimpl_ = std::make_unique<impl>();

    // ── Windows：CreateFileA 打开串口设备 ─────────────────────────────────
#ifdef PLATFORM_WINDOWS
    std::string full_name = win_port_name(port_name);
    HANDLE h = CreateFileA(
        full_name.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED,
        nullptr);

    if (h == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        throw serial_port_error(serial_errc::open_failed,
                                "CreateFile failed: " + std::to_string(err));
    }

    pimpl_->fd = h;
#else
    // ── POSIX：open() 打开串口设备 ────────────────────────────────────────
    //  自动补全 /dev/ 前缀（如果端口名不以 / 开头）
    std::string path = port_name;
    if (path.find('/') == std::string::npos)
        path = "/dev/" + path;

    int fd = ::open(path.c_str(), O_RDWR | O_NOCTTY);
    if (fd < 0) {
        throw serial_port_error(serial_errc::open_failed,
                                "open failed: " + std::string(std::strerror(errno)));
    }

    pimpl_->fd = fd;
#endif

    port_name_ = port_name;
    config_ = cfg;

    apply_config(cfg);
}

void serial_port::close() noexcept {
    if (!is_open())
        return;

    // 关闭底层句柄，恢复初始状态
#ifdef PLATFORM_WINDOWS
    if (pimpl_ && pimpl_->fd != INVALID_HANDLE_VALUE) {
        CloseHandle(pimpl_->fd);
        pimpl_->fd = INVALID_HANDLE_VALUE;
    }
#else
    if (pimpl_ && pimpl_->fd >= 0) {
        ::close(pimpl_->fd);
        pimpl_->fd = -1;
    }
#endif

    port_name_.clear();
    config_ = serial_config{};
}

bool serial_port::is_open() const noexcept {
    // 检查底层句柄是否有效
    if (!pimpl_)
        return false;
#ifdef PLATFORM_WINDOWS
    return pimpl_->fd != INVALID_HANDLE_VALUE;
#else
    return pimpl_->fd >= 0;
#endif
}

// ─── 配置 ─────────────────────────────────────────────────────────────────

void serial_port::configure(const serial_config& cfg) {
    if (!is_open())
        throw serial_port_error(serial_errc::not_open,
                                "cannot configure: port not open");
    apply_config(cfg);
    config_ = cfg;
}

serial_config serial_port::current_config() const noexcept {
    return config_;
}

// ─── 读写操作 ─────────────────────────────────────────────────────────────

// 读取原始字节数据到缓冲区
//   参数: buf — 接收缓冲区指针，len — 期望读取的最大字节数
//   返回: 实际读取的字节数（可能小于 len）
//   超时: 由 config_.read_timeout 决定（阻塞 / 非阻塞 / 限时）

std::size_t serial_port::read(std::uint8_t* buf, std::size_t len) {
    if (!is_open())
        throw serial_port_error(serial_errc::not_open, "port not open");
    if (len == 0)
        return 0;

    // ── Windows：使用 OVERLAPPED（异步 I/O）实现带超时的读取 ────────────
#ifdef PLATFORM_WINDOWS
    OVERLAPPED ov{};
    ov.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent)
        throw serial_port_error(serial_errc::read_failed, "CreateEvent failed");

    DWORD read_len = 0;
    BOOL ok = ReadFile(pimpl_->fd, buf, static_cast<DWORD>(len), &read_len, &ov);

    // 如果操作异步挂起，则等待超时事件
    // 异步操作已挂起 — 等待完成或超时
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        DWORD timeout_ms = (config_.read_timeout.count() < 0)
                               ? INFINITE
                               : static_cast<DWORD>(config_.read_timeout.count());
        DWORD wait = WaitForSingleObject(ov.hEvent, timeout_ms);
        if (wait == WAIT_TIMEOUT) {
            CancelIo(pimpl_->fd);
            CloseHandle(ov.hEvent);
            throw serial_port_error(serial_errc::timeout, "read timeout");
        }
        ok = GetOverlappedResult(pimpl_->fd, &ov, &read_len, FALSE);
    }

    CloseHandle(ov.hEvent);

    if (!ok)
        throw serial_port_error(serial_errc::read_failed, "ReadFile failed");

    return static_cast<std::size_t>(read_len);

    // ── POSIX：使用 select() + read() 实现带超时的读取 ─────────────────────
#else // POSIX
    int fd = pimpl_->fd;

    // 非阻塞模式：直接调用 read()
    if (config_.read_timeout.count() == 0) {
        ssize_t n = ::read(fd, buf, len);
        if (n < 0)
            throw serial_port_error(serial_errc::read_failed, std::strerror(errno));
        return static_cast<std::size_t>(n);
    }

    // 阻塞或限时模式：先用 select() 检查可读性
    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(fd, &read_fds);

    struct timeval tv{};
    struct timeval* tvp = nullptr;

    if (config_.read_timeout.count() >= 0) {
        auto ms = config_.read_timeout.count();
        tv.tv_sec  = static_cast<long>(ms / 1000);
        tv.tv_usec = static_cast<long>((ms % 1000) * 1000);
        tvp = &tv;
    }

    int sel = select(fd + 1, &read_fds, nullptr, nullptr, tvp);
    if (sel < 0)
        throw serial_port_error(serial_errc::read_failed, std::strerror(errno));
    if (sel == 0)
        throw serial_port_error(serial_errc::timeout, "read timeout");

    // 有数据可读
    ssize_t n = ::read(fd, buf, len);
    if (n < 0)
        throw serial_port_error(serial_errc::read_failed, std::strerror(errno));

    return static_cast<std::size_t>(n);
#endif
}

// 逐字符读取直到遇到分隔符（默认 '\n'），返回整行字符串
std::string serial_port::read_line(char delim) {
    std::string line;
    char ch;
    while (true) {
        std::size_t n = read(reinterpret_cast<std::uint8_t*>(&ch), 1);
        if (n == 0)
            break;
        if (ch == delim)
            break;
        line.push_back(ch);
    }
    return line;
}

// 写入原始字节数据到串口
//   参数: data — 待发送数据的指针，len — 数据长度
//   返回: 实际写入的字节数
std::size_t serial_port::write(const std::uint8_t* data, std::size_t len) {
    if (!is_open())
        throw serial_port_error(serial_errc::not_open, "port not open");
    if (len == 0)
        return 0;

    // ── Windows：使用 OVERLAPPED 异步 I/O 写入 ───────────────────────────
#ifdef PLATFORM_WINDOWS
    OVERLAPPED ov{};
    ov.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent)
        throw serial_port_error(serial_errc::write_failed, "CreateEvent failed");

    DWORD written = 0;
    BOOL ok = WriteFile(pimpl_->fd, data, static_cast<DWORD>(len), &written, &ov);

    // 异步写入 — 若挂起则等待完成或超时
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        DWORD timeout_ms = (config_.write_timeout.count() < 0)
                               ? INFINITE
                               : static_cast<DWORD>(config_.write_timeout.count());
        DWORD wait = WaitForSingleObject(ov.hEvent, timeout_ms);
        if (wait == WAIT_TIMEOUT) {
            CancelIo(pimpl_->fd);
            CloseHandle(ov.hEvent);
            throw serial_port_error(serial_errc::timeout, "write timeout");
        }
        ok = GetOverlappedResult(pimpl_->fd, &ov, &written, FALSE);
    }

    CloseHandle(ov.hEvent);

    if (!ok)
        throw serial_port_error(serial_errc::write_failed, "WriteFile failed");

    return static_cast<std::size_t>(written);

    // ── POSIX：使用 select() + write() 实现带超时的写入 ────────────────────
#else // POSIX
    int fd = pimpl_->fd;

    // 非阻塞模式：直接调用 write()
    if (config_.write_timeout.count() == 0) {
        ssize_t n = ::write(fd, data, len);
        if (n < 0)
            throw serial_port_error(serial_errc::write_failed, std::strerror(errno));
        return static_cast<std::size_t>(n);
    }

    // 阻塞或限时模式：先用 select() 检查是否可写
    fd_set write_fds;
    FD_ZERO(&write_fds);
    FD_SET(fd, &write_fds);

    struct timeval tv{};
    struct timeval* tvp = nullptr;

    if (config_.write_timeout.count() >= 0) {
        auto ms = config_.write_timeout.count();
        tv.tv_sec  = static_cast<long>(ms / 1000);
        tv.tv_usec = static_cast<long>((ms % 1000) * 1000);
        tvp = &tv;
    }

    int sel = select(fd + 1, nullptr, &write_fds, nullptr, tvp);
    if (sel < 0)
        throw serial_port_error(serial_errc::write_failed, std::strerror(errno));
    if (sel == 0)
        throw serial_port_error(serial_errc::timeout, "write timeout");

    // 可写，执行写入
    ssize_t n = ::write(fd, data, len);
    if (n < 0)
        throw serial_port_error(serial_errc::write_failed, std::strerror(errno));

    return static_cast<std::size_t>(n);
#endif
}

// 重载：方便直接传入 std::string 进行写入
std::size_t serial_port::write(const std::string& s) {
    return write(reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
}

// ─── 清空缓冲区 ────────────────────────────────────────────────────────────
//  清空接收缓冲区（已接收但未读取的数据）
//  清空发送缓冲区（已写入但未发送的数据）
//  同时清空两者

void serial_port::flush_input() {
    if (!is_open())
        throw serial_port_error(serial_errc::not_open, "port not open");
#ifdef PLATFORM_WINDOWS
    PurgeComm(pimpl_->fd, PURGE_RXCLEAR);
#else
    tcflush(pimpl_->fd, TCIFLUSH);
#endif
}

void serial_port::flush_output() {
    if (!is_open())
        throw serial_port_error(serial_errc::not_open, "port not open");
#ifdef PLATFORM_WINDOWS
    PurgeComm(pimpl_->fd, PURGE_TXCLEAR);
#else
    tcflush(pimpl_->fd, TCOFLUSH);
#endif
}

void serial_port::flush_both() {
    if (!is_open())
        throw serial_port_error(serial_errc::not_open, "port not open");
#ifdef PLATFORM_WINDOWS
    PurgeComm(pimpl_->fd, PURGE_RXCLEAR | PURGE_TXCLEAR);
#else
    tcflush(pimpl_->fd, TCIOFLUSH);
#endif
}
