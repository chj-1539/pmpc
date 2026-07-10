#include "socket.h"

#include <cstring>
#include <cerrno>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#  define PLATFORM_WINDOWS
#  if !defined(NOMINMAX)
#    define NOMINMAX
#  endif
#  if !defined(WIN32_LEAN_AND_MEAN)
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifdef _WIN32_WINNT
#    undef _WIN32_WINNT
#  endif
#  define _WIN32_WINNT 0x0600
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  ifdef _MSC_VER
#    pragma comment(lib, "ws2_32.lib")
#  endif
#elif defined(__linux__) || defined(__APPLE__) || defined(__unix__)
#  define PLATFORM_POSIX
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <arpa/inet.h>
#  include <netdb.h>
#  include <unistd.h>
#  include <fcntl.h>
#  include <sys/time.h>
#else
#  error "Unsupported platform"
#endif

#ifdef PLATFORM_WINDOWS
using socket_handle_t = SOCKET;
static constexpr socket_handle_t invalid_socket = INVALID_SOCKET;
static int   get_last_error()    noexcept { return WSAGetLastError(); }
static std::string last_error_string() {
    return "WSA error " + std::to_string(WSAGetLastError());
}
#else
using socket_handle_t = int;
static constexpr socket_handle_t invalid_socket = -1;
static int   get_last_error()    noexcept { return errno; }
static std::string last_error_string() {
    return std::strerror(errno);
}
#endif

namespace {
class socket_category_impl : public std::error_category {
public:
    const char* name() const noexcept override { return "socket"; }
    std::string message(int ev) const override {
        switch (static_cast<socket_errc>(ev)) {
        case socket_errc::init_failed:     return "socket initialisation failed";
        case socket_errc::create_failed:   return "socket creation failed";
        case socket_errc::bind_failed:     return "bind failed";
        case socket_errc::listen_failed:   return "listen failed";
        case socket_errc::connect_failed:  return "connect failed";
        case socket_errc::accept_failed:   return "accept failed";
        case socket_errc::send_failed:     return "send failed";
        case socket_errc::recv_failed:     return "recv failed";
        case socket_errc::resolve_failed:  return "address resolution failed";
        case socket_errc::timeout:         return "I/O timed out";
        case socket_errc::not_open:        return "socket is not open";
        case socket_errc::would_block:     return "operation would block";
        case socket_errc::shutdown_failed: return "shutdown failed";
        case socket_errc::closed:          return "connection closed";
        default:                           return "unknown socket error";
        }
    }
};
const socket_category_impl socket_category_instance;
}

std::error_code make_error_code(socket_errc e) noexcept {
    return {static_cast<int>(e), socket_category_instance};
}
const std::error_category& socket_category() noexcept {
    return socket_category_instance;
}

static void throw_with_errno(socket_errc ec, const char* prefix) {
    throw socket_error(ec, std::string(prefix) + ": " + last_error_string());
}

// Address-to-string via getnameinfo (portable, avoids inet_ntop).
static std::string addr_to_string(const void* addr_raw, int addr_len) {
    if (addr_len <= 0) return "";
    char host[NI_MAXHOST]{};
    if (::getnameinfo(static_cast<const struct sockaddr*>(addr_raw),
                      static_cast<socklen_t>(addr_len),
                      host, sizeof(host), nullptr, 0, NI_NUMERICHOST) != 0)
        return "";
    return host;
}

static int to_ai_family(address_family fam) noexcept {
    switch (fam) {
    case address_family::ipv4:        return AF_INET;
    case address_family::ipv6:        return AF_INET6;
    case address_family::unspecified: return AF_UNSPEC;
    }
    return AF_UNSPEC;
}

static int to_ai_socktype(protocol_type type) noexcept {
    return (type == protocol_type::udp) ? SOCK_DGRAM : SOCK_STREAM;
}

static int to_ai_protocol(protocol_type type) noexcept {
    return (type == protocol_type::udp) ? IPPROTO_UDP : IPPROTO_TCP;
}

static address_family from_ai_family(int af) noexcept {
    switch (af) {
    case AF_INET:  return address_family::ipv4;
    case AF_INET6: return address_family::ipv6;
    default:       return address_family::unspecified;
    }
}

socket_addr socket_addr::from_storage(const void* data, int len,
                                       std::string host, std::uint16_t port) {
    socket_addr addr;
    addr.host_     = std::move(host);
    addr.port_     = port;
    addr.addr_len_ = len;
    if (len > 0 && len <= static_cast<int>(sizeof(addr.addr_.data)))
        std::memcpy(addr.addr_.data, data, static_cast<std::size_t>(len));
    return addr;
}

