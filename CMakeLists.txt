#=============================================================================
# CMakeLists.txt — 四遥数据管理系统 跨平台构建配置
#=============================================================================
cmake_minimum_required(VERSION 3.14)
project(pmpc VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/utf-8 /W3 /wd4996)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -Wconversion)
endif()

set(PMPC_SOURCES
    src/main.cxx
    src/pmpc_config_reader.cxx
    src/pmpc_data_mgr.cxx
    src/pemp_server.cxx
    src/protocol.cxx
    src/soe_queue.cxx
    src/modbus_tcp_master.cxx
    src/modbus_tcp_slave.cxx
    src/modbus_rtu_master.cxx
    src/iec104_master.cxx
    src/iec104_slave.cxx
    src/dlt645_master.cxx
    src/modbus_rtu_slave.cxx
    src/iec103_master.cxx
    src/iec101_master.cxx
    src/iec101_slave.cxx
    src/cdt_master.cxx
    src/cdt_slave.cxx
    src/debug_console.cxx
    src/packet_logger.cxx
    src/data_recorder.cxx
    src/module_manager.cxx
    src/redundancy.cxx
    src/ini_reader.cxx
    src/socket.cpp
    src/serial_port.cpp
)

add_executable(pmpc ${PMPC_SOURCES})
target_include_directories(pmpc PRIVATE include)

# MySQL — 优先通过 find_package 查找，回退到默认路径
find_package(MySQL QUIET)
if(MySQL_FOUND)
    target_include_directories(pmpc PRIVATE ${MYSQL_INCLUDE_DIR})
    target_link_libraries(pmpc PRIVATE ${MYSQL_LIBRARIES})
    message(STATUS "MySQL found via find_package: ${MYSQL_INCLUDE_DIR}")
else()
    # 回退：尝试常见安装路径
    if(EXISTS "C:/Program Files/MySQL/MySQL Server 8.4/include/mysql.h")
        target_include_directories(pmpc PRIVATE "C:/Program Files/MySQL/MySQL Server 8.4/include")
        target_link_directories(pmpc PRIVATE "C:/Program Files/MySQL/MySQL Server 8.4/lib")
        target_link_libraries(pmpc PRIVATE libmysql)
        message(STATUS "MySQL found at default path: C:/Program Files/MySQL/MySQL Server 8.4")
    elseif(EXISTS "/usr/include/mysql/mysql.h")
        target_include_directories(pmpc PRIVATE /usr/include/mysql)
        target_link_libraries(pmpc PRIVATE mysqlclient)
        message(STATUS "MySQL found at /usr/include/mysql")
    else()
        message(WARNING "MySQL not found - data_recorder module will not compile")
    endif()
endif()

find_package(Threads REQUIRED)
target_link_libraries(pmpc PRIVATE Threads::Threads)

if(MSVC OR MINGW)
    target_link_libraries(pmpc PRIVATE ws2_32)
endif()

install(TARGETS pmpc RUNTIME DESTINATION bin)
install(FILES point_cfg.ini DESTINATION bin/config)
install(DIRECTORY include/ DESTINATION include)

option(PMPC_BUILD_TESTS "构建测试模块 (Google Test)" OFF)
if(PMPC_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
