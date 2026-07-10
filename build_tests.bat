@echo off
REM ===========================================================================
REM  build_tests.bat - Compile and run PMPC unit tests
REM  Uses tests/mini_gtest.h (header-only, no external deps)
REM  Uses tests/test_manifest.txt to define test list
REM  All .o files go to build/test_obj/ (separate from main build)
REM
REM  Usage:
REM    build_tests.bat                  Build and run all tests
REM    build_tests.bat list             List available tests
REM    build_tests.bat clean            Remove test build artifacts
REM    build_tests.bat <test_name>      Build and run a single test
REM ===========================================================================
setlocal enabledelayedexpansion

set OBJ_DIR=build\test_obj
set TEST_DIR=tests
set MANIFEST=%TEST_DIR%\test_manifest.txt

if "%1"=="clean" goto :clean
if "%1"=="list"  goto :list

REM Compiler flags
set CFLAGS=-std=c++17 -I include -I %TEST_DIR% -Wall -Wextra -Wpedantic -Wconversion -pthread -g -O0
set LDFLAGS=-pthread -lws2_32
if not exist %OBJ_DIR% mkdir %OBJ_DIR%

set PASS=0
set FAIL=0
set FAILED_NAMES=
set SINGLE_TEST=%1

REM Read manifest and build each test
if not exist %MANIFEST% (
    echo [ERROR] Manifest file %MANIFEST% not found!
    exit /b 1
)

if not "%SINGLE_TEST%"=="" (
    echo ===== Running single test: %SINGLE_TEST% =====
)

for /f "usebackq tokens=1,2,* delims=|" %%a in ("%MANIFEST%") do (
    REM Skip blank lines and comments
    set _line=%%a
    if not "!_line!"=="" if not "!_line:~0,1!"=="#" (
        set TNAME=%%a
        set TSRC=%%b
        set TDEPS=%%c
        REM Trim whitespace from each field
        call :trim TNAME
        call :trim TSRC
        call :trim TDEPS

        if not "!TNAME!"=="" if not "!TSRC!"=="" (
            if "%SINGLE_TEST%"=="" (
                call :build_and_run !TNAME! !TSRC! "!TDEPS!"
            ) else (
                if /i "!TNAME!"=="%SINGLE_TEST%" (
                    call :build_and_run !TNAME! !TSRC! "!TDEPS!"
                )
            )
        )
    )
)

if not "%SINGLE_TEST%"=="" if %PASS% equ 0 if %FAIL% equ 0 (
    echo [WARN] No test found matching "%SINGLE_TEST%"
    echo Use "build_tests.bat list" to see available tests.
)

goto :summary

REM ===== Trim helper =====
:trim
set _str=!%~1!
:trim_loop
if "!_str:~0,1!"==" " set _str=!_str:~1!& goto trim_loop
if "!_str:~-1!"==" " set _str=!_str:~0,-1!& goto trim_loop
set %~1=%_str%
goto :eof

REM ===== Build and run one test =====
:build_and_run
set TNAME=%1
set TSRC=%2
set TDEPS=%~3

echo ===== Building %TNAME% =====

REM Check if test source exists
if not exist "%TEST_DIR%\%TSRC%" (
    echo [FAIL] Source not found: %TEST_DIR%\%TSRC%
    set /a FAIL+=1
    set FAILED_NAMES=!FAILED_NAMES! %TNAME%
    goto :eof
)

REM Compile test source
set _need_link=0
if not exist %OBJ_DIR%\%TNAME%.o (
    set _need_link=1
) else (
    for %%f in ("%TEST_DIR%\%TSRC%") do set _TSRC_TIME=%%~tf
    for %%f in ("%OBJ_DIR%\%TNAME%.o") do set _TOBJ_TIME=%%~tf
    if "!_TSRC_TIME!" GTR "!_TOBJ_TIME!" set _need_link=1
)
if !_need_link! equ 1 (
    g++ -c %CFLAGS% %TEST_DIR%\%TSRC% -o %OBJ_DIR%\%TNAME%.o
    if errorlevel 1 (
        echo [FAIL] Compilation failed: %TNAME%
        set /a FAIL+=1
        set FAILED_NAMES=!FAILED_NAMES! %TNAME%
        goto :eof
    )
)

