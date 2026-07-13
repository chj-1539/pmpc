# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build

```powershell
# MinGW-w64 g++ — 使用构建脚本（推荐）
.\build_gpp.bat            # Release (pmpc.exe,  -O2)
.\build_gpp.bat debug      # Debug   (pmpd.exe, -g -O0)
.\build_gpp.bat clean      # 清理 build/obj/ 和 exe

# 或手动分步编译 + 链接（debug 示例）
$objdir = "build/obj"; if (!(Test-Path $objdir)) { New-Item -ItemType Directory $objdir }
g++ -c -std=c++17 -I include -IC:/Progra~1/MySQL/MYSQLS~1.4/include -Wall -Wextra -Wpedantic -Wconversion -pthread -g -O0 src/main.cxx -o $objdir/main.o
# ... 重复 -c 编译每个 .cxx/.cpp ...（源文件列表见 build_gpp.bat :48）
g++ $objdir/*.o -LC:/Progra~1/MySQL/MYSQLS~1.4/lib -o pmpd.exe -llibmysql -lws2_32

# CMake
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release

# CMake with tests
cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug -DPMPC_BUILD_TESTS=ON
cmake --build build/debug
ctest --test-dir build/debug

# 单元测试（使用 mini_gtest.h，无需下载 Google Test）
.\build_tests.bat                   # 编译并运行全部测试
.\build_tests.bat list              # 列出可用测试
.\build_tests.bat <test_name>       # 运行单个测试（如 test_protocol）
.\build_tests.bat clean             # 清理测试产物
```

**MySQL dependency:** `data_recorder` 模块需要 MySQL 8.4+ C client library。CMake 使用 `find_package(MySQL)` 并回退到默认安装路径。密码可通过环境变量 `MYSQL_PASSWORD` 覆盖。仅 `data_recorder` 模块依赖 MySQL，其他模块可在无 MySQL 环境下编译运行。

**Run:** `pmpc.exe [point_cfg.ini]` — 可选参数覆盖点表路径。

## Test framework

使用 `tests/mini_gtest.h`（自包含单头文件，零外部依赖），提供 `TEST/TEST_F/EXPECT_*/ASSERT_*` 宏和彩色输出。  
测试清单在 `tests/test_manifest.txt`，格式：`test_name | source.cxx | dep1.cxx dep2.cxx ...`  
每个测试编译为独立 exe，输出到 `tests/` 目录。增量编译：仅当源文件更新时重新编译。

**测试阶段：** Phase 0（`test_protocol`, `test_event_bus`, `test_ini_reader`, `test_soe_queue`, `test_data_mgr`）和 Phase 2（基础层：`test_str_util`, `test_logger`, `test_socket`, `test_serial_port`, `test_comm_io`）已实现。Phase 3-6（协议主站/从站/服务模块测试）待添加。

## Directory layout

```
├── include/           # 全部头文件 (.h)
├── src/               # 全部源文件 (.cxx / .cpp)
│   ├── main.cxx       # 入口：加载点表 → 启动模块 → CheckAllPointChange 循环
│   ├── module_manager.cxx  # ModuleManager + 内置模块 PIMPL 实现
│   │                       # (modbus_tcp_master, pemp_server, redundancy)
│   └── <protocol>_*.cxx    # 其他模块（各自 .cxx 末尾 REGISTER_MODULE）
├── tests/             # 单元测试（mini_gtest.h + test_manifest.txt）
├── build/obj/         # g++ 编译的 .o 文件
├── build/test_obj/    # 测试编译的 .o 文件
├── .vscode/           # tasks.json（6 个构建任务）, launch.json, c_cpp_properties.json
├── *.ini              # 全部配置文件在项目根目录
└── 说明书.md           # 50+ 页中文文档
```

## Architecture

### 四遥数据模型

| 类型 | 结构 | 含义 |
|------|------|------|
| DI (遥信) | `DiPoint` — `bool value, uint64 tsMs` | 数字输入，开关/状态信号 |
| AI (遥测) | `AiPoint` — `double value` | 模拟输入，电压/电流/温度/电度等 |
| DO (遥控) | `DoPoint` — `bool masterVal, slaveVal` | 数字输出，主站下发 + 从站反馈 |
| AO (遥调) | `AoPoint` — `double value` | 模拟输出，设定值 |

