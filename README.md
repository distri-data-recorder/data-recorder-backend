# 数据采集系统后端

这是一个高性能的信号采集系统后端，包含数据采集进程和数据处理进程，通过共享内存和消息队列实现高效的进程间通信。

## 系统架构

```
┌─────────────────┐    USB-CDC    ┌─────────────────┐    共享内存    ┌─────────────────┐
│                 │ ──────────────▶│                 │ ──────────────▶│                 │
│   下位机设备     │               │  数据采集进程    │               │  数据处理进程    │
│   (信号源)      │               │  (data-reader)  │               │ (data-processor) │
│                 │               │                 │ ◀──────────────│                 │
└─────────────────┘               └─────────────────┘    消息队列    └─────────────────┘
                                                                            │
                                                                            │ WebSocket
                                                                            ▼
                                                                    ┌─────────────────┐
                                                                    │                 │
                                                                    │     前端界面     │
                                                                    │                 │
                                                                    └─────────────────┘
                                                                            ▲
                                                                            │ HTTPS API
                                                                            │
```

## 项目结构

```
data-recorder-backend/
├── data-reader/                 # 数据采集进程 (C语言)
│   ├── serialread.c            # 主程序
│   ├── shared_memory.h/.c      # 共享内存模块
│   ├── protocol/               # 通信协议
│   │   ├── protocol.h/.c       # 帧解析
│   │   └── io_buffer.h/.c      # 缓冲区管理
│   └── Makefile               # 构建配置
├── data-processor/             # 数据处理进程 (Rust语言)
│   ├── src/
│   │   ├── main.rs            # 主程序入口
│   │   ├── config.rs          # 配置管理
│   │   ├── ipc.rs             # 进程间通信
│   │   ├── data_processing.rs # 数据处理核心
│   │   ├── web_server.rs      # HTTPS API服务器
│   │   ├── websocket.rs       # WebSocket服务器
│   │   └── file_manager.rs    # 文件管理
│   └── Cargo.toml             # Rust依赖配置
├── ipc_design.md              # 进程间通信设计文档
├── test_system.ps1            # 系统测试脚本
├── quick_test.ps1             # 快速测试脚本
├── TESTING_GUIDE.md           # 详细测试指南
└── README.md                  # 项目说明文档
```

## 功能特性

### 数据采集进程 (data-reader)
- **串口通信**: 通过USB-CDC接收下位机数据
- **协议解析**: 解析帧格式，提取ADC数据包
- **共享内存**: 高性能数据传输到处理进程
- **文件保存**: 原始帧数据持久化存储
- **实时监控**: 交互式命令行界面

### 数据处理进程 (data-processor)
- **数据处理**: ADC数据解析、滤波、格式转换
- **WebSocket服务**: 实时数据推送到前端
- **HTTPS API**: 控制命令和文件下载接口
- **文件管理**: 波形文件保存和管理
- **配置管理**: 灵活的系统配置

## 进程间通信

### 共享内存
- **名称**: `ADC_DATA_SHARED_MEM`
- **大小**: 约4.2MB (1024个数据包缓冲区)
- **同步**: 原子操作保证线程安全
- **数据格式**: 时间戳 + 序列号 + ADC载荷

### 消息队列
- **控制信号**: 开始/停止采集、状态查询
- **文件操作**: 波形保存请求
- **错误处理**: 异常情况通知

## API接口

### HTTPS API (默认端口: 8443)
- `POST /api/control/start` - 开始数据采集
- `POST /api/control/stop` - 停止数据采集
- `GET /api/control/status` - 获取系统状态
- `GET /api/files` - 列出可用文件
- `GET /api/files/{filename}` - 下载文件
- `POST /api/files/save` - 保存当前波形

### WebSocket (默认端口: 8080)
- 实时数据流推送
- 客户端连接管理
- 数据质量监控

## 快速开始

### 环境要求
- Windows 10/11
- GCC编译器 (MinGW)
- Rust 1.70+ (可选，用于数据处理进程)
- Visual Studio Build Tools (Rust编译需要)

### 编译和运行

1. **编译数据采集进程**:
```bash
cd data-reader
gcc -std=c11 -Wall -Wextra -Wno-unused-parameter -Iprotocol -O0 -g -c serialread.c -o serialread.o
gcc -std=c11 -Wall -Wextra -Wno-unused-parameter -Iprotocol -O0 -g -c shared_memory.c -o shared_memory.o
gcc serialread.o protocol/protocol.o protocol/io_buffer.o shared_memory.o -o serialread.exe -lkernel32 -luser32
```

2. **编译数据处理进程** (需要Visual Studio Build Tools):
```bash
cd data-processor
cargo build --release
```

3. **运行快速测试**:
```powershell
# 快速测试（推荐）
.\quick_test.ps1

# 或运行完整系统测试
.\test_system.ps1
```

### 使用方法

1. **启动数据采集进程**:
```bash
cd data-reader
.\serialread.exe [COM端口号]
```

2. **启动数据处理进程**:
```bash
cd data-processor
cargo run --release
```

3. **连接前端**:
   - WebSocket: `ws://localhost:8080`
   - API: `https://localhost:8443`

## 测试指南

### 快速测试
```powershell
# 运行快速测试脚本
.\quick_test.ps1

# 仅编译测试
.\quick_test.ps1 -BuildOnly

# 仅功能测试
.\quick_test.ps1 -TestOnly

# 详细输出
.\quick_test.ps1 -Verbose
```

### 详细测试
请参考 [TESTING_GUIDE.md](TESTING_GUIDE.md) 获取完整的测试步骤和故障排除指南。

## 配置说明

### 数据采集进程配置
- 默认COM端口: COM7
- 波特率: 115200
- 帧缓存: 500帧批量保存
- 文件大小: 每文件最多5万帧

### 数据处理进程配置
- 共享内存名称: `ADC_DATA_SHARED_MEM`
- WebSocket端口: 8080
- HTTPS端口: 8443
- 处理间隔: 10ms

## 开发状态

✅ **已完成**:
- 进程间通信架构设计
- 数据采集进程 (C语言实现)
- 数据处理进程框架 (Rust语言)
- 共享内存通信模块
- WebSocket和HTTPS服务器
- 文件管理功能
- 系统集成测试

🔄 **待完成**:
- Visual Studio Build Tools安装
- Rust项目完整编译测试
- 端到端数据流测试
- 前端界面集成
- 性能优化和调试

## 技术栈

- **数据采集**: C语言 + Windows API
- **数据处理**: Rust + Tokio异步运行时
- **Web服务**: Axum框架 + WebSocket
- **进程通信**: 共享内存 + 消息队列
- **数据格式**: JSON + 二进制协议

## 许可证

本项目为内部开发项目，版权所有。