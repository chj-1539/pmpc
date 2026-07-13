#=============================================================================
# tests/CMakeLists.txt — Google Test 单元测试框架
#
# 依赖:
#   - CMake 3.14+ (FetchContent)
#   - 网络连接 (首次构建时自动下载 Google Test)
#
# 构建:
#   cmake -S . -B build/debug -DPMPC_BUILD_TESTS=ON
#   cmake --build build/debug
#   ctest --test-dir build/debug
#=============================================================================
cmake_minimum_required(VERSION 3.14)

include(FetchContent)
FetchContent_Declare(
    googletest
    URL     https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
)
set(FETCHCONTENT_QUIET ON)
FetchContent_MakeAvailable(googletest)

include(GoogleTest)

# ─── 辅助函数 ─────────────────────────────────────────────────────────────────
# add_pmpc_test(name source.cxx [extra_src...])
#   - 编译 test 可执行文件，链接 gtest_main
#   - 自动注册到 ctest
function(add_pmpc_test TEST_NAME TEST_SRC)
    add_executable(${TEST_NAME} ${TEST_SRC} ${ARGN})
    target_include_directories(${TEST_NAME} PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_link_libraries(${TEST_NAME} PRIVATE GTest::gtest_main GTest::gmock)
    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
endfunction()

# ─── Phase 0: 已有测试 ──────────────────────────────────────────────────────

# Protocol 测试
add_pmpc_test(test_protocol test_protocol.cxx
    ${CMAKE_SOURCE_DIR}/src/protocol.cxx
)

# EventBus 测试 (header-only)
add_pmpc_test(test_event_bus test_event_bus.cxx)

# IniReader 测试
add_pmpc_test(test_ini_reader test_ini_reader.cxx
    ${CMAKE_SOURCE_DIR}/src/ini_reader.cxx
)

# SOEQueue 测试
add_pmpc_test(test_soe_queue test_soe_queue.cxx
    ${CMAKE_SOURCE_DIR}/src/soe_queue.cxx
    ${CMAKE_SOURCE_DIR}/src/protocol.cxx
)

# DataMgr 测试
add_pmpc_test(test_data_mgr test_data_mgr.cxx
    ${CMAKE_SOURCE_DIR}/src/pmpc_data_mgr.cxx
    ${CMAKE_SOURCE_DIR}/src/pmpc_config_reader.cxx
    ${CMAKE_SOURCE_DIR}/src/ini_reader.cxx
)

# ─── Phase 2: 基础层测试 ────────────────────────────────────────────────────

# str_util 测试 (header-only)
add_pmpc_test(test_str_util test_str_util.cxx)

# Logger 测试 (header-only)
add_pmpc_test(test_logger test_logger.cxx)

# Socket 测试
add_pmpc_test(test_socket test_socket.cxx
    ${CMAKE_SOURCE_DIR}/src/socket.cpp
)

# SerialPort 测试
add_pmpc_test(test_serial_port test_serial_port.cxx
    ${CMAKE_SOURCE_DIR}/src/serial_port.cpp
)

# CommIO 测试
add_pmpc_test(test_comm_io test_comm_io.cxx
    ${CMAKE_SOURCE_DIR}/src/socket.cpp
    ${CMAKE_SOURCE_DIR}/src/serial_port.cpp
)

# ─── Phase A: 代码审查回归测试 (bug 修复配套) ───────────────────────────────

# C6: event_bus.h 独立编译测试
add_pmpc_test(test_event_bus_isolated_include test_event_bus_isolated_include.cxx)

# M13: config 读取器 uint16 溢出回归
add_pmpc_test(test_config_reader_overflow test_config_reader_overflow.cxx
    ${CMAKE_SOURCE_DIR}/src/pmpc_data_mgr.cxx
    ${CMAKE_SOURCE_DIR}/src/pmpc_config_reader.cxx
    ${CMAKE_SOURCE_DIR}/src/ini_reader.cxx
    ${CMAKE_SOURCE_DIR}/src/soe_queue.cxx
    ${CMAKE_SOURCE_DIR}/src/protocol.cxx
)

# C2: cdt_master 同步头状态机回归（AdvanceSyncHeader 已 inline，无 .cxx 依赖）
add_pmpc_test(test_cdt_master_parser test_cdt_master_parser.cxx)

# M8: debug_console 自动任务生命周期（死锁修复回归）
# 只依赖 DebugConsole::DetachTaskLocked/DetachAllTasksLocked 两个 static 模板
# (在 debug_console.h 里内联)。不需要链接 debug_console.cxx 本身。
add_pmpc_test(test_debug_console_lifecycle test_debug_console_lifecycle.cxx
    ${CMAKE_SOURCE_DIR}/src/socket.cpp
)

# Bug #3: debug_console telnet IAC 过滤（filter_telnet_iac 提到 include/telnet_iac.h）
add_pmpc_test(test_debug_console_telnet test_debug_console_telnet.cxx)

# C4: Redundancy 心跳/同步帧编解码回归（peerPriority 字段修复）
add_pmpc_test(test_redundancy_frame_codec test_redundancy_frame_codec.cxx
    ${CMAKE_SOURCE_DIR}/src/redundancy.cxx
    ${CMAKE_SOURCE_DIR}/src/pmpc_data_mgr.cxx
    ${CMAKE_SOURCE_DIR}/src/pmpc_config_reader.cxx
    ${CMAKE_SOURCE_DIR}/src/ini_reader.cxx
    ${CMAKE_SOURCE_DIR}/src/soe_queue.cxx
    ${CMAKE_SOURCE_DIR}/src/protocol.cxx
    ${CMAKE_SOURCE_DIR}/src/socket.cpp
)

# C5: Redundancy DecideRole 表驱动回归
add_pmpc_test(test_redundancy_role_transitions test_redundancy_role_transitions.cxx
    ${CMAKE_SOURCE_DIR}/src/redundancy.cxx
    ${CMAKE_SOURCE_DIR}/src/pmpc_data_mgr.cxx
    ${CMAKE_SOURCE_DIR}/src/pmpc_config_reader.cxx
    ${CMAKE_SOURCE_DIR}/src/ini_reader.cxx
    ${CMAKE_SOURCE_DIR}/src/soe_queue.cxx
    ${CMAKE_SOURCE_DIR}/src/protocol.cxx
    ${CMAKE_SOURCE_DIR}/src/socket.cpp
)

# Bug #4: TCP socket 超时 vs 对端断连语义
add_pmpc_test(test_socket_timeout test_socket_timeout.cxx
    ${CMAKE_SOURCE_DIR}/src/socket.cpp
)

# Bug #5: Modbus TCP master doSent_/aoSent_ 竞态修复回归
add_pmpc_test(test_modbus_master_write_race test_modbus_master_write_race.cxx
    ${CMAKE_SOURCE_DIR}/src/modbus_tcp_master.cxx
    ${CMAKE_SOURCE_DIR}/src/pmpc_data_mgr.cxx
    ${CMAKE_SOURCE_DIR}/src/pmpc_config_reader.cxx
    ${CMAKE_SOURCE_DIR}/src/ini_reader.cxx
    ${CMAKE_SOURCE_DIR}/src/soe_queue.cxx
    ${CMAKE_SOURCE_DIR}/src/protocol.cxx
    ${CMAKE_SOURCE_DIR}/src/packet_logger.cxx
    ${CMAKE_SOURCE_DIR}/src/module_manager.cxx
    ${CMAKE_SOURCE_DIR}/src/pemp_server.cxx
    ${CMAKE_SOURCE_DIR}/src/redundancy.cxx
    ${CMAKE_SOURCE_DIR}/src/socket.cpp
    ${CMAKE_SOURCE_DIR}/src/serial_port.cpp
)

# Bug #1: ModuleManager 热重载 entries_/modules_ 索引错位修复回归
add_pmpc_test(test_module_manager_hotreload test_module_manager_hotreload.cxx
    ${CMAKE_SOURCE_DIR}/src/module_manager.cxx
    ${CMAKE_SOURCE_DIR}/src/modbus_tcp_master.cxx
    ${CMAKE_SOURCE_DIR}/src/pemp_server.cxx
    ${CMAKE_SOURCE_DIR}/src/redundancy.cxx
    ${CMAKE_SOURCE_DIR}/src/pmpc_data_mgr.cxx
    ${CMAKE_SOURCE_DIR}/src/pmpc_config_reader.cxx
    ${CMAKE_SOURCE_DIR}/src/ini_reader.cxx
    ${CMAKE_SOURCE_DIR}/src/soe_queue.cxx
    ${CMAKE_SOURCE_DIR}/src/protocol.cxx
    ${CMAKE_SOURCE_DIR}/src/packet_logger.cxx
    ${CMAKE_SOURCE_DIR}/src/socket.cpp
    ${CMAKE_SOURCE_DIR}/src/serial_port.cpp
)

# M3: PempServer 遥控密码校验修复回归
add_pmpc_test(test_pemp_server_auth test_pemp_server_auth.cxx
    ${CMAKE_SOURCE_DIR}/src/pemp_server.cxx
    ${CMAKE_SOURCE_DIR}/src/pmpc_data_mgr.cxx
    ${CMAKE_SOURCE_DIR}/src/pmpc_config_reader.cxx
    ${CMAKE_SOURCE_DIR}/src/ini_reader.cxx
    ${CMAKE_SOURCE_DIR}/src/soe_queue.cxx
    ${CMAKE_SOURCE_DIR}/src/protocol.cxx
    ${CMAKE_SOURCE_DIR}/src/socket.cpp
)

# H2: PempServer CH/DEV 8 位边界防护
add_pmpc_test(test_pemp_server_id_range test_pemp_server_id_range.cxx
    ${CMAKE_SOURCE_DIR}/src/pemp_server.cxx
    ${CMAKE_SOURCE_DIR}/src/pmpc_data_mgr.cxx
    ${CMAKE_SOURCE_DIR}/src/pmpc_config_reader.cxx
    ${CMAKE_SOURCE_DIR}/src/ini_reader.cxx
    ${CMAKE_SOURCE_DIR}/src/soe_queue.cxx
    ${CMAKE_SOURCE_DIR}/src/protocol.cxx
    ${CMAKE_SOURCE_DIR}/src/socket.cpp
)

# H1: PempServer SOE 上传失败回填队列
add_pmpc_test(test_pemp_server_soe_upload test_pemp_server_soe_upload.cxx
    ${CMAKE_SOURCE_DIR}/src/pemp_server.cxx
    ${CMAKE_SOURCE_DIR}/src/pmpc_data_mgr.cxx
    ${CMAKE_SOURCE_DIR}/src/pmpc_config_reader.cxx
    ${CMAKE_SOURCE_DIR}/src/ini_reader.cxx
    ${CMAKE_SOURCE_DIR}/src/soe_queue.cxx
    ${CMAKE_SOURCE_DIR}/src/protocol.cxx
    ${CMAKE_SOURCE_DIR}/src/socket.cpp
)

# H3: PempServer AI double→int32 精度保护
add_pmpc_test(test_pemp_server_ai_precision test_pemp_server_ai_precision.cxx
    ${CMAKE_SOURCE_DIR}/src/pemp_server.cxx
    ${CMAKE_SOURCE_DIR}/src/pmpc_data_mgr.cxx
    ${CMAKE_SOURCE_DIR}/src/pmpc_config_reader.cxx
    ${CMAKE_SOURCE_DIR}/src/ini_reader.cxx
    ${CMAKE_SOURCE_DIR}/src/soe_queue.cxx
    ${CMAKE_SOURCE_DIR}/src/protocol.cxx
    ${CMAKE_SOURCE_DIR}/src/socket.cpp
)

# Phase B 特征化：C3/H11/L4 锁定当前行为
add_pmpc_test(test_data_mgr_char test_data_mgr_char.cxx
    ${CMAKE_SOURCE_DIR}/src/pmpc_data_mgr.cxx
    ${CMAKE_SOURCE_DIR}/src/pmpc_config_reader.cxx
    ${CMAKE_SOURCE_DIR}/src/ini_reader.cxx
    ${CMAKE_SOURCE_DIR}/src/soe_queue.cxx
    ${CMAKE_SOURCE_DIR}/src/protocol.cxx
)

# M4: iec104_slave client 线程 cleanup 修复回归（helper 已 inline）
add_pmpc_test(test_iec104_slave_thread_cleanup test_iec104_slave_thread_cleanup.cxx)

# H9: Modbus AO epsilon 修复回归（AoAlmostEqual 已 inline）
add_pmpc_test(test_modbus_ao_epsilon test_modbus_ao_epsilon.cxx)

# H6: Modbus master 地址范围合并（ComputeAddrSpan 已 inline template）
add_pmpc_test(test_modbus_master_addr_span test_modbus_master_addr_span.cxx)

# L5: Redundancy 缺少配置文件时应 return false 而非默认双主
add_pmpc_test(test_redundancy_loadconfig test_redundancy_loadconfig.cxx
    ${CMAKE_SOURCE_DIR}/src/redundancy.cxx
    ${CMAKE_SOURCE_DIR}/src/pmpc_data_mgr.cxx
    ${CMAKE_SOURCE_DIR}/src/pmpc_config_reader.cxx
    ${CMAKE_SOURCE_DIR}/src/ini_reader.cxx
    ${CMAKE_SOURCE_DIR}/src/soe_queue.cxx
    ${CMAKE_SOURCE_DIR}/src/protocol.cxx
    ${CMAKE_SOURCE_DIR}/src/socket.cpp
)

# L6: PempServer stop 应 join 而非 detach（避免 UAF）
add_pmpc_test(test_pemp_server_stop_lifecycle test_pemp_server_stop_lifecycle.cxx
    ${CMAKE_SOURCE_DIR}/src/pemp_server.cxx
    ${CMAKE_SOURCE_DIR}/src/pmpc_data_mgr.cxx
    ${CMAKE_SOURCE_DIR}/src/pmpc_config_reader.cxx
    ${CMAKE_SOURCE_DIR}/src/ini_reader.cxx
    ${CMAKE_SOURCE_DIR}/src/soe_queue.cxx
    ${CMAKE_SOURCE_DIR}/src/protocol.cxx
    ${CMAKE_SOURCE_DIR}/src/socket.cpp
)

# L11: iec101_slave 帧完整性判定（IsCompleteFixedFrame/VariableFrame 已 inline）
add_pmpc_test(test_iec101_slave_framing test_iec101_slave_framing.cxx)

# M2: iec104_master I 帧序号编解码（EncodeIFrameCtrl 等已 inline）
add_pmpc_test(test_iec104_master_seqnum test_iec104_master_seqnum.cxx)

# C1: iec104_slave 客户端注册（ClientThread 应把自己 push 进 clients_）
add_pmpc_test(test_iec104_slave_client_registration test_iec104_slave_client_registration.cxx
    ${CMAKE_SOURCE_DIR}/src/iec104_slave.cxx
    ${CMAKE_SOURCE_DIR}/src/pmpc_data_mgr.cxx
    ${CMAKE_SOURCE_DIR}/src/pmpc_config_reader.cxx
    ${CMAKE_SOURCE_DIR}/src/ini_reader.cxx
    ${CMAKE_SOURCE_DIR}/src/soe_queue.cxx
    ${CMAKE_SOURCE_DIR}/src/protocol.cxx
    ${CMAKE_SOURCE_DIR}/src/packet_logger.cxx
    ${CMAKE_SOURCE_DIR}/src/module_manager.cxx
    ${CMAKE_SOURCE_DIR}/src/modbus_tcp_master.cxx
    ${CMAKE_SOURCE_DIR}/src/pemp_server.cxx
    ${CMAKE_SOURCE_DIR}/src/redundancy.cxx
    ${CMAKE_SOURCE_DIR}/src/iec104_master.cxx
    ${CMAKE_SOURCE_DIR}/src/socket.cpp
    ${CMAKE_SOURCE_DIR}/src/serial_port.cpp
)

# H5: iec104_slave 遥控 fallback 修复回归（DecideRemoteControlTargets 已 inline）
add_pmpc_test(test_iec104_slave_remote_ctrl test_iec104_slave_remote_ctrl.cxx)

# M11: iec101_master 硬编码 channel=1 修复回归
add_pmpc_test(test_iec101_master_multichannel test_iec101_master_multichannel.cxx
    ${CMAKE_SOURCE_DIR}/src/iec101_master.cxx
    ${CMAKE_SOURCE_DIR}/src/pmpc_data_mgr.cxx
    ${CMAKE_SOURCE_DIR}/src/pmpc_config_reader.cxx
    ${CMAKE_SOURCE_DIR}/src/ini_reader.cxx
    ${CMAKE_SOURCE_DIR}/src/soe_queue.cxx
    ${CMAKE_SOURCE_DIR}/src/protocol.cxx
    ${CMAKE_SOURCE_DIR}/src/packet_logger.cxx
    ${CMAKE_SOURCE_DIR}/src/module_manager.cxx
    ${CMAKE_SOURCE_DIR}/src/modbus_tcp_master.cxx
    ${CMAKE_SOURCE_DIR}/src/pemp_server.cxx
    ${CMAKE_SOURCE_DIR}/src/redundancy.cxx
    ${CMAKE_SOURCE_DIR}/src/socket.cpp
    ${CMAKE_SOURCE_DIR}/src/serial_port.cpp
)

# M12: iec104 RecvFrame 无效 apduLen 关闭 socket
add_pmpc_test(test_iec104_recvframe test_iec104_recvframe.cxx
    ${CMAKE_SOURCE_DIR}/src/iec104_master.cxx
    ${CMAKE_SOURCE_DIR}/src/pmpc_data_mgr.cxx
    ${CMAKE_SOURCE_DIR}/src/pmpc_config_reader.cxx
    ${CMAKE_SOURCE_DIR}/src/ini_reader.cxx
    ${CMAKE_SOURCE_DIR}/src/soe_queue.cxx
    ${CMAKE_SOURCE_DIR}/src/protocol.cxx
    ${CMAKE_SOURCE_DIR}/src/packet_logger.cxx
    ${CMAKE_SOURCE_DIR}/src/module_manager.cxx
    ${CMAKE_SOURCE_DIR}/src/modbus_tcp_master.cxx
    ${CMAKE_SOURCE_DIR}/src/pemp_server.cxx
    ${CMAKE_SOURCE_DIR}/src/redundancy.cxx
    ${CMAKE_SOURCE_DIR}/src/socket.cpp
    ${CMAKE_SOURCE_DIR}/src/serial_port.cpp
)