点表结构：`Channel[] → Device[] → 四遥向量`。每个 Device 有独立互斥锁 `devMtx`。

**DI 点号约定：** 点号 1 固定为通讯状态指示（ON=在线 OFF=离线），由 master 模块自动管理。业务遥信从点号 2 开始。点号 1 不触发 EventBus 事件。

**遥脉（电能脉冲）：** 统一按 AI 存储（double），通过 scale/offset 做工程转换。

### 核心数据流

```
采集层 (Master 模块)   →  RemoteDataMgr  ←  服务层 (Slave/Server 模块)
Modbus TCP/RTU        →     (全局单例)    ←  Modbus TCP/RTU Slave
IEC 104/103/101      →                  ←  PempServer (PEMP2.0推送)
CDT/DLT 645          →                  ←  IEC 104 Slave
                                           ↓ EventBus::Publish
                     DataRecorder → MySQL (pmpc_data 数据库)
                     PacketLogger → logs/traffic/ (报文文件)
```

`RemoteDataMgr::Set*` 接口会发布 `DIChange`/`AIChange`/`DOChange`/`AOChange` 事件到 `EventBus`。  
**注意：** `SetDi` 仅在值真正变化时（`oldVal != val`）才发布 DIChange。早期版本有 `(void)change` 的 bug，现已修复。

### SOE 事件流（PEMP2.0 52H）

```
采集模块 SetDi() → DIChange 事件 → EventBus
  → PempServer 订阅者 → SOEEvent → g_soeQueue.Push()
  → client_handler 循环 → do_upload_soe() → 52H 帧发送
```

`g_soeQueue` 是全局 `SOEQueue` 实例（线程安全），`do_upload_soe()` 通过 `PopAll()` 取走全部待发送事件并清空队列。`SOEEvent` 结构在 [protocol.h](include/protocol.h) 中定义。

### DO 脉冲复位（DO Pulse Reset）

`SetDoMaster(ch, dev, pt, true)` 写入 DO=1 时，自动将 `(ch, dev, pt, enqueueMs)` 推入 `pulseQueue_`。`CheckAllPointChange()` 每次主循环检查队列：若当前时间 >= `enqueueMs + doPulseMs_`（默认 300ms），自动复位 DO=0。此机制确保遥控合闸脉冲宽度可控，防止永久合闸。

### AO 变化队列

`SetAo` 将 AO 变化推入 `aoChangeQueue_`（deque + `aoQueueMtx_` 保护），由冗余模块的同步通道消费并发送到备机。

### EventBus ([event_bus.h](include/event_bus.h))

模板驱动的类型安全事件发布/订阅系统：
- `EventBus::Publish(event)` — 拷贝 handler 列表后逐个调用，避免重入死锁
- `EventBus::Subscribe<T>(handler)` — 返回 `size_t token` 用于取消订阅
- 预定义事件类型：`DIChange`, `AIChange`, `DOChange`, `AOChange`
- **严禁在持有任何锁时调用 `EventBus::Publish()`**（已在 `pmpc_data_mgr.cxx` 中修复为锁外发布）

### 主循环

```cpp
while (g_running) { mgr.CheckAllPointChange(); std::this_thread::sleep_for(200ms); }
```

仅做变化检测（对比 `pt.value != pt.lastVal`），不轮询存储。存储完全由 EventBus 事件驱动。

### 调试/测试辅助

- `src/pmpc_threads.cxx` — 模拟采集/遥控子线程，**默认不加入构建**，需手动添加源文件后启用
- Debug build 输出 `pmpd.exe`，Release 输出 `pmpc.exe`
- `logs/config_error_*.log` — 点表配置解析错误日志
- DebugConsole (`telnet 127.0.0.1 9090`) — 支持 `get/set/auto/role/status/log` 等命令
- **DebugConsole 的 telnet IAC 协商**：Windows telnet 会发送 IAC 字节（`\xFF\xFD...`），`debug_console.cxx` 中的 `filter_telnet_iac()` 会自动过滤掉协商字节