static void resolve_impl(const std::string& host, std::uint16_t port,
                          int socktype, int protocol, int family,
                          std::vector<socket_addr>& out)
{
    struct addrinfo hints{};
    hints.ai_family   = family;
    hints.ai_socktype = socktype;
    hints.ai_protocol = protocol;
    std::string port_str = std::to_string(port);
    struct addrinfo* res = nullptr;
    int rc = ::getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res);
    if (rc != 0) {
#ifdef PLATFORM_WINDOWS
        throw socket_error(socket_errc::resolve_failed,
            "getaddrinfo: WSA error " + std::to_string(rc));
#else
        throw socket_error(socket_errc::resolve_failed,
            "getaddrinfo: " + std::string(gai_strerror(rc)));
#endif
    }
    for (auto cur = res; cur; cur = cur->ai_next) {
        out.push_back(socket_addr::from_storage(
            cur->ai_addr, static_cast<int>(cur->ai_addrlen), host, port));
    }
    ::freeaddrinfo(res);
}

socket_addr::socket_addr(const std::string& host, std::uint16_t port,
                         protocol_type type, address_family fam) {
    std::vector<socket_addr> results;
    resolve_impl(host, port, to_ai_socktype(type), to_ai_protocol(type),
                 to_ai_family(fam), results);
    if (results.empty())
        throw socket_error(socket_errc::resolve_failed,
            host + ":" + std::to_string(port) + " resolved to nothing");
    *this = std::move(results.front());
}

std::vector<socket_addr> socket_addr::resolve_all(
    const std::string& host, std::uint16_t port,
    protocol_type type, address_family fam) {
    std::vector<socket_addr> results;
    resolve_impl(host, port, to_ai_socktype(type), to_ai_protocol(type),
                 to_ai_family(fam), results);
    return results;
}

std::string socket_addr::to_string() const {
    if (!valid()) return "<invalid>";
    std::string r = host_;
    std::string ip = addr_to_string(addr_.data, addr_len_);
    if (!ip.empty() && ip != host_) r += " (" + ip + ")";
    r += ":" + std::to_string(port_);
    return r;
}

address_family socket_addr::family() const noexcept {
    if (addr_len_ < static_cast<int>(sizeof(struct sockaddr_in)))
        return address_family::unspecified;
    return from_ai_family(
        reinterpret_cast<const struct sockaddr_in*>(addr_.data)->sin_family);
}

bool socket_addr::operator==(const socket_addr& o) const noexcept {
    if (host_ != o.host_ || port_ != o.port_ || addr_len_ != o.addr_len_)
        return false;
    return std::memcmp(addr_.data, o.addr_.data, addr_len_) == 0;
}

struct socket::impl {
    socket_handle_t fd = invalid_socket;
    protocol_type   type = protocol_type::tcp;
    ~impl() {
        if (fd != invalid_socket) {
#ifdef PLATFORM_WINDOWS
            ::closesocket(fd);
#else
            ::close(fd);
#endif
        }
    }
};

#ifdef PLATFORM_WINDOWS
struct wsa_guard::impl {
    WSADATA wsa{};
    bool ok = false;
    impl() {
        int rc = WSAStartup(MAKEWORD(2, 2), &wsa);
        ok = (rc == 0);
        if (!ok)
            throw socket_error(socket_errc::init_failed,
                               "WSAStartup failed: " + std::to_string(rc));
    }
    ~impl() noexcept { if (ok) WSACleanup(); }
};
#else
struct wsa_guard::impl {};
#endif

wsa_guard::wsa_guard() : pimpl_(std::make_unique<impl>()) {}
wsa_guard::~wsa_guard() noexcept {}

socket::socket() noexcept {}
socket::~socket() noexcept {}

void socket::move_from(socket& other) noexcept {
    pimpl_ = std::move(other.pimpl_);
    other.pimpl_ = std::make_unique<impl>();
}

socket::socket(socket&& other) noexcept { move_from(other); }

auto socket::operator=(socket&& other) noexcept -> socket& {
    if (this != &other) { close(); move_from(other); }
    return *this;
}

