# Data Reader - 通用数据采集系统

## 概述

Data Reader 是通用数据采集系统的核心组件，负责与下位机设备通信，实现协议V6的完整功能。它作为中间件，连接硬件设备和上层数据处理模块。

## 核心功能

### 双模式通信支持
- **串口模式**: 与真实硬件设备通信 (COM端口)
- **Socket模式**: 与测试模拟器通信 (TCP连接)

### 协议V6完整实现
- 系统控制命令 (PING/PONG, 设备信息查询)
- 采集配置命令 (模式切换, 流控制, 通道配置)
- 数据传输处理 (数据包接收, 触发事件处理)
- 设备日志接收和转发

### IPC通信支持
- Windows命名管道服务器
- JSON消息格式通信
- 与data-processor的双向通信

### 数据管理
- 共享内存数据写入
- 原始帧文件记录
- 实时数据流处理

## 文件结构

```
data-reader/
├── serialread.c              # 主程序文件
├── ipc_communication.h       # IPC通信头文件
├── ipc_communication.c       # IPC通信实现
├── shared_memory.h           # 共享内存头文件
├── shared_memory.c           # 共享内存实现
├── protocol/                 # 协议实现模块
│   ├── protocol.h
│   ├── protocol.c
│   ├── io_buffer.h
│   └── io_buffer.c
├── Makefile                  # 构建脚本
├── build/                    # 构建输出目录
│   └── windows/
│       ├── debug/
│       └── release/
└── README.md                 # 本文档
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

### 运行选项

**串口模式**:
```bash
# 默认COM7
./serialread.exe

# 指定COM口
./serialread.exe 3          # 使用COM3
./serialread.exe 7          # 使用COM7
```

**Socket模式** (测试用):
```bash
# 默认连接127.0.0.1:9001
./serialread.exe -s

# 指定主机
./serialread.exe -s 192.168.1.100

# 指定主机和端口
./serialread.exe -s 192.168.1.100 8080
```

## 交互命令

程序运行时支持以下键盘命令：

| 命令 | 功能 | 协议命令 |
|------|------|----------|
| `p` | 发送PING测试连接 | CMD_PING |
| `i` | 获取设备详细信息 | CMD_GET_DEVICE_INFO |
| `1` | 设置连续采集模式 | CMD_SET_MODE_CONTINUOUS |
| `2` | 设置事件触发模式 | CMD_SET_MODE_TRIGGER |
| `3` | 开始数据流 | CMD_START_STREAM |
| `4` | 停止数据流 | CMD_STOP_STREAM |
| `c` | 配置数据流(演示) | CMD_CONFIGURE_STREAM |
| `s` | 显示当前状态 | - |
| `h` | 显示帮助信息 | - |
| `ESC/q` | 退出程序 | - |

## 典型使用流程

### 1. 设备连接和初始化
```
启动程序 → 发送PING → 收到PONG响应 → 设备连接成功
```

### 2. 获取设备信息
```
按 'i' → 发送GET_DEVICE_INFO → 接收设备详细信息
显示: 协议版本、固件版本、通道数、通道能力等
```

### 3. 配置和启动采集
```
按 'c' → 配置数据流 → 按 '1' → 设置连续模式 → 按 '3' → 开始数据采集
```

### 4. 数据接收
```
接收CMD_DATA_PACKET → 解析数据包头 → 写入共享内存 → 显示统计信息
```

## IPC通信接口

### 命名管道
- **管道名称**: `\\.\pipe\data_reader_ipc`
- **消息格式**: JSON Lines (每行一个JSON对象)
- **连接类型**: 双向通信

### 接收的消息类型 (来自data-processor)

**FORWARD_TO_DEVICE**
```json
{
  "id": "msg_001",
  "timestamp": "2024-12-31T10:00:00Z",
  "type": "FORWARD_TO_DEVICE",
  "payload": {
    "command_id": "0x03",
    "data": "Base64EncodedData"
  }
}
```

**SET_READER_MODE**
```json
{
  "id": "msg_002", 
  "timestamp": "2024-12-31T10:00:00Z",
  "type": "SET_READER_MODE",
  "payload": {
    "mode": "socket",
    "target": "127.0.0.1:9001"
  }
}
```

**REQUEST_READER_STATUS**
```json
{
  "id": "msg_003",
  "timestamp": "2024-12-31T10:00:00Z", 
  "type": "REQUEST_READER_STATUS",
  "payload": {}
}
```

### 发送的消息类型 (发给data-processor)

**READER_STATUS_UPDATE**
```json
{
  "id": "msg_101",
  "timestamp": "2024-12-31T10:00:01Z",
  "type": "READER_STATUS_UPDATE", 
  "payload": {
    "mode": "socket",
    "target": "127.0.0.1:9001",
    "device_connected": true,
    "device_id": "11223344AABBCCDD"
  }
}
```

**DEVICE_FRAME_RECEIVED**
```json
{
  "id": "msg_102",
  "timestamp": "2024-12-31T10:00:02Z",
  "type": "DEVICE_FRAME_RECEIVED",
  "payload": {
    "command_id": "0x41",
    "seq": 5,
    "data": "Base64EncodedFrameData"
  }
}
```

**DEVICE_LOG_RECEIVED**
```json
{
  "id": "msg_103", 
  "timestamp": "2024-12-31T10:00:03Z",
  "type": "DEVICE_LOG_RECEIVED",
  "payload": {
    "level": "INFO",
    "message": "Stream started"
  }
}
```

## 数据流向

```
┌─────────────┐    协议V6    ┌──────────────┐    共享内存     ┌──────────────┐
│   下位机    │ ◄─────────► │ data-reader  │ ────────────► │ data-processor│
│  /测试器    │   二进制帧   │              │   ADC数据包    │              │
└─────────────┘              └──────────────┘                └──────────────┘
                                    │                               ▲
                                    │ IPC命名管道                    │
                                    │ JSON消息                      │
                                    └───────────────────────────────┘
                                         控制命令/状态通知