### 模块系统

所有模块继承 `AppModule`（6 个纯虚方法），通过 `REGISTER_MODULE` 宏自动注册到 `ModuleFactory`。`ModuleManager` 从 `app_modules.ini` 加载模块，支持文件修改自动热重载。

**PIMPL 模式：** 大多数模块使用 `struct Impl; std::unique_ptr<Impl> impl_;` 隐藏实现细节。  
**内置模块**（ModbusTcpMasterModule / PempServerModule / RedundancyModule）的 PIMPL 实现在 `module_manager.cxx` 中。  
**其他模块**（IEC/Modbus Slave/CDT/DLT645 等）各自在独立 `.cxx` 文件中，末尾调用 `REGISTER_MODULE`。

| 模块名 | 传输层 | 功能 |
|--------|--------|------|
| `modbus_tcp_master` | TCP | Modbus TCP 主站 FC01~16，模板继承 + 地址合并 |
| `modbus_tcp_slave` | TCP | Modbus TCP 从站 FC01~05，点对点映射 |
| `modbus_rtu_master` | 串口/TCP | Modbus RTU 主站，模板继承 + 地址合并 |
| `modbus_rtu_slave` | 串口/TCP | Modbus RTU 从站 FC01~05 |
| `iec104_master` | TCP | IEC 104 主站，长连接+总召唤+主动上报 |
| `iec104_slave` | TCP | IEC 104 从站，GI+遥控+DI/AI上送 |
| `iec103_master` | 串口/TCP | IEC 103 主站，FUN+INF 映射 |
| `iec101_master` | 串口/TCP | IEC 101 主站，ASDU同104 |
| `iec101_slave` | 串口/TCP | IEC 101 从站，GI+遥控 |
| `dlt645_master` | 串口/TCP | DLT 645 电表采集，1997/2007双版本 |
| `cdt_master` | 串口/TCP | CDT 主站，被动接收 YC/YX/YM |
| `cdt_slave` | 串口/TCP | CDT 从站，主动循环上送 |
| `pemp_server` | TCP | PEMP2.0 协议服务端（DI/AI/SOE 主动推送） |
| `data_recorder` | — | MySQL 数据存储，按月分表 |
| `packet_logger` | — | 报文记录，协议自动解析 |
| `debug_console` | TCP | TCP 调试控制台，密码认证 |
| `redundancy` | TCP | 双机冗余：心跳+角色切换 |

### PEMP2.0 通讯协议

帧格式：`7BH | FUN(1B) | LEN(2B小端) | DATA | 7DH`

| 功能码 | 方向 | 说明 |
|--------|------|------|
| 01H | 主站→从站 | 查询状态 |
| 02H | 主站→从站 | 切换工作状态（含启停主动上传） |
| 03H | 主站→从站 | 召唤遥测 |
| 04H | 主站→从站 | 召唤历史 SOE |
| 06H | 主站→从站 | 执行遥控（含密码验证） |
| 08H | 主站→从站 | 同步时钟 |
| 09H | 主站→从站 | 召唤遥信 |
| 51H | 从站→主站 | 主动上传遥信（按包按位打包） |
| 52H | 从站→主站 | 主动上传 SOE（每条: CH+DEV+SOEID+CP56time2a+STATUS） |
| 53H | 从站→主站 | 主动上传遥测（定时驱动） |

`FrameBuilder` / `FrameParser` 在 [protocol.h](include/protocol.h) + [protocol.cxx](src/protocol.cxx) 中实现。  
SOE 帧单条记录格式（12 字节）：`CH(1) | DEV(1) | SOE_ID(2小端) | ms(2) | min(1) | hour(1) | day(1) | month(1) | year(1) | STATUS(1)`，status 取值 `01`=开(OFF)、`02`=合(ON)。

### PacketLogger 协议解析器插件框架

继承 `ProtocolParser`，实现 `CanParse()` 和 `Parse()` 方法，调用 `PacketLogger::RegisterParser()` 注册。内置 `ModbusTcpParser` 作为默认解析器。新增协议只需添加一个解析器类，无需修改记录器核心逻辑。

