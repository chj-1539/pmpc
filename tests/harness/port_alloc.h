#ifndef PMPC_TEST_HARNESS_PORT_ALLOC_H
#define PMPC_TEST_HARNESS_PORT_ALLOC_H

//=============================================================================
// tests/harness/port_alloc.h — 端口 & loopback 辅助
//
// 目的：让协议级 e2e 测试（fake_modbus_slave / fake_pemp_client 等）不必
// 复制粘贴 bind(0) → listen → local_addr 那一套；同时可以避免多个测试
// 并行时争抢固定端口的 flake。
//
// 用法：
//   #include "harness/port_alloc.h"
//   using namespace pmpc::testing::harness;
//
//   auto server = MakeListener();          // bind 127.0.0.1:0, listen
//   uint16_t port = server.port;
//   // 测试代码 connect 到 "127.0.0.1:port"
//   server.sock 是 std::move'd 到调用者手里的 listener socket
//
// 依赖：include/socket.h + src/socket.cpp。调用者需自己确保 wsa_guard 已在
// 更高作用域构造（通常 fixture SetUp 里放一个）。
//=============================================================================

#include "socket.h"
#include <cstdint>
#include <string>

namespace pmpc {
namespace testing {
namespace harness {

// 已 bind + listen 的 loopback 监听 socket + 其分配到的端口
struct Listener {
    ::socket   sock;       // 已 listen 的 TCP socket
    uint16_t   port = 0;   // 内核分配的端口
    std::string host = "127.0.0.1";

    std::string endpoint() const {
        return host + ":" + std::to_string(port);
    }
};

// 在 127.0.0.1:0 上 bind + listen；backlog 默认 1。
// 调用方后续用 sock.accept() 接收客户端。
inline Listener MakeListener(int backlog = 1, const std::string& host = "127.0.0.1") {
    Listener L;
    L.host = host;
    L.sock.bind(socket_addr(host, 0));
    L.sock.listen(backlog);
    L.port = L.sock.local_addr().port();
    return L;
}

} // namespace harness
} // namespace testing
} // namespace pmpc

#endif // PMPC_TEST_HARNESS_PORT_ALLOC_H