void socket::connect(const socket_addr& addr) {
    close();
    if (!pimpl_) pimpl_ = std::make_unique<impl>();
    int family = AF_UNSPEC;
    if (addr.addr_len_ > 0)
        family = reinterpret_cast<const struct sockaddr_in*>(addr.raw_ptr())->sin_family;
    socket_handle_t fd = ::socket(family, SOCK_STREAM, IPPROTO_TCP);
    if (fd == invalid_socket) throw_with_errno(socket_errc::create_failed, "socket");
    pimpl_->fd = fd; pimpl_->type = protocol_type::tcp;
    if (::connect(pimpl_->fd,
                  static_cast<const struct sockaddr*>(addr.raw_ptr()),
                  static_cast<socklen_t>(addr.addr_len_)) != 0) {
        close();
        throw_with_errno(socket_errc::connect_failed, "connect");
    }
}

void socket::connect(const std::string& host, std::uint16_t port) {
    connect(socket_addr(host, port, protocol_type::tcp));
}

void socket::bind(const socket_addr& addr) {
    close();
    if (!pimpl_) pimpl_ = std::make_unique<impl>();
    int family = AF_UNSPEC;
    if (addr.addr_len_ > 0)
        family = reinterpret_cast<const struct sockaddr_in*>(addr.raw_ptr())->sin_family;
    socket_handle_t fd = ::socket(family, SOCK_STREAM, IPPROTO_TCP);
    if (fd == invalid_socket) throw_with_errno(socket_errc::create_failed, "socket");
    pimpl_->fd = fd; pimpl_->type = protocol_type::tcp;
    set_reuse_addr(true);
    if (::bind(pimpl_->fd,
               static_cast<const struct sockaddr*>(addr.raw_ptr()),
               static_cast<socklen_t>(addr.addr_len_)) != 0) {
        close();
        throw_with_errno(socket_errc::bind_failed, "bind");
    }
}

void socket::listen(int backlog) {
    if (!is_open()) throw socket_error(socket_errc::not_open, "socket not open");
    if (::listen(pimpl_->fd, backlog) != 0) {
        close();
        throw_with_errno(socket_errc::listen_failed, "listen");
    }
}

auto socket::accept(socket_addr* peer) -> socket {
    if (!is_open()) throw socket_error(socket_errc::not_open, "socket not open");
    socket client;
    if (!client.pimpl_) client.pimpl_ = std::make_unique<impl>();
    socket_addr tmp;
    tmp.addr_len_ = sizeof(tmp.addr_.data);
    if (peer) peer->addr_len_ = sizeof(peer->addr_.data);
    socket_addr* ap = peer ? peer : &tmp;
    socket_handle_t cf = ::accept(pimpl_->fd,
        reinterpret_cast<struct sockaddr*>(ap->raw_ptr()),
        reinterpret_cast<socklen_t*>(&ap->addr_len_));
    if (cf == invalid_socket) throw_with_errno(socket_errc::accept_failed, "accept");
    client.pimpl_->fd = cf;
    client.pimpl_->type = protocol_type::tcp;
    if (peer) {
        peer->host_ = addr_to_string(peer->addr_.data, peer->addr_len_);
        // 从 sockaddr 中提取端口号
        if (peer->addr_len_ >= static_cast<int>(sizeof(struct sockaddr_in))) {
            auto* sin = reinterpret_cast<struct sockaddr_in*>(peer->addr_.data);
            if (sin->sin_family == AF_INET) {
                peer->port_ = ntohs(sin->sin_port);
            }
        }
    }
    return client;
}

void socket::open_udp() {
    close();
    if (!pimpl_) pimpl_ = std::make_unique<impl>();
    socket_handle_t fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd == invalid_socket) throw_with_errno(socket_errc::create_failed, "socket");
    pimpl_->fd = fd; pimpl_->type = protocol_type::udp;
}

void socket::open_udp_bind(const socket_addr& addr) {
    open_udp();
    if (::bind(pimpl_->fd,
               static_cast<const struct sockaddr*>(addr.raw_ptr()),
               static_cast<socklen_t>(addr.addr_len_)) != 0) {
        close();
        throw_with_errno(socket_errc::bind_failed, "bind");
    }
}

std::size_t socket::send(const std::uint8_t* data, std::size_t len, int flags) {
    if (!is_open()) throw socket_error(socket_errc::not_open, "socket not open");
#ifdef MSG_NOSIGNAL
    flags |= MSG_NOSIGNAL;
#endif
    ssize_t n = ::send(pimpl_->fd, reinterpret_cast<const char*>(data),
                       static_cast<int>(len), flags);
    if (n < 0) {
        int e = get_last_error();
#ifdef PLATFORM_WINDOWS
        if (e == WSAEWOULDBLOCK) throw socket_error(socket_errc::would_block, "send would block");
        if (e == WSAETIMEDOUT)   throw socket_error(socket_errc::timeout, "send timeout");
#else
        if (e == EAGAIN || e == EWOULDBLOCK)
            throw socket_error(socket_errc::would_block, "send would block");
#endif
        throw_with_errno(socket_errc::send_failed, "send");
    }
    return static_cast<std::size_t>(n);
}

