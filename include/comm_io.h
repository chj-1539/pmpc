#ifndef COMM_IO_H
#define COMM_IO_H

//=============================================================================
// comm_io.h — 串口/TCP 统一 IO 封装
//
// 根据 port_name 格式自动选择：
//   包含 ":"  → TCP (socket)
//   否则      → 串口 (serial_port)
//=============================================================================

#include "socket.h"
#include "serial_port.h"
#include <string>
#include <memory>
#include <iostream>

/// 判断是否为 TCP 地址 (ip:port 格式)
inline bool IsTcpAddr(const std::string& s) {
    return s.find(':') != std::string::npos && s.find('\\') == std::string::npos
           && s.find('/') == std::string::npos;
}

/// 统一 IO 接口，包装 serial_port 和 socket
struct CommIO {
    enum Type { SERIAL, TCP } type = SERIAL;
    std::unique_ptr<serial_port> sp;
    std::unique_ptr<socket> sock;

    CommIO() = default;

    /// 返回 true 表示连接/打开成功
    bool open(const std::string& portName, int baud = 9600,
              const std::string& parity = "even", int dataBits = 8,
              int stopBits = 1, int timeoutMs = 1000) {
        if (IsTcpAddr(portName)) {
            type = TCP;
            sock = std::make_unique<socket>();
            size_t colon = portName.find(':');
            std::string host = portName.substr(0, colon);
            int port = 0;
            try { port = std::stoi(portName.substr(colon + 1)); }
            catch (...) { std::cerr << "[CommIO] Bad port: " << portName << std::endl; return false; }
            try {
                sock->connect(host, static_cast<uint16_t>(port));
                sock->set_recv_timeout(std::chrono::milliseconds(timeoutMs));
                return true;
            } catch (const std::exception& e) {
                std::cerr << "[CommIO] TCP connect " << host << ":" << port << " failed: " << e.what() << std::endl;
                return false;
            }
        } else {
            type = SERIAL;
            sp = std::make_unique<serial_port>();
            serial_config scfg;
            scfg.baud = static_cast<baud_rate>(baud);
            if (parity == "even") scfg.parity_check = parity::even;
            else if (parity == "odd") scfg.parity_check = parity::odd;
            else scfg.parity_check = parity::none;
            scfg.data_bits = dataBits;
            scfg.stop = (stopBits == 2) ? stop_bits::two : stop_bits::one;
            scfg.read_timeout = std::chrono::milliseconds(timeoutMs);
            try {
                sp->open(portName, scfg);
                return true;
            } catch (const std::exception& e) {
                std::cerr << "[CommIO] Serial open " << portName << " failed: " << e.what() << std::endl;
                return false;
            }
        }
    }

    void close() {
        if (type == SERIAL && sp) try { sp->close(); } catch (...) {}
        if (type == TCP && sock) try { sock->close(); } catch (...) {}
    }

    size_t read(uint8_t* buf, size_t len) {
        if (type == SERIAL && sp) return sp->read(buf, len);
        if (type == TCP && sock) return sock->recv(buf, len);
        return 0;
    }

    void write(const uint8_t* data, size_t len) {
        if (type == SERIAL && sp) { sp->write(data, len); return; }
        if (type == TCP && sock && len > 0) {
            size_t total = 0;
            while (total < len) {
                size_t n = sock->send(data + total, len - total);
                if (n == 0) break;
                total += n;
            }
        }
    }

    bool isOpen() const {
        if (type == SERIAL && sp) return sp->is_open();
        if (type == TCP && sock) return sock->is_open();
        return false;
    }

    void setReadTimeout(int ms) {
        if (type == TCP && sock)
            sock->set_recv_timeout(std::chrono::milliseconds(ms));
    }

    /// TCP 模式下切换波特率无意义，仅串口模式生效
    bool setBaud(int baud) {
        if (type == SERIAL && sp && sp->is_open()) {
            serial_config cfg = sp->current_config();
            cfg.baud = static_cast<baud_rate>(baud);
            try { sp->configure(cfg); return true; } catch (...) {}
        }
        return false;
    }

    /// 获取当前波特率（仅 TCP 返回 0）
    int currentBaud() const {
        if (type == SERIAL && sp)
            return static_cast<int>(sp->current_config().baud);
        return 0;
    }
};

#endif // COMM_IO_H
