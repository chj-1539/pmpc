#ifndef SOCKET_H
#define SOCKET_H

#include <cstdint>
#include <memory>
#include <string>
#include <chrono>
#include <system_error>
#include <stdexcept>
#include <type_traits>
#include <vector>

// ─── Error types ────────────────────────────────────────────────────────────

enum class socket_errc {
    init_failed       = 1,
    create_failed     = 2,
    bind_failed       = 3,
    listen_failed     = 4,
    connect_failed    = 5,
    accept_failed     = 6,
    send_failed       = 7,
    recv_failed       = 8,
    resolve_failed    = 9,
    timeout           = 10,
    not_open          = 11,
    would_block       = 12,
    closed            = 13,
    shutdown_failed   = 14,
};

template <>
struct std::is_error_code_enum<socket_errc> : std::true_type {};

std::error_code make_error_code(socket_errc e) noexcept;
const std::error_category& socket_category() noexcept;

class socket_error : public std::system_error {
public:
    explicit socket_error(socket_errc ec, const std::string& what = "")
        : std::system_error(make_error_code(ec), what) {}
};

// ─── Protocol / address family ──────────────────────────────────────────────

enum class protocol_type {
    tcp,
    udp,
};

enum class address_family {
    unspecified,  // IPv4 + IPv6
    ipv4,
    ipv6,
};

// ─── Socket address (resolves host:port to system address) ──────────────────

class socket_addr {
public:
    socket_addr() = default;

    // Resolve a single address.  Throws socket_error on failure.
    socket_addr(const std::string& host, std::uint16_t port,
                protocol_type type  = protocol_type::tcp,
                address_family fam  = address_family::unspecified);

    // Construct from raw address storage (used internally by resolve methods).
    static socket_addr from_storage(const void* data, int len,
                                    std::string host, std::uint16_t port);

    // Resolve all matching addresses.
    static std::vector<socket_addr> resolve_all(
        const std::string& host, std::uint16_t port,
        protocol_type type  = protocol_type::tcp,
        address_family fam  = address_family::unspecified);

    // ── Accessors ──────────────────────────────────────────────────────────

    const std::string& host() const noexcept { return host_; }
    std::uint16_t      port() const noexcept { return port_; }
    std::string        to_string() const;
    bool               valid() const noexcept { return !host_.empty(); }
    address_family     family() const noexcept;

    bool operator==(const socket_addr& o) const noexcept;
    bool operator!=(const socket_addr& o) const noexcept { return !(*this == o); }

private:
    friend class socket;

    struct sockaddr_storage_ {
        std::uint8_t data[128]{};  // large enough for sockaddr_storage
    };

    std::string     host_;
    std::uint16_t   port_ = 0;
    int             addr_len_ = 0;
    sockaddr_storage_ addr_{};

    int  raw_len()        const noexcept { return addr_len_; }
    void* raw_ptr()             noexcept { return addr_.data; }
    const void* raw_ptr() const noexcept { return addr_.data; }
};

// ─── RAII socket ────────────────────────────────────────────────────────────

class socket {
public:
    // ── Construction / destruction ──────────────────────────────────────────

    socket() noexcept;
    ~socket() noexcept;

    socket(socket&& other) noexcept;
    socket& operator=(socket&& other) noexcept;

    socket(const socket&) = delete;
    socket& operator=(const socket&) = delete;

    // ── TCP client ─────────────────────────────────────────────────────────

    void connect(const socket_addr& addr);
    void connect(const std::string& host, std::uint16_t port);

    // ── TCP server ─────────────────────────────────────────────────────────

    void bind(const socket_addr& addr);
    void listen(int backlog = 128);

    // Accept one connection.  *peer is filled if non-null.
    socket accept(socket_addr* peer = nullptr);

    // ── UDP ────────────────────────────────────────────────────────────────

    void open_udp();
    void open_udp_bind(const socket_addr& addr);

    // ── I/O ────────────────────────────────────────────────────────────────

    std::size_t send(const std::uint8_t* data, std::size_t len, int flags = 0);
    std::size_t send(const std::string& s, int flags = 0);

    std::size_t recv(std::uint8_t* buf, std::size_t len, int flags = 0);

    std::size_t send_to(const std::uint8_t* data, std::size_t len,
                        const socket_addr& addr, int flags = 0);
    std::size_t recv_from(std::uint8_t* buf, std::size_t len,
                          socket_addr& addr, int flags = 0);

    // ── Options ────────────────────────────────────────────────────────────

    void set_non_blocking(bool enable);
    void set_reuse_addr(bool enable);
    void set_recv_timeout(std::chrono::milliseconds ms);
    void set_send_timeout(std::chrono::milliseconds ms);

    // ── State ──────────────────────────────────────────────────────────────

    bool is_open() const noexcept;
    void close() noexcept;
    void shutdown(int how = 2);  // 0=read, 1=write, 2=both

    // ── Information ────────────────────────────────────────────────────────

    protocol_type type() const noexcept;
    socket_addr   local_addr() const;

private:
    struct impl;
    std::unique_ptr<impl> pimpl_;

    void move_from(socket& other) noexcept;
};

// ─── WSA initialisation guard (Windows only, no-op elsewhere) ───────────────

class wsa_guard {
public:
    wsa_guard();
    ~wsa_guard() noexcept;

    wsa_guard(const wsa_guard&) = delete;
    wsa_guard& operator=(const wsa_guard&) = delete;

private:
    struct impl;
    std::unique_ptr<impl> pimpl_;
};

#endif // SOCKET_H