std::size_t socket::send(const std::string& s, int flags) {
    return send(reinterpret_cast<const std::uint8_t*>(s.data()), s.size(), flags);
}

std::size_t socket::recv(std::uint8_t* buf, std::size_t len, int flags) {
    if (!is_open()) throw socket_error(socket_errc::not_open, "socket not open");
    ssize_t n = ::recv(pimpl_->fd, reinterpret_cast<char*>(buf),
                       static_cast<int>(len), flags);
    if (n < 0) {
        int e = get_last_error();
#ifdef PLATFORM_WINDOWS
        if (e == WSAEWOULDBLOCK) throw socket_error(socket_errc::would_block, "recv would block");
        if (e == WSAETIMEDOUT)   throw socket_error(socket_errc::timeout, "recv timeout");
#else
        if (e == EAGAIN || e == EWOULDBLOCK)
            throw socket_error(socket_errc::would_block, "recv would block");
#endif
        throw_with_errno(socket_errc::recv_failed, "recv");
    }
    if (n == 0) return 0;  // 连接正常关闭 (EOF), 非错误
    return static_cast<std::size_t>(n);
}

std::size_t socket::send_to(const std::uint8_t* data, std::size_t len,
                            const socket_addr& addr, int flags) {
    if (!is_open()) throw socket_error(socket_errc::not_open, "socket not open");
#ifdef MSG_NOSIGNAL
    flags |= MSG_NOSIGNAL;
#endif
    ssize_t n = ::sendto(pimpl_->fd, reinterpret_cast<const char*>(data),
                         static_cast<int>(len), flags,
                         static_cast<const struct sockaddr*>(addr.raw_ptr()),
                         static_cast<socklen_t>(addr.addr_len_));
    if (n < 0) {
        int e = get_last_error();
#ifdef PLATFORM_WINDOWS
        if (e == WSAEWOULDBLOCK) throw socket_error(socket_errc::would_block, "sendto would block");
#else
        if (e == EAGAIN || e == EWOULDBLOCK)
            throw socket_error(socket_errc::would_block, "sendto would block");
#endif
        throw_with_errno(socket_errc::send_failed, "sendto");
    }
    return static_cast<std::size_t>(n);
}

std::size_t socket::recv_from(std::uint8_t* buf, std::size_t len,
                              socket_addr& addr, int flags) {
    if (!is_open()) throw socket_error(socket_errc::not_open, "socket not open");
    addr.addr_len_ = sizeof(addr.addr_.data);
    ssize_t n = ::recvfrom(pimpl_->fd, reinterpret_cast<char*>(buf),
                           static_cast<int>(len), flags,
                           reinterpret_cast<struct sockaddr*>(addr.raw_ptr()),
                           reinterpret_cast<socklen_t*>(&addr.addr_len_));
    if (n < 0) {
        int e = get_last_error();
#ifdef PLATFORM_WINDOWS
        if (e == WSAEWOULDBLOCK) throw socket_error(socket_errc::would_block, "recvfrom would block");
        if (e == WSAETIMEDOUT)   throw socket_error(socket_errc::timeout, "recvfrom timeout");
#else
        if (e == EAGAIN || e == EWOULDBLOCK)
            throw socket_error(socket_errc::would_block, "recvfrom would block");
#endif
        throw_with_errno(socket_errc::recv_failed, "recvfrom");
    }
    if (n == 0) throw socket_error(socket_errc::closed, "recvfrom: connection closed");
    return static_cast<std::size_t>(n);
}

void socket::set_non_blocking(bool enable) {
    if (!is_open()) throw socket_error(socket_errc::not_open, "socket not open");
#ifdef PLATFORM_WINDOWS
    u_long mode = enable ? 1 : 0;
    if (ioctlsocket(pimpl_->fd, FIONBIO, &mode) != 0)
        throw_with_errno(socket_errc::create_failed, "ioctlsocket(FIONBIO)");
#else
    int flags = ::fcntl(pimpl_->fd, F_GETFL, 0);
    if (flags < 0) throw_with_errno(socket_errc::create_failed, "fcntl(F_GETFL)");
    if (enable) flags |= O_NONBLOCK; else flags &= ~O_NONBLOCK;
    if (::fcntl(pimpl_->fd, F_SETFL, flags) < 0)
        throw_with_errno(socket_errc::create_failed, "fcntl(F_SETFL)");
#endif
}