### 关键基础设施

- **`CommIO`** ([comm_io.h](include/comm_io.h)) — 统一 IO 封装：`port_name` 含 `:` → TCP socket，否则 → 物理串口。所有串口模块（Modbus RTU、IEC 101/103、DLT 645、CDT）均通过它通讯
- **`socket`** ([socket.h](include/socket.h)) — RAII 跨平台 TCP/UDP 封装，`wsa_guard` 自动初始化 Winsock。`std::error_code` 错误码体系 + `socket_error` 异常
- **`serial_port`** ([serial_port.h](include/serial_port.h)) — RAII 跨平台串口封装，110~921600 波特率，奇/偶/无/标记/空格校验
- **`IniReader`** ([ini_reader.h](include/ini_reader.h)) — INI 解析器，支持 `[Section]`/`key=value`/`;`/`#` 注释
- **`str_util.h`** — 内联字符串工具：`Trim`/`ToLower`/`Split`/`ParseKeyValues`/`StartsWith`
- **`SOEQueue`** ([soe_queue.h](include/soe_queue.h)) — 线程安全 SOE 事件队列（deque + mutex），全局实例 `g_soeQueue`
- **`Logger`** ([logger.h](include/logger.h)) — 轻量级头文件日志工具，线程安全，全局/模块级别控制，自动时间戳前缀。使用 `Logger::Info("ModuleName") << "msg" << std::endl;`
- **`ModuleFactory`** ([module_factory.h](include/module_factory.h)) — `REGISTER_MODULE` 宏自动注册模块，无需修改工厂代码
- **`g_running`** — 全局 `std::atomic<bool>` 退出标志

### 🔒 锁顺序约束（死锁预防）

**全局锁层级（必须遵守，违反导致 ABBA 死锁）：**
1. `mysqlMtx_` (DataRecorder)
2. `structMtx_` (RemoteDataMgr 通道结构)
3. `pulseMtx_` / `aoQueueMtx_` (DO脉冲/AO变化队列)
4. `Device::devMtx` (每设备独立锁)
5. `EventBus::Publish` / handler（**严禁在持任何锁时调用**）

**重要规则：**
- `EventBus::Publish()` 必须在所有锁之外调用。`SetDi/SetAi/SetDo/SetAo` 已在 `pmpc_data_mgr.cxx` 中修复为先更新数据→释放锁→再发布
- **`WithDeviceLocked`** 是访问 Device 数据的唯一安全路径：内部同时持有 `structMtx_` + `devMtx`，传入 lambda 在锁保护下执行操作。禁止直接解引用 `FindDev` 返回的裸指针
- `CheckAllPointChange()` 使用 `(chId, devNo)` 快照而非 `Device*` 裸指针，通过 `WithDeviceLocked` 安全遍历
- EventBus handler 内不可反向持其他模块的锁
- 冗余模块的 `hbSendSock_` 必须通过 `hbMtx_` 保护并发访问

### ConfigErrorReporter（点表配置错误收集）

`ConfigErrorReporter` 在点表加载时收集错误/警告，支持带行号报告和全局（无行号）报告。错误保存到 `logs/config_error_*.log`（自动创建目录），同时打印摘要到 stderr。`HasErrors()` 不影响主流程——配置警告不阻止启动。

### 线程模型

- **无线程池** — 每个模块自行管理其线程（accept 线程、handler 线程、轮询线程）
- **主线程** — `CheckAllPointChange` 循环（200ms 周期）
- **模块管理器** — 独立 watch 线程检测 .ini 文件变更（2s 间隔）
- **冗余模块** — 独立心跳发送/监听线程、同步通道线程、DO 脉冲复位线程
- **冗余联动** — `ModuleManager::StartModule/StopModule` 通过回调注册到冗余模块，角色切换时（Master→Standby 或反向）自动启停采集模块（`modbus_tcp_master` 等）。采集模块的 `IsRunning()` 被冗余模块轮询以判断对方角色
- **退出** — `SIGINT`/`SIGTERM` 设置 `g_running=false`，各模块轮询该标志退出

