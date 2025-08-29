# Data Reader - Protocol V6 设备通信工具

一个独立的设备通信工具，完整实现 Protocol V6 协议，支持串口和TCP Socket双连接模式。

## 概述

**Data Reader** 是一个轻量级的设备通信工具，专门用于与下位机设备或设备模拟器通信。它完整实现了 Protocol V6 协议的所有功能，包括设备控制、数据接收、状态查询等，同时提供原始帧数据的文件记录功能。

## 核心特性

### 双模式通信
- **串口模式**：与真实设备通过 COM 口通信
- **Socket 模式**：与测试模拟器通过 TCP 连接

### Protocol V6 完整支持
- **系统控制**：PING/PONG、设备信息查询、状态监控
- **采集控制**：模式切换（连续/触发）、启动/停止、参数配置
- **数据接收**：数据包、触发事件、缓冲区传输、设备日志

### 数据管理
- **原始帧记录**：按批落盘，减少磁盘 IO 峰值
- **统计监控**：实时显示通信统计和设备状态
- **交互式控制**：键盘命令实时控制设备

## 文件结构

```
data-reader/
├── serialread.c            # 主程序（通信主循环、键盘交互、文件记录）
├── protocol/
│   ├── protocol.h          # Protocol V6 协议实现
│   ├── protocol.c
│   ├── io_buffer.h         # 流式帧解析缓冲
│   └── io_buffer.c
├── Makefile                # 构建配置
└── README.md
```

## 编译和运行

### 编译

```bash
# 调试版本
make

# 发布版本
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

## 命令行参数

- 无参数：默认使用 `COM7`
- `N`：数字形式指定 `COMN`（例如 `3` → `COM3`）
- `-s [HOST] [PORT]`：启用 TCP 客户端模式（默认 `127.0.0.1:9001`）
- `-h` 或 `--help`：显示使用帮助

## 运行期键盘命令

| 键         | 说明                     | 协议命令                      |
|-----------|------------------------|-----------------------------|
| `h`       | 显示帮助                 | -                           |
| `s`       | 显示当前状态             | -                           |
| `p`       | 发送 PING               | `CMD_PING`                  |
| `i`       | 获取设备信息             | `CMD_GET_DEVICE_INFO`       |
| `1`       | 设置连续模式             | `CMD_SET_MODE_CONTINUOUS`   |
| `2`       | 设置触发模式             | `CMD_SET_MODE_TRIGGER`      |
| `3`       | 开始数据流               | `CMD_START_STREAM`          |
| `4`       | 停止数据流               | `CMD_STOP_STREAM`           |
| `c`       | 发送流配置示例           | `CMD_CONFIGURE_STREAM`      |
| `ESC/q`   | 退出程序                 | -                           |

## Protocol V6 支持

### 系统控制命令
- `CMD_PING (0x01)` / `CMD_PONG (0x81)` - 设备发现
- `CMD_GET_DEVICE_INFO (0x03)` - 获取设备能力信息
- `CMD_GET_STATUS (0x02)` - 查询设备状态

### 采集控制命令
- `CMD_SET_MODE_CONTINUOUS (0x10)` - 设置连续采集模式
- `CMD_SET_MODE_TRIGGER (0x11)` - 设置触发采集模式
- `CMD_START_STREAM (0x12)` - 开始数据采集
- `CMD_STOP_STREAM (0x13)` - 停止数据采集
- `CMD_CONFIGURE_STREAM (0x14)` - 配置采集参数

### 数据传输命令
- `CMD_DATA_PACKET (0x40)` - ADC数据包
- `CMD_EVENT_TRIGGERED (0x41)` - 触发事件通知
- `CMD_REQUEST_BUFFERED_DATA (0x42)` - 请求缓冲数据
- `CMD_BUFFER_TRANSFER_COMPLETE (0x4F)` - 传输完成信号

### 日志命令
- `CMD_LOG_MESSAGE (0xE0)` - 设备日志消息

## 数据文件格式

### 原始帧记录
- **文件名**：`raw_frames_000.txt`, `raw_frames_001.txt`, ...
- **格式**：`LEN:18 HEX: AA 55 0C 00 ...`
- **策略**：每 500 帧批量写入；单文件 50,000 帧后自动换新

## 系统架构

```
┌─────────────┐   Protocol V6   ┌──────────────┐
│   Device    │ ◄─────────────► │  data-reader │
│ / Simulator │   (Binary)      │              │
└─────────────┘                 └──────────────┘
                                         │
                                         ▼
                                ┌─────────────────┐
                                │  Raw Frame      │
                                │  Files          │
                                └─────────────────┘
```

## 性能指标

- **吞吐量**：≈ 1000 帧/秒
- **延迟**：< 50 ms 端到端
- **CPU 占用**：< 5%
- **内存使用**：< 10 MB

## 使用场景

### 设备调试
- 实时监控设备通信状态
- 验证协议命令响应
- 记录原始通信数据供分析

### 协议测试
- 测试 Protocol V6 命令集
- 验证数据包格式
- 触发模式功能测试

### 开发支持
- 与 test-sender 配合进行集成测试
- 提供独立的设备通信能力
- 支持自动化测试脚本

## 开发与测试

### 与测试端配合

```bash
# 终端1：启动设备模拟器
cd ../test-sender
make run

# 终端2：启动 data-reader
cd ../data-reader
make run-socket
```

### 常用测试命令

```bash
make test          # 连接到 test-sender 进行测试
make test-com3     # 测试 COM3 连接
make test-com7     # 测试 COM7 连接
```

## 故障排除

### 连接问题
- **串口连接失败**：检查 COM 口是否被占用，确认设备连接
- **Socket 连接失败**：确认目标地址和端口，检查防火墙设置

### 通信异常
- **帧解析错误**：检查协议版本兼容性，验证 CRC 校验
- **命令无响应**：确认设备状态，检查命令格式

### 性能问题
- **数据丢失**：降低数据传输频率，检查系统负载
- **文件写入慢**：使用 SSD 存储，调整批写入大小

## 版本历史

- **v2.0**：简化架构，移除 IPC 和共享内存依赖，专注设备通信功能
- **v1.4**：IPC 后台线程模式，改善交互响应
- **v1.3**：完善错误处理与状态监控
- **v1.2**：增加 IPC 通信与共享内存
- **v1.1**：增加 Socket 模式支持
- **v1.0**：基础串口通信 + Protocol V6

---

**协议版本**：V6  
**平台支持**：Windows、Linux  
**编译器**：GCC, MinGW, MSVC