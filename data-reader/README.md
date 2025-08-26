---

# Data Reader - 通用数据采集系统（Protocol V6）

> **更新要点（v1.4）**
>
> * IPC 改为**后台线程模式**：`startIPCThread()` / `stopIPCThread()`，不再在主循环内轮询 `processIPCMessages()`，避免阻塞，键盘响应更流畅。
> * 保持与 data-processor 的 JSON 消息格式不变。
> * 设备数据包继续走**共享内存**通道；控制/状态走**命名管道 IPC**。

---

## 目录

* [概述](#概述)
* [核心特性](#核心特性)
* [系统架构](#系统架构)
* [文件结构](#文件结构)
* [编译 & 运行](#编译--运行)
* [命令行参数](#命令行参数)
* [运行期键盘命令](#运行期键盘命令)
* [IPC 通信接口](#ipc-通信接口)
* [共享内存数据](#共享内存数据)
* [原始帧落盘](#原始帧落盘)
* [性能与资源占用](#性能与资源占用)
* [调试与故障排除](#调试与故障排除)
* [开发与测试](#开发与测试)
* [版本历史](#版本历史)

---

## 概述

**Data Reader** 是通用数据采集系统的核心组件，负责与下位机设备通信，完整实现 **Protocol V6**：命令下发、数据/事件接收、设备日志、状态查询等。
它作为中间件，连接**硬件设备**与**上层 data-processor**（数据处理模块）。

---

## 核心特性

### 双模式通信

* **串口模式**：与真实设备通过 COM 口通信
* **Socket 模式**：与测试模拟器通过 TCP 连接

### 协议 V6 支持

* 系统控制：PING/PONG、设备信息、状态查询
* 采集控制：模式切换（连续/触发）、启动/停止、参数配置
* 数据与事件：数据包、触发事件、缓冲区传输完成、设备日志

### IPC（命名管道，后台线程）

* Windows 命名管道（双向）
* **后台线程**异步接收/回调，不阻塞主循环
* JSON Lines（每行一个 JSON 对象）

### 数据管理

* **共享内存**写入高吞吐数据包（ADC等）
* 原始帧按批落盘（减少磁盘 IO 峰值）
* 统计信息与状态可随时查询

---

## 系统架构

```
┌─────────────┐   Protocol V6   ┌──────────────┐    Shared Memory     ┌───────────────┐
│   Device    │ ◄─────────────► │  data-reader │ ───────────────────► │ data-processor│
│ / Simulator │   (Binary)      │              │     (ADC packets)    │               │
└─────────────┘                 └──────────────┘                      └───────────────┘
                                         │                                       ▲
                                         │ IPC (Named Pipe, JSON Lines)          │
                                         └─────────────── Control/Status ────────┘
                                     (Handled by background IPC thread)
```

---

## 文件结构

```
data-reader/
├─ serialread.c             # 主程序（通信主循环、键盘交互、文件写入、共享内存写入）
├─ ipc_communication.h      # IPC（命名管道）头文件：线程版 API
├─ ipc_communication.c      # IPC 实现：后台线程阻塞 ReadFile + 回调
├─ shared_memory.h          # 共享内存接口
├─ shared_memory.c
├─ protocol.h               # 协议 V6 封装
├─ protocol.c
├─ io_buffer.h              # 流式帧解析缓冲
├─ io_buffer.c
├─ Makefile
└─ README.md
```

---

## 编译 & 运行

### 编译

```bash
# 调试版
make

# 发布版
make BUILD=release

# 清理重建
make rebuild

# 查看帮助
make help
```

### 运行

**串口模式（默认 COM7）**

```bash
./serialread.exe
# 或指定 COM 口
./serialread.exe 3         # COM3
./serialread.exe 7         # COM7
```

**Socket 模式（默认 127.0.0.1:9001）**

```bash
./serialread.exe -s
./serialread.exe -s 192.168.1.100
./serialread.exe -s 192.168.1.100 8080
```

---

## 命令行参数

* 无参数：默认使用 `COM7`
* `N`：数字形式指定 `COMN`（例如 `3` → `COM3`）
* `-s [HOST] [PORT]`：启用 TCP 客户端模式（默认 `127.0.0.1 9001`）

---

## 运行期键盘命令

| 键         | 说明                     | 协议命令                      |
| --------- | ---------------------- | ------------------------- |
| `h`       | 显示帮助                   | -                         |
| `s`       | 显示当前状态（连接、IPC、设备信息、统计） | -                         |
| `p`       | 发送 PING                | `CMD_PING`                |
| `i`       | 获取设备信息                 | `CMD_GET_DEVICE_INFO`     |
| `1`       | 设置**连续**模式             | `CMD_SET_MODE_CONTINUOUS` |
| `2`       | 设置**触发**模式             | `CMD_SET_MODE_TRIGGER`    |
| `3`       | **开始**数据流              | `CMD_START_STREAM`        |
| `4`       | **停止**数据流              | `CMD_STOP_STREAM`         |
| `c`       | 发送演示用“流配置”示例           | `CMD_CONFIGURE_STREAM`    |
| `ESC / q` | 退出程序                   | -                         |

> 由于 IPC 改为后台线程，主循环不会被管道读阻塞；以上按键应**即时响应**。

---

## IPC 通信接口

### 基本信息

* **命名管道**：`\\.\pipe\data_reader_ipc`
* **格式**：JSON Lines（每行一个 JSON 对象，末尾 `\n`）
* **线程模型**：

  * Data Reader 作为 **Named Pipe 服务器**。
  * **后台线程**执行阻塞 `ReadFile`；收到完整一行后解析并触发**回调**。
  * 回调函数在**IPC线程上下文**中执行，**如修改共享状态请自行加锁**。

### 接收的消息（来自 data-processor）

**FORWARD\_TO\_DEVICE**（转发控制到设备）

```json
{"id":"...","timestamp":"...","type":"FORWARD_TO_DEVICE","payload":{"command_id":"0x03","data":"Base64EncodedData"}}
```

**SET\_READER\_MODE**（切换 reader 模式/目标）

```json
{"id":"...","timestamp":"...","type":"SET_READER_MODE","payload":{"mode":"socket","target":"127.0.0.1:9001"}}
```

**REQUEST\_READER\_STATUS**（请求当前状态）

```json
{"id":"...","timestamp":"...","type":"REQUEST_READER_STATUS","payload":{}}
```

### 发送的消息（发往 data-processor）

**READER\_STATUS\_UPDATE**

```json
{"id":"...","timestamp":"...","type":"READER_STATUS_UPDATE","payload":{
  "mode":"socket","target":"127.0.0.1:9001",
  "device_connected":true,
  "device_id":"11223344AABBCCDD",
  "data_transmission":true
}}
```

**DEVICE\_FRAME\_RECEIVED**（转发非数据帧，如 PONG、INFO、ACK/NACK、事件等）

```json
{"id":"...","timestamp":"...","type":"DEVICE_FRAME_RECEIVED","payload":{
  "command_id":"0x41","seq":5,"payload_len":32,"data":"Base64EncodedFrameData"
}}
```

**DEVICE\_LOG\_RECEIVED**（设备侧日志）

```json
{"id":"...","timestamp":"...","type":"DEVICE_LOG_RECEIVED","payload":{
  "level":"INFO","message":"Stream started"
}}
```

> 说明：**数据包（大量/高频）不经 IPC**，直接写入共享内存；IPC 更适合**低频控制/状态/日志**。

---

## 共享内存数据

### 典型数据结构（示例）

```c
typedef struct {
    uint32_t timestamp_ms;   // 时间戳
    uint16_t sequence;       // 序列号
    uint16_t payload_len;    // 数据长度
    uint8_t  payload[4096];  // ADC数据内容
} ADCDataPacket;
```

### 使用方式

* 名称：`ADC_DATA_SHARED_MEM`（示例）
* 大小：约 4MB（例如 1024 个数据包的环形缓冲）
* 访问：跨进程共享；请按既定写入/读取协议处理并发

---

## 原始帧落盘

* **文件名**：`raw_frames_000.txt`, `raw_frames_001.txt`, …
* **格式**：`LEN:18 HEX: AA 55 0C 00 ...`
* **策略**：每 **500** 帧批量写入；单文件 **50,000** 帧后自动换新

---

## 性能与资源占用（典型）

* **吞吐**：≈ 1000 帧/秒（示例场景）
* **带宽**：≈ 500 KB/s（双通道@10kHz）
* **端到端延迟**：< 50 ms
* **CPU 占用**：< 5%（后台线程阻塞 I/O，不空转）
* **内存**：≈ 6 MB（含共享内存）

---

## 调试与故障排除

### 常见问题

* **键盘无响应**

  * 确认使用的是**线程版 IPC**，主循环中**不要**再调用 `processIPCMessages()`。
* **IPC 连接失败/无消息**

  * 检查管道名与权限；确保 data-processor 以客户端连接。
  * 确认消息以 `\n` 结尾（JSON Lines）。
* **共享内存异常**

  * 检查命名、大小、权限；校验读写是否遵守环形缓冲协议。
* **数据包解析失败**

  * 校验协议版本、帧格式、CRC；打印原始帧以定位。
* **Socket 连接问题**

  * 核对 HOST/PORT；防火墙放行；必要时抓包排查。

### 状态监控

运行中按 `s` 可查看：

```
Connection: CONNECTED (Serial/Socket)
IPC: CONNECTED / LISTENING
Device Connected: YES/NO
Device ID: 0x...
Device Info: ...
Data Transmission: ON/OFF
Total Frames: ...
Data Packets: ...
Current Seq: ...
```

---

## 开发与测试

### 与测试端配合

```bash
# 终端1：启动模拟器（示例路径）
cd ../test-sender
./build/windows/debug/test-sender.exe

# 终端2：启动 data-reader
cd ../data-reader
./build/windows/debug/serialread.exe -s
```

### 常用 Make 目标（示例）

```bash
make               # 调试编译
make BUILD=release # 发布编译
make rebuild       # 清理并重建
make run-socket    # 以 socket 模式运行（如工程内定义）
make test          # 一键编译并连到默认模拟器（如工程内定义）
```

---

## 版本历史

* **v1.4**：IPC 改为**后台线程模式**（`startIPCThread` / `stopIPCThread`），主循环不卡顿，交互更顺畅
* **v1.3**：完善错误处理与状态监控
* **v1.2**：加入 IPC 通信与共享内存
* **v1.1**：增加 Socket 模式
* **v1.0**：基础串口通信 + Protocol V6

---

**协议**：V6
**平台**：Windows（w64devkit / MSVC 均可）
**依赖**：Winsock2、Windows API