### 🔧 已知陷阱 / 修复历史（修改时注意）

- **热重载索引错位**（`module_manager.cxx`）：`entries_` 含禁用模块（17个），`modules_` 仅含已启用（5个），两者索引不对应。`ReloadModule` 必须按名称查找 entry，不可共用 `FindModule` 返回的索引。已修复。
- **`SetDi` 的 `change` 参数**（`pmpc_data_mgr.cxx`）：早期版本 `(void)change` 忽略了这个参数，每次 SetDi 都无条件发 DIChange。现已改为仅 `oldVal != val` 时发布。调用方如需要强制发布，应在调用前主动判断。
- **DebugConsole telnet 连接**：Windows telnet 发送 IAC 协商字节（以 `\xFF` 开头），被 `filter_telnet_iac()` 过滤。recv 超时 500ms，超时不等于断开——已在 `ClientThread` 中处理。
- **TCP socket 超时 vs 断开**：`set_recv_timeout` 设超时后，`::recv` 在 Windows 上返回 `SOCKET_ERROR`（WSAETIMEDOUT → 抛异常），在 POSIX 上返回 -1（EAGAIN）。recv 返回 0 才是对端正常关闭。代码中异常和返回值需分别处理，不可混为一谈。
- **WriteDOChanges/WriteAOChanges `lastMaster`/`lastVal` 冲突**（`modbus_tcp_master.cxx`，`modbus_rtu_master.cxx`）：原代码通过对比 `DoPoint::lastMaster` / `AoPoint::lastVal` 判断值是否变化。但 `CheckAllPointChange()`（200ms 周期）会将这两个字段同步为当前值，抢在 master 模块的采集循环（500ms 以上周期）之前，导致变化检测永远认为"无变化" → 不写回。修复为模块级 `doSent_`/`aoSent_` 独立追踪上次已发值。已修复。

### 🩹 本轮代码审查修复（回归测试见 tests/）

- **`iec104_slave` clients_ 从未注册**（C1）：SendDIActiveUpload / SendAIActiveUpload / TimerThread 都遍历 `clients_`，但 ClientThread 从未 push 任何 ClientInfo → 主动上送全部空转。修复：`clients_` 改为 `vector<shared_ptr<ClientInfo>>`；ClientThread 构造自己的 ClientInfo、进入时 push、退出时 erase；RecvFrame 增加 `peerClosedOut` 参数供 ClientThread 区分超时与断连。加 public `ClientCount()` 供测试观察。
- **`RedundancyManager::SetRole` 无锁**（C5）：CheckFailover（心跳线程）与 RequestRoleChange（debug_console 线程）都能进入，两条路径可能并发启动 syncChannel_。修复：新增 `roleMtx_` 保护整段（状态检查 + syncChannel_.Stop → Start），保证并发调用互相 serialize；同时新语义"每次切换前 Stop 现有 syncChannel_"消除了 Standby→Master 时旧 recv 线程未终止的隐患。

