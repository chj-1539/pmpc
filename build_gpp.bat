@echo off
REM ===========================================================================
REM  build_gpp.bat - MinGW-w64 g++ build script
REM  Compile each .cxx/.cpp to build/obj/*.o, then link final executable.
REM
REM  Usage:
REM    build_gpp.bat           Release build (pmpc.exe,  -O2)
REM    build_gpp.bat debug     Debug build   (pmpd.exe, -g -O0)
REM    build_gpp.bat clean     Remove build/obj/ and exe files
REM ===========================================================================
setlocal enabledelayedexpansion

set MODE=%1
if "%MODE%"=="" set MODE=release

REM ---- clean ----
if /I "%MODE%"=="clean" (
    echo [Build] Cleaning...
    if exist build\obj rmdir /s /q build\obj
    if exist pmpc.exe del /q pmpc.exe
    if exist pmpd.exe del /q pmpd.exe
    echo [Build] Clean done
    exit /b 0
)

REM ---- compiler flags ----
set CFLAGS=-std=c++17 -I include -Wall -Wextra -Wpedantic -Wconversion -pthread
set MYSQL_INC=-I"C:\Program Files\MySQL\MySQL Server 8.4\include"
set MYSQL_LIB=-L"C:\Program Files\MySQL\MySQL Server 8.4\lib"

if /I "%MODE%"=="debug" (
    set EXTRA=-g -O0 -DPMPC_DEBUG_DI_AI_PRINT
    set OUT=pmpd.exe
    echo [Build] Debug mode --^> %OUT%
) else if /I "%MODE%"=="release" (
    set EXTRA=-O2
    set OUT=pmpc.exe
    echo [Build] Release mode --^> %OUT%
) else (
    echo [Error] Unknown mode: %MODE%  (use: release^, debug^, clean^)
    exit /b 1
)

REM ---- create obj directory ----
if not exist build\obj mkdir build\obj

REM ---- compile each source to build/obj/*.o ----
set SOURCES=src/main.cxx src/pmpc_config_reader.cxx src/pmpc_data_mgr.cxx src/pemp_server.cxx src/protocol.cxx src/soe_queue.cxx src/modbus_tcp_master.cxx src/modbus_tcp_slave.cxx src/modbus_rtu_master.cxx src/modbus_rtu_slave.cxx src/iec104_master.cxx src/iec104_slave.cxx src/iec103_master.cxx src/iec101_master.cxx src/iec101_slave.cxx src/dlt645_master.cxx src/cdt_master.cxx src/cdt_slave.cxx src/debug_console.cxx src/data_recorder.cxx src/packet_logger.cxx src/module_manager.cxx src/redundancy.cxx src/ini_reader.cxx src/socket.cpp src/serial_port.cpp

echo [Build] Compiling ^(%MODE%^)...

for %%f in (%SOURCES%) do (
    echo   %%~nf
    g++ -c %CFLAGS% %EXTRA% %MYSQL_INC% "%%f" -o "build\obj\%%~nf.o"
    if errorlevel 1 (
        echo [Error] Compilation failed: %%f
        exit /b 1
    )
)

REM ---- link ----
echo [Build] Linking...
g++ build\obj\*.o %MYSQL_LIB% -o %OUT% -llibmysql -lws2_32

if errorlevel 1 (
    echo [Error] Linking failed
    exit /b 1
)

echo [Build] Done: %OUT%
