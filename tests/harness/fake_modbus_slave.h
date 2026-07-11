#ifndef PMPC_TEST_HARNESS_FAKE_MODBUS_SLAVE_H
#define PMPC_TEST_HARNESS_FAKE_MODBUS_SLAVE_H

//=============================================================================
// tests/harness/fake_modbus_slave.h — Loopback Modbus TCP 从站（测试用）
//
// 只支持刚够 modbus_tcp_master 单元测试用的最小子集：
//   FC 05 (Write Single Coil)   → echo request（Modbus 规范：正常应答等于请求）
//
// 用法：
//   using namespace pmpc::testing::harness;
//   FakeModbusSlave slave;
//   slave.Start();                        // bind + listen + 后台 accept
//   uint16_t port = slave.Port();
//   // ... 测试驱动 ModbusTcpMaster 的 socket 连到 127.0.0.1:port ...
//   auto reqs = slave.TakeRequests();     // 拿到所有已收到的请求，供断言用
//   slave.Stop();
//
// 记录的每个请求包含：mbap transId + PDU 全部字节。测试通过 `reqs.size()` 断言
// "两次相同 SetDoMaster 只落地一次线上写请求"来验证 bug #5 竞态修复。
//=============================================================================

#include "socket.h"
#include "harness/port_alloc.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace pmpc {
namespace testing {
namespace harness {

struct ModbusRequest {
    uint16_t              transId = 0;
    uint8_t               stationId = 0;
    uint8_t               func = 0;
    std::vector<uint8_t>  pdu;   // 完整 PDU（func + payload）
};

class FakeModbusSlave {
public:
    FakeModbusSlave() = default;
    ~FakeModbusSlave() { Stop(); }

    void Start() {
        listener_ = MakeListener(/*backlog=*/1);
        running_ = true;
        thr_ = std::thread([this]() { AcceptLoop(); });
    }

    uint16_t Port() const { return listener_.port; }

    // 取走并清空已收到的请求列表（线程安全）
    std::vector<ModbusRequest> TakeRequests() {
        std::lock_guard<std::mutex> lock(mtx_);
        auto out = std::move(reqs_);
        reqs_.clear();
        return out;
    }

    size_t RequestCount() {
        std::lock_guard<std::mutex> lock(mtx_);
        return reqs_.size();
    }

    void Stop() {
        if (!running_.exchange(false)) return;
        try { listener_.sock.close(); } catch (...) {}
        {
            std::lock_guard<std::mutex> lock(clientMtx_);
            try { clientSock_.close(); } catch (...) {}
        }
        if (thr_.joinable()) thr_.join();
    }

private:
    void AcceptLoop() {
        while (running_) {
            socket_addr peer;
            socket client;
            try { client = listener_.sock.accept(&peer); }
            catch (...) { return; }
            {
                std::lock_guard<std::mutex> lock(clientMtx_);
                clientSock_ = std::move(client);
            }
            HandleClient();
        }
    }

    void HandleClient() {
        // MBAP header 7 字节: transId(2) protoId(2) len(2) unitId(1)
        // 之后是 PDU: funcCode(1) + payload
        std::lock_guard<std::mutex> lock(clientMtx_);
        clientSock_.set_recv_timeout(std::chrono::milliseconds(500));
        while (running_) {
            uint8_t mbap[7];
            size_t got = 0;
            try {
                while (got < 7 && running_) {
                    size_t n = clientSock_.recv(mbap + got, 7 - got);
                    if (n == 0) return;   // peer close
                    got += n;
                }
            } catch (const socket_error&) {
                return;  // timeout / disconnect
            }
            if (got < 7) return;
            uint16_t transId = static_cast<uint16_t>(
                (static_cast<unsigned>(mbap[0]) << 8) | mbap[1]);
            uint16_t plen    = static_cast<uint16_t>(
                (static_cast<unsigned>(mbap[4]) << 8) | mbap[5]);
            uint8_t  station = mbap[6];
            if (plen < 2 || plen > 250) return;

            size_t pduLen = static_cast<size_t>(plen - 1);  // 减去 unitId
            std::vector<uint8_t> pdu(pduLen);
            got = 0;
            try {
                while (got < pduLen && running_) {
                    size_t n = clientSock_.recv(pdu.data() + got, pduLen - got);
                    if (n == 0) return;
                    got += n;
                }
            } catch (const socket_error&) {
                return;
            }
            if (got < pduLen) return;

            {
                std::lock_guard<std::mutex> lk(mtx_);
                ModbusRequest req;
                req.transId   = transId;
                req.stationId = station;
                req.func      = pdu[0];
                req.pdu       = pdu;
                reqs_.push_back(std::move(req));
            }

            // 正常应答：MBAP echo + PDU echo（对 FC05/06 是规范应答）
            std::vector<uint8_t> resp;
            resp.reserve(7 + pdu.size());
            resp.insert(resp.end(), mbap, mbap + 7);
            resp.insert(resp.end(), pdu.begin(), pdu.end());
            try {
                size_t sent = 0;
                while (sent < resp.size() && running_) {
                    sent += clientSock_.send(resp.data() + sent, resp.size() - sent);
                }
            } catch (const socket_error&) { return; }
        }
    }

    Listener            listener_;
    std::atomic<bool>   running_{false};
    std::thread         thr_;
    std::mutex          clientMtx_;
    socket              clientSock_;
    std::mutex          mtx_;
    std::vector<ModbusRequest> reqs_;
};

} // namespace harness
} // namespace testing
} // namespace pmpc

#endif // PMPC_TEST_HARNESS_FAKE_MODBUS_SLAVE_H