- **`cdt_master.cxx` 同步头判断被注释吞掉**（C2）：`if (pos==0 && byte != SYNC_BYTE)` 写进了 `//` 单行注释里，pos=0 时任意字节都会写入 buf[0]。已抽出 `CdtMaster::AdvanceSyncHeader` 纯函数并让 `PortThread` 复用。
- **`event_bus.h` 缺 `<algorithm>`**（C6）：`Unsubscribe` 用 `std::remove_if` 却未包含该头文件，只通过传递包含才能编译。已直接 include。
- **Set{Ai,DoMaster,DoSlave,Ao} 无差别发布 EventBus**（C3）：与 SetDi 不一致，每次调用都发事件，会灌爆 data_recorder / iec104_slave 等订阅者。修复为 `oldVal != val` 门控。
- **Redundancy 心跳缺 peerPriority**（C4）：帧只 role+ts，`peerPriority_` 永远为 0，双主优先级决策失效。已扩帧格式 LEN=8→11，向前兼容老 14 字节格式。同时把帧编解码 + `DecideRole` 抽为 `pmpc::redundancy` 命名空间的自由函数。
- **PempServer 忽略遥控密码**（M3）：`(void)pwdLen` 丢弃密码字段。新增 `rcPassword_` 成员 + `[global] rc_password` ini 配置；空密码保留老行为兼容。
- **PempServer CH/DEV 8 位截断**（H2）：规约限制 1..255；`chId & 0xFF` 让 chId=256 与 chId=0 撞车。改为 log 警告 + 跳过。
- **PempServer AI double→int32 未 clamp**（H3）：`static_cast<int32_t>(v)` 对 |v|>2^31−1 是 UB。新增 `QuantizeAiToInt32Warn` 做 clamp 到 INT32_MIN/MAX，首次警告去重。
- **PempServer SOE 发送失败即丢**（H1）：`PopAll()` 后 send 失败 events 就永久消失。新增 `SOEQueue::PushFrontBatch` 保序回填，失败时 events 回队。
- **DebugConsole `StopAutoTask` 死锁**（M8）：持 `autoMtx_` 后 `join()` 工作线程；后者每轮循环也抢 `autoMtx_`。抽出 `DetachTaskLocked` / `DetachAllTasksLocked` 静态模板：锁内标记 + 移出，锁外 join。
- **`pmpc_config_reader.cxx` uint16 溢出**（M13）：`for (uint16_t i=1; i<=diCnt+1; i++)` 在 `diCnt=65535` 时 `diCnt+1` 回绕到 0，循环不执行。修为 uint32 循环变量 + 上限 `diCnt≤65534`（pt=1 保留给通讯状态位）。
- **`iec104_slave` 遥控 fallback 到 doMap[0]**（H5）：cmdVal 不匹配任何 mapping 时错误地写第一条 entry。抽 `DecideRemoteControlTargets` 纯函数，只返回严格匹配；不匹配走 COT.P/N=1 negative ack。
- **`iec101_master` 硬编码 SetDi(chId=1, ...)**（M11）：`HandleGIResponse` 遍历所有 channels 用 coa 匹配，且硬编码 chId=1，多通道下相互覆盖。加 chIdx 参数、按通道路由 chId=(chIdx+1)。
- **`iec104_master`/`slave` RecvFrame apduLen 无效未排空 socket**（M12）：非法 apduLen 后返回 false 但残余字节留在流里 → 下次误当帧头。改为 shutdown+close，让上层重连。
- **`iec104_slave` client 线程 `joinable()` 判定错**（M4）：joinable() 对 return 后未 join 的线程仍为 true → cleanup 分支永远不走，clientThreads_ 只增不减。加显式 done 标志 + `CleanupFinishedClientThreads` helper。
- **`filter_telnet_iac` 无状态跨包丢字节**（H7）：一个 IAC 序列跨两次 recv() 时末尾裸 0xFF 被 break 丢，选项字节泄入命令流。重写为 `pmpc::TelnetIacFilter` 状态机，`DebugConsole::ClientThread` 每连接持一份实例。
- **Modbus AO 固定 epsilon 0.001**（H9）：对量程 1e9 附近浮点噪声 >0.001，重复发送；对 1e-6 附近漏发。改为混合容差 `AoAlmostEqual(a, b, absTol=1e-6, relTol=1e-4)`。
- **`pmpc_config_reader.cxx` 重复 [Channel_N] 段静默合并**（L4）：产生两个同 chId 的 Channel，`FindCh` 只返回首个 → 后续段成"死"数据。改为拒绝第二个及以后同名段，报 error。
- **`RedundancyManager::LoadConfig` 文件缺失时 return true**（L5）：默认 `peerIp_=127.0.0.1` → 两台机器裸上生产互相 localhost 撞车。改为 return false，让 ModuleManager 跳过 redundancy 模块。
- **`PempServer::stop` 用 detach 而非 join**（L6）：detach 后 `delete server` 触发被 detach 线程的 UAF。client_handler 已有 500ms recv 超时，join 是安全的。
- **`iec101_slave` PortThread 接受 LEN=0 的可变帧**（L11）：`[0x68, 0x00, 0x16]` 会通过 `pos>=buf[1]+2 && buf[pos-1]==END` 判定进入 HandleFrame。抽 `IsCompleteFixedFrame` / `IsCompleteVariableFrame` inline helper，要求 LEN≥4。
- **`Iec104Master` I 帧 ctrl 恒为 0**（M2）：严格从站在 k=12 未确认后拒绝后续 I 帧。新增 `EncodeIFrameCtrl(sNr, rNr)` 纯函数，`SendIFrame` 接收 sendSeq/recvSeq 引用；ChannelThread 每连接维护、每收 I 帧 recvSeq++。
- **Modbus master `if (qty<maxQty) qty=maxQty` 死代码**（H6）：`qty = maxAddr-minAddr+1` 数学上已 ≥ 任何单条 qty；fallback 永远不生效。删除死代码并抽 `ComputeAddrSpan` 模板做不变式测试。
- **`main.cxx` CheckAllPointChange 无异常保护**（L17）：一次 lambda 抛异常就杀进程。加 try/catch 记 stderr 继续。