REM Compile dependencies (incremental, only if source newer than .o)
set DEP_OBJS=
for %%d in (%TDEPS%) do (
    set DNAME=%%~nd
    set _need_comp=0
    if not exist %OBJ_DIR%\!DNAME!.o (
        set _need_comp=1
    ) else (
        for %%f in ("%%d") do set _DSRC_TIME=%%~tf
        for %%f in ("%OBJ_DIR%\!DNAME!.o") do set _DOBJ_TIME=%%~tf
        if "!_DSRC_TIME!" GTR "!_DOBJ_TIME!" set _need_comp=1
    )
    if !_need_comp! equ 1 (
        g++ -c %CFLAGS% %%d -o %OBJ_DIR%\!DNAME!.o
        if errorlevel 1 (
            echo [FAIL] Compilation failed: %%d
            set /a FAIL+=1
            set FAILED_NAMES=!FAILED_NAMES! %TNAME%
            goto :eof
        )
    )
    set DEP_OBJS=!DEP_OBJS! %OBJ_DIR%\!DNAME!.o
)

REM Link (check if exe is older than any .o)
set _need_link_exe=0
if not exist "%TEST_DIR%\%TNAME%.exe" (
    set _need_link_exe=1
) else (
    for %%f in ("%OBJ_DIR%\%TNAME%.o") do set _TO_TIME=%%~tf
    for %%f in ("%TEST_DIR%\%TNAME%.exe") do set _TEXE_TIME=%%~tf
    if "!_TO_TIME!" GTR "!_TEXE_TIME!" set _need_link_exe=1
)
REM Check dependency .o times too
for %%d in (%TDEPS%) do (
    set DNAME=%%~nd
    if !_need_link_exe! equ 0 (
        for %%f in ("%OBJ_DIR%\!DNAME!.o") do set _DO_TIME=%%~tf
        for %%f in ("%TEST_DIR%\%TNAME%.exe") do set _TEXE_TIME=%%~tf
        if "!_DO_TIME!" GTR "!_TEXE_TIME!" set _need_link_exe=1
    )
)
if !_need_link_exe! equ 1 (
    g++ %OBJ_DIR%\%TNAME%.o !DEP_OBJS! -o %TEST_DIR%\%TNAME%.exe %LDFLAGS%
    if errorlevel 1 (
        echo [FAIL] Linking failed: %TNAME%
        set /a FAIL+=1
        set FAILED_NAMES=!FAILED_NAMES! %TNAME%
        goto :eof
    )
) else (
    echo [SKIP] %TNAME% is up to date
)

REM Run
echo.
echo ===== Running %TNAME% =====
%TEST_DIR%\%TNAME%.exe
if errorlevel 1 (
    echo [FAIL] Tests failed: %TNAME%
    set /a FAIL+=1
    set FAILED_NAMES=!FAILED_NAMES! %TNAME%
) else (
    set /a PASS+=1
)
echo.
goto :eof

REM ===== Summary =====
:summary
echo ========================================
echo  Results: %PASS% passed, %FAIL% failed
echo ========================================
if not "!FAILED_NAMES!"=="" echo  FAILED: !FAILED_NAMES!
if %FAIL% gtr 0 exit /b 1
exit /b 0

REM ===== Clean =====
:clean
echo [Tests] Cleaning...
if exist %OBJ_DIR% rmdir /s /q %OBJ_DIR%
for %%f in (%TEST_DIR%\test_*.exe) do del /q "%%f" 2>nul
echo Done
exit /b 0

:list
echo Available tests:
if not exist %MANIFEST% (
    echo [ERROR] Manifest file not found
    exit /b 1
)
for /f "usebackq tokens=1,2 delims=|" %%a in ("%MANIFEST%") do (
    set _ln=%%a
    if not "!_ln!"=="" if not "!_ln:~0,1!"=="#" (
        set _desc=%%b
        call :trim _ln
        call :trim _desc
        if not "!_ln!"=="" echo   !_ln!  - !_desc!
    )
)
exit /b 0