```

## 共享内存数据结构

### 数据包格式
```c
typedef struct {
    uint32_t timestamp_ms;    // 时间戳
    uint16_t sequence;        // 序列号  
    uint16_t payload_len;     // 数据长度
    uint8_t  payload[4096];   // ADC数据内容
} ADCDataPacket;
```

### 共享内存布局
- **名称**: `ADC_DATA_SHARED_MEM`
- **大小**: ~4MB (1024个数据包的循环缓冲区)
- **访问**: 读写互斥，支持多进程访问

## 文件输出

### 原始帧记录
- **文件名格式**: `raw_frames_000.txt`, `raw_frames_001.txt` ...
- **内容格式**: `LEN:18 HEX: AA 55 0C 00 01 00 11 22 33 44 AA BB CC DD E7 A1 55 AA`
- **分割策略**: 每个文件最多50,000帧，超出自动切换新文件

### 批量写入机制
- 内存中缓存500帧后批量写入文件
- 程序退出时自动刷新剩余缓存
- 避免频繁磁盘IO影响实时性能

## 错误处理

### 连接错误
- **串口**: 自动重试机制，显示详细错误信息
- **Socket**: 连接断开后显示错误，支持手动重连
- **IPC**: 客户端断开后自动恢复监听状态

### 协议错误  
- **帧解析失败**: 记录错误并继续处理后续数据
- **CRC校验失败**: 丢弃错误帧，统计错误次数
- **命令响应超时**: 显示警告，不影响数据接收

### 资源错误
- **共享内存创建失败**: 警告提示，程序继续运行
- **文件写入失败**: 警告提示，原始帧记录停止
- **内存分配失败**: 优雅处理，避免程序崩溃

## 性能指标

### 数据吞吐量
- **最大帧率**: 1000帧/秒 (典型应用场景)
- **数据带宽**: ~500KB/s (双通道@10kHz)
- **延迟**: 端到端延迟 < 50ms

### 资源占用
- **内存使用**: ~6MB (包括共享内存)
- **CPU占用**: < 5% (正常数据流)
- **磁盘IO**: 批量写入，低峰值影响

## 调试和诊断

### 日志级别
- **设备日志**: 通过CMD_LOG_MESSAGE接收，自动分类显示
- **系统日志**: 连接状态、错误信息等输出到控制台
- **调试信息**: 编译debug版本时显示详细帧解析过程

### 状态监控
```bash
# 运行时按 's' 查看状态
Connection: CONNECTED (Socket)
IPC: CONNECTED  
Device Connected: YES
Device ID: 0x11223344AABBCCDD
Device Info: Protocol V6, FW v2.0, 2 channels
Data Transmission: ON
Total Frames: 1523
Data Packets: 1456
Current Seq: 67
```

## 开发和测试

### 与test-sender配合测试
```bash
# 终端1: 启动模拟器
cd ../test-sender  
./build/windows/debug/test-sender.exe

# 终端2: 启动data-reader
cd ../data-reader
./build/windows/debug/serialread.exe -s
```

### 快速测试命令
```bash
make test           # 编译并连接到test-sender
make run-socket     # Socket模式运行
make test-com7      # 测试COM7连接
```

## 故障排除

### 常见问题

**无法连接设备**
- 检查COM口是否被其他程序占用
- 确认设备波特率设置正确
- Socket模式下确认目标地址和端口

**数据包解析失败**
- 检查CRC校验算法是否一致
- 确认协议版本匹配
- 检查帧格式定义

**IPC连接失败**
- 确认命名管道权限
- 检查是否有防火墙拦截
- 验证JSON消息格式正确性

**共享内存问题**
- 检查系统内存是否充足
- 确认进程间访问权限
- 验证数据结构版本兼容性

### 调试技巧
1. 使用debug版本查看详细日志
2. 监控原始帧文件验证通信正确性
3. 使用系统工具检查命名管道状态
4. 通过状态命令实时监控运行情况

## 后续开发计划

- [ ] 支持更多数据格式 (float32, int32)
- [ ] 实现配置文件管理
- [ ] 添加性能监控和统计
- [ ] 支持多设备并发连接
- [ ] Web管理界面集成

## 版本历史

- **v1.0**: 基础串口通信和协议V6实现
- **v1.1**: 添加Socket模式支持
- **v1.2**: 集成IPC通信和共享内存
- **v1.3**: 完善错误处理和状态监控

---

**协议版本**: V6  
**编译环境**: Windows + w64devkit  
**依赖库**: Winsock2, Windows API