//=============================================================================
// test_serial_port.cxx — SerialPort 测试
// 策略: 测试构造/移动语义/错误码/配置管理/无效端口
// 注意: 实际串口收发需要 null-modem 硬件或 com0com 虚拟串口,
//       此处仅测试不依赖硬件的逻辑
//=============================================================================
#include "mini_gtest.h"
#include "serial_port.h"
#include <thread>

// ==================== serial_config 默认值 ====================

TEST(SerialConfigTest, DefaultBaud) {
    serial_config cfg;
    EXPECT_EQ(cfg.baud, baud_rate::br_115200);
}

TEST(SerialConfigTest, DefaultParity) {
    serial_config cfg;
    EXPECT_EQ(cfg.parity_check, parity::none);
}

TEST(SerialConfigTest, DefaultStopBits) {
    serial_config cfg;
    EXPECT_EQ(cfg.stop, stop_bits::one);
}

TEST(SerialConfigTest, DefaultDataBits) {
    serial_config cfg;
    EXPECT_EQ(cfg.data_bits, 8);
}

TEST(SerialConfigTest, DefaultFlowControl) {
    serial_config cfg;
    EXPECT_EQ(cfg.flow, flow_control::none);
}

TEST(SerialConfigTest, DefaultTimeoutsInfinite) {
    serial_config cfg;
    EXPECT_EQ(cfg.read_timeout.count(), -1);
    EXPECT_EQ(cfg.write_timeout.count(), -1);
}

TEST(SerialConfigTest, CustomConfig) {
    serial_config cfg;
    cfg.baud = baud_rate::br_9600;
    cfg.parity_check = parity::even;
    cfg.stop = stop_bits::two;
    cfg.data_bits = 7;
    cfg.flow = flow_control::hardware;
    cfg.read_timeout = std::chrono::milliseconds(500);
    EXPECT_EQ(cfg.baud, baud_rate::br_9600);
    EXPECT_EQ(cfg.parity_check, parity::even);
    EXPECT_EQ(cfg.stop, stop_bits::two);
    EXPECT_EQ(cfg.data_bits, 7);
    EXPECT_EQ(cfg.flow, flow_control::hardware);
    EXPECT_EQ(cfg.read_timeout.count(), 500);
}

// ==================== 错误码 ====================

TEST(SerialErrorTest, ErrorCategoryName) {
    auto& cat = serial_category();
    std::string name = cat.name();
    EXPECT_FALSE(name.empty());
}

TEST(SerialErrorTest, MakeErrorCode) {
    auto ec = make_error_code(serial_errc::open_failed);
    EXPECT_TRUE(ec);
    EXPECT_EQ(ec.value(), static_cast<int>(serial_errc::open_failed));
}

TEST(SerialErrorTest, AllErrorCodes) {
    auto ec1 = make_error_code(serial_errc::invalid_port);
    EXPECT_EQ(ec1.value(), 1);
    auto ec2 = make_error_code(serial_errc::open_failed);
    EXPECT_EQ(ec2.value(), 2);
    auto ec3 = make_error_code(serial_errc::configure_failed);
    EXPECT_EQ(ec3.value(), 3);
    auto ec4 = make_error_code(serial_errc::read_failed);
    EXPECT_EQ(ec4.value(), 4);
    auto ec5 = make_error_code(serial_errc::write_failed);
    EXPECT_EQ(ec5.value(), 5);
    auto ec6 = make_error_code(serial_errc::timeout);
    EXPECT_EQ(ec6.value(), 6);
    auto ec7 = make_error_code(serial_errc::not_open);
    EXPECT_EQ(ec7.value(), 7);
    auto ec8 = make_error_code(serial_errc::already_open);
    EXPECT_EQ(ec8.value(), 8);
}

TEST(SerialErrorTest, ErrorMessage) {
    serial_port_error err(serial_errc::open_failed, "port not found");
    std::string msg = err.what();
    EXPECT_NE(msg.find("port not found"), std::string::npos);
}

// ==================== 构造/析构/状态 ====================

TEST(SerialPortTest, DefaultConstructedIsNotOpen) {
    serial_port sp;
    EXPECT_FALSE(sp.is_open());
    EXPECT_EQ(sp.port_name(), "");
}

TEST(SerialPortTest, MoveConstruction) {
    serial_port sp1;
    serial_port sp2(std::move(sp1));
    EXPECT_FALSE(sp1.is_open()); // NOLINT: moved-from
    EXPECT_FALSE(sp2.is_open());
}

TEST(SerialPortTest, MoveAssignment) {
    serial_port sp1;
    serial_port sp2;
    sp2 = std::move(sp1);
    EXPECT_FALSE(sp1.is_open()); // NOLINT: moved-from
    EXPECT_FALSE(sp2.is_open());
}

TEST(SerialPortTest, DoubleCloseIsSafe) {
    serial_port sp;
    sp.close();
    EXPECT_NO_THROW(sp.close());
}

TEST(SerialPortTest, CloseAfterMove) {
    serial_port sp1;
    serial_port sp2(std::move(sp1));
    EXPECT_NO_THROW(sp2.close());
}

// ==================== 无效端口打开 ====================
// 尝试打开不存在的串口应抛出 serial_port_error

TEST(SerialPortTest, OpenInvalidPortThrows) {
    serial_port sp;
    serial_config cfg;
    cfg.read_timeout = std::chrono::milliseconds(100);
    // "COM_FAKE_999999" 应不存在于任何 Windows 系统
    EXPECT_THROW(sp.open("COM_FAKE_999999", cfg), serial_port_error);
}

// ==================== 配置管理 ====================

TEST(SerialPortTest, ConfigDefaults) {
    serial_port sp;
    serial_config dflt = sp.current_config();
    EXPECT_EQ(dflt.baud, baud_rate::br_115200);
}

TEST(SerialPortTest, CurrentConfigAfterConstruct) {
    serial_port sp;
    serial_config cfg = sp.current_config();
    EXPECT_EQ(cfg.data_bits, 8);
}