### 新增模块 checklist

1. 创建 `class XxxModule : public AppModule`，实现 6 个纯虚方法
2. 如果使用串口/TCP，用 `CommIO` 替代 `serial_port` 直接调用
3. `.cxx` 末尾加 `REGISTER_MODULE("name", XxxModule)` — 自动注册到工厂
4. `app_modules.ini` 加配置节（enable/cfg_file/auto_reload）
5. `CMakeLists.txt` 加源文件
6. `build_gpp.bat` 的 `SOURCES=` 列表加源文件
7. `.vscode/tasks.json` 加源文件
8. 修改 `CLAUDE.md` 和 `说明书.md`

### 端口分配

| 用途 | 端口 |
|------|------|
| Modbus Master（客户端） | 502 |
| Modbus Slave（服务端） | 503 |
| PempServer（PEMP2.0） | 4096 |
| IEC 104 | 2404 |
| DebugConsole | 9090 |
| 冗余模块 - 心跳 | 7503 |
| 冗余模块 - 同步 | 7502 |
| MySQL | 3306（外部） |

### 配置文件

| 文件 | 用途 |
|------|------|
| `app_modules.ini` | 模块加载清单（enable/cfg_file/auto_reload） |
| `point_cfg.ini` | 四遥点表（通道/设备/模板/映射） |
| `modbus_tcp_master.ini` 等 | 各模块独立配置 |

### MySQL 数据库表

| 表名 | 用途 |
|------|------|
| `rt_status` | 实时值 `(source,tag,ch,dev,pt,type,value,ts_ms)` |
| `di_log_YYYYMM` | DI 变化记录，按月分表 |
| `ai_log_YYYYMM` | AI 定时存盘，时间对齐，按月分表 |
| `pmpc_instance` | 实例在线 |
| `schema_version` | 版本管理 |

多实例通过 `source` 字段区分数据来源。`retention_months` 配置自动删除过期表。

### DebugConsole 命令

连接 `telnet 127.0.0.1 9090`：
- `get/set di/ai/do/ao` — 读写四遥值
- `auto di/ai` — 自动变位模拟（DI 翻转 / AI 正弦波）
- `role` — 查看/切换冗余角色
- `status / channels / devices` — 系统查询
- `start/stop <module>` — 启停模块
- `reload [point\|module]` — 热重载
- `log start/stop/status/parse` — 报文记录控制

### 平台注意

- Windows：`main.cxx` 中使用 `SetConsoleOutputCP(CP_UTF8)` + `ENABLE_VIRTUAL_TERMINAL_PROCESSING` 启用 UTF-8 + ANSI 彩色输出
- 引用 `windows.h` 时必须先定义 `WIN32_LEAN_AND_MEAN` + `NOMINMAX`，避免 Winsock 1.1 冲突
- `wsa_guard` 在 `main()` 中构造，RAII 管理 Winsock 生命周期
- `serial_port` 在 Windows 上使用 `OVERLAPPED` 异步 I/O，POSIX 上使用阻塞模式
