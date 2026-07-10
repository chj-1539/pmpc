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