void socket::set_reuse_addr(bool enable) {
    if (!is_open()) throw socket_error(socket_errc::not_open, "socket not open");
    int opt = enable ? 1 : 0;
    if (::setsockopt(pimpl_->fd, SOL_SOCKET, SO_REUSEADDR,
                     reinterpret_cast<const char*>(&opt), sizeof(opt)) != 0)
        throw_with_errno(socket_errc::create_failed, "setsockopt(SO_REUSEADDR)");
}

void socket::set_recv_timeout(std::chrono::milliseconds ms) {
    if (!is_open()) throw socket_error(socket_errc::not_open, "socket not open");
#ifdef PLATFORM_WINDOWS
    DWORD tv = static_cast<DWORD>(ms.count());
#else
    struct timeval tv{};
    tv.tv_sec = static_cast<long>(ms.count() / 1000);
    tv.tv_usec = static_cast<long>((ms.count() % 1000) * 1000);
#endif
    if (::setsockopt(pimpl_->fd, SOL_SOCKET, SO_RCVTIMEO,
                     reinterpret_cast<const char*>(&tv), sizeof(tv)) != 0)
        throw_with_errno(socket_errc::create_failed, "setsockopt(SO_RCVTIMEO)");
}

void socket::set_send_timeout(std::chrono::milliseconds ms) {
    if (!is_open()) throw socket_error(socket_errc::not_open, "socket not open");
#ifdef PLATFORM_WINDOWS
    DWORD tv = static_cast<DWORD>(ms.count());
#else
    struct timeval tv{};
    tv.tv_sec = static_cast<long>(ms.count() / 1000);
    tv.tv_usec = static_cast<long>((ms.count() % 1000) * 1000);
#endif
    if (::setsockopt(pimpl_->fd, SOL_SOCKET, SO_SNDTIMEO,
                     reinterpret_cast<const char*>(&tv), sizeof(tv)) != 0)
        throw_with_errno(socket_errc::create_failed, "setsockopt(SO_SNDTIMEO)");
}

bool socket::is_open() const noexcept {
    return pimpl_ && pimpl_->fd != invalid_socket;
}

void socket::close() noexcept {
    if (!is_open()) return;
#ifdef PLATFORM_WINDOWS
    ::closesocket(pimpl_->fd);
#else
    ::close(pimpl_->fd);
#endif
    pimpl_->fd = invalid_socket;
    pimpl_->type = protocol_type::tcp;
}

void socket::shutdown(int how) {
    if (!is_open()) return;
    int hn = 0;
#ifdef PLATFORM_WINDOWS
    hn = how;
#else
    switch (how) {
    case 0:  hn = SHUT_RD;   break;
    case 1:  hn = SHUT_WR;   break;
    default: hn = SHUT_RDWR; break;
    }
#endif
    if (::shutdown(pimpl_->fd, hn) != 0) {
        int e = get_last_error();
#ifdef PLATFORM_WINDOWS
        if (e != WSAENOTCONN)
#else
        if (e != ENOTCONN)
#endif
            throw_with_errno(socket_errc::shutdown_failed, "shutdown");
    }
}

protocol_type socket::type() const noexcept {
    return pimpl_ ? pimpl_->type : protocol_type::tcp;
}

socket_addr socket::local_addr() const {
    if (!is_open()) throw socket_error(socket_errc::not_open, "socket not open");
    socket_addr addr;
    addr.addr_len_ = sizeof(addr.addr_.data);
    if (::getsockname(pimpl_->fd,
                      reinterpret_cast<struct sockaddr*>(addr.raw_ptr()),
                      reinterpret_cast<socklen_t*>(&addr.addr_len_)) != 0)
        throw_with_errno(socket_errc::create_failed, "getsockname");
    if (addr.addr_len_ >= static_cast<int>(sizeof(struct sockaddr_in))) {
        auto* sin = reinterpret_cast<struct sockaddr_in*>(addr.raw_ptr());
        if (sin->sin_family == AF_INET) {
            addr.port_ = ntohs(sin->sin_port);
            addr.host_ = addr_to_string(addr.raw_ptr(), addr.addr_len_);
        } else if (sin->sin_family == AF_INET6 &&
                   addr.addr_len_ >= static_cast<int>(sizeof(struct sockaddr_in6))) {
            auto* sin6 = reinterpret_cast<struct sockaddr_in6*>(addr.raw_ptr());
            addr.port_ = ntohs(sin6->sin6_port);
            addr.host_ = addr_to_string(addr.raw_ptr(), addr.addr_len_);
        }
    }
    return addr;
}
