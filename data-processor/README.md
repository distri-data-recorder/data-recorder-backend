# Data Processor - 高性能数据采集与处理系统

一个基于Rust构建的高性能数据采集和处理系统，专为实时传感器数据采集、处理和分发而设计。

## 核心特性

- **直接设备通信**：支持串口（USB-CDC）和TCP套接字连接
- **实时数据处理**：滤波、质量评估和格式转换
- **WebSocket实时流**：向多个客户端实时分发数据
- **完善的REST API**：全面的控制和监控接口
- **触发模式增强**：智能触发批次管理和自定义保存
- **文件管理系统**：安全的数据存储和自动清理
- **跨平台支持**：Windows、Linux和macOS

## 新增功能（v2.0）

### 增强的触发模式支持
- **触发批次管理**：自动缓存最近的触发事件，支持预览和管理
- **自定义保存**：用户可指定文件名、保存路径和格式
- **多格式导出**：支持JSON（含元数据）、CSV（便于分析）、Binary（紧凑）
- **质量评估**：自动评估触发数据质量并提供详细统计
- **实时预览**：WebSocket广播触发事件和批次完成状态

### 智能数据管理
- **批次缓存**：保留最近10个触发批次供用户选择
- **质量监控**：实时评估数据质量并标记异常
- **统计分析**：自动计算每个通道的统计信息
- **内存优化**：智能缓存管理避免内存溢出

## 快速开始

### 安装部署

```bash
# 克隆项目
git clone <repository-url>
cd data-processor

# 构建应用程序
cargo build --release

# 使用默认配置运行
cargo run --release
```

### 环境配置

通过环境变量配置系统：

```bash
# 设备连接配置
export DEVICE_TYPE=socket           # "serial" 或 "socket"
export SOCKET_ADDRESS=127.0.0.1:9001  # Socket模式地址
export SERIAL_PORT=COM7             # 串口模式端口
export BAUD_RATE=115200            # 波特率

# Web服务配置
export WEB_HOST=127.0.0.1
export WEB_PORT=8080
export WS_HOST=127.0.0.1
export WS_PORT=8081

# 存储配置
export DATA_DIR=./data
export FILE_PREFIX=wave
export FILE_EXT=.bin
export MAX_FILES=200
```

### 与设备模拟器联调

```bash
# 终端1：启动设备模拟器
cd device-simulator-v2.1
make run

# 终端2：启动数据处理器
cd data-processor  
cargo run --release
```

访问系统：
- Web API: http://127.0.0.1:8080
- WebSocket: ws://127.0.0.1:8081
- 测试网页: 打开 `trigger_test.html`

## API文档

### 系统状态

#### 获取系统状态
```http
GET /api/control/status
```

**响应示例：**
```json
{
  "success": true,
  "data": {
    "data_collection_active": false,
    "device_connected": true,
    "connected_clients": 2,
    "packets_processed": 15420,
    "uptime_seconds": 3600,
    "memory_usage_mb": 45.2,
    "connection_type": "socket",
    "current_mode": "trigger",
    "trigger_support": true,
    "trigger_status": {
      "cached_bursts": 3,
      "current_burst_active": false,
      "last_trigger_timestamp": 1704067200,
      "total_triggers_received": 25
    }
  },
  "timestamp": 1704067200000
}
```

### 设备控制

#### 启动数据采集
```http
POST /api/control/start
```

#### 停止数据采集
```http
POST /api/control/stop
```

#### 设备连通性测试
```http
POST /api/control/ping
```

#### 获取设备信息
```http
POST /api/control/device_info
```

### 模式控制

#### 设置连续模式
```http
POST /api/control/continuous_mode
```
配置设备进行连续数据流传输。

#### 设置触发模式
```http
POST /api/control/trigger_mode
```
配置设备进行基于事件的触发数据采集。

#### 配置数据流
```http
POST /api/control/configure
Content-Type: application/json
```

**请求体：**
```json
{
  "channels": [
    {
      "channel_id": 0,
      "sample_rate": 10000,
      "format": 1
    },
    {
      "channel_id": 1,
      "sample_rate": 10000,
      "format": 1
    }
  ]
}
```

**格式值说明：**
- `1`: int16
- `2`: int32  
- `4`: float32

### 触发数据管理（新增）

#### 获取触发批次列表
```http
GET /api/trigger/list
```

**响应示例：**
```json
{
  "success": true,
  "data": [
    {
      "burst_id": "trigger_1704067200_1704067205000",
      "trigger_timestamp": 1704067200,
      "trigger_channel": 0,
      "total_samples": 1500,
      "duration_ms": 75.5,
      "created_at": 1704067205000,
      "quality": "Good",
      "can_save": true
    }
  ]
}
```

#### 预览触发批次
```http
GET /api/trigger/preview/{burst_id}
```
获取指定触发批次的详细信息和数据预览。

#### 保存触发批次
```http
POST /api/trigger/save/{burst_id}
Content-Type: application/json
```

**请求体：**
```json
{
  "dir": "experiments/vibration_test",
  "filename": "impact_measurement_001",
  "format": "csv",
  "description": "50Hz振动冲击测试数据"
}
```

**参数说明：**
- `dir` (可选)：相对于data目录的子目录路径
- `filename` (可选)：自定义文件名，未提供则自动生成
- `format` (必需)：导出格式（json、csv、binary）
- `description` (可选)：文件描述信息

**响应示例：**
```json
{
  "success": true,
  "data": {
    "saved_path": "experiments/vibration_test/impact_measurement_001.csv",
    "format": "csv",
    "size_bytes": 125600,
    "burst_info": {
      "burst_id": "trigger_1704067200_1704067205000",
      "quality": "Good",
      "total_samples": 1500
    }
  }
}
```

#### 删除触发批次缓存
```http
DELETE /api/trigger/delete/{burst_id}
```

### 文件管理

#### 列出文件
```http
GET /api/files
GET /api/files?dir=subfolder
```

#### 下载文件
```http
GET /api/files/{filename}
```
支持子目录路径，如 `subfolder/file.bin`。

#### 保存数据文件
```http
POST /api/files/save
Content-Type: application/json
```

**请求体：**
```json
{
  "dir": "measurements/2024-01-01",
  "filename": "test_data.bin",
  "base64": "AAABAAACAAADAAAEAAAF..."
}
```

### 系统健康检查

#### 健康状态
```http
GET /health
```

## WebSocket接口

### 连接
连接到：`ws://{host}:{port}`

### 订阅控制
```javascript
// 订阅所有事件
ws.send(JSON.stringify({
  type: 'subscribe',
  channels: ['all']
}));

// 仅订阅触发事件
ws.send(JSON.stringify({
  type: 'subscribe',
  channels: ['trigger_events', 'trigger_bursts']
}));
```

### 数据消息类型

#### 实时数据流
```json
{
  "type": "data",
  "timestamp": 1704067200000,
  "sequence": 12345,
  "channel_count": 2,
  "sample_rate": 10000.0,
  "data": [1.23, 1.24, 1.25, ...],
  "data_type": {
    "source": "Trigger",
    "trigger_info": {
      "trigger_timestamp": 1704067200,
      "is_complete": false,
      "sequence_in_burst": 3
    }
  }
}
```

#### 触发事件通知
```json
{
  "type": "trigger_event",
  "timestamp": 1704067200,
  "channel": 0,
  "pre_samples": 1000,
  "post_samples": 1000,
  "event_time": 1704067205000
}
```

#### 触发批次完成（新增）
```json
{
  "type": "trigger_burst_complete",
  "burst_id": "trigger_1704067200_1704067205000",
  "trigger_timestamp": 1704067200,
  "trigger_channel": 0,
  "total_samples": 1500,
  "total_packets": 8,
  "duration_ms": 75.5,
  "quality": "Good",
  "can_save": true,
  "preview_samples": [1.23, 1.24, 1.25, ...],
  "voltage_range": [0.1, 3.2]
}
```

## 测试网页

项目包含一个完整的测试网页 `trigger_test.html`，提供：

- **实时系统监控**：连接状态、数据包统计、设备状态
- **设备控制界面**：模式切换、数据采集控制
- **实时数据可视化**：双通道数据图表显示
- **触发批次管理**：列表显示、预览、保存、删除
- **智能保存界面**：自定义路径、文件名、格式选择
- **实时日志显示**：操作记录和系统状态

### 使用方法

1. 确保data-processor和device-simulator运行
2. 在浏览器中打开 `trigger_test.html`
3. 点击"Set Trigger Mode"切换到触发模式
4. 点击"Start Collection"开始数据采集
5. 等待触发事件发生并在右侧面板查看
6. 选择感兴趣的触发批次进行保存

## 典型使用流程

### 触发模式数据采集
```bash
# 1. 设置触发模式
curl -X POST http://127.0.0.1:8080/api/control/trigger_mode

# 2. 启动数据采集
curl -X POST http://127.0.0.1:8080/api/control/start

# 3. 等待触发事件发生，查看可用批次
curl http://127.0.0.1:8080/api/trigger/list

# 4. 预览感兴趣的批次
curl http://127.0.0.1:8080/api/trigger/preview/{burst_id}

# 5. 保存重要数据
curl -X POST http://127.0.0.1:8080/api/trigger/save/{burst_id} \
-H "Content-Type: application/json" \
-d '{
  "dir": "experiment_data",
  "filename": "vibration_test_001", 
  "format": "csv",
  "description": "振动台50Hz测试数据"
}'
```

## 系统架构

### 核心组件

```
data-processor/
├── src/
│   ├── main.rs                    # 应用程序入口
│   ├── config.rs                  # 配置管理
│   ├── device_communication.rs    # 设备协议实现
│   ├── data_processing.rs         # 实时数据处理和触发批次管理
│   ├── web_server.rs             # REST API服务器
│   ├── websocket.rs              # WebSocket流媒体
│   └── file_manager.rs           # 文件存储管理
├── Cargo.toml                    # Rust依赖配置
├── trigger_test.html             # 完整测试界面
└── .env                         # 环境变量配置
```

### 数据流

```
设备 → 协议解析器 → 数据处理器 → 质量评估器 → {WebSocket广播, 文件存储, 批次管理}
                                        ↑
                             REST API ← Web服务器
```

### 并发模型

- **异步运行时**：基于Tokio异步运行时
- **消息传递**：使用通道进行任务间通信
- **共享状态**：最小化共享状态，合理同步
- **背压处理**：优雅处理慢速消费者

## 配置参考

### 环境变量

| 变量 | 默认值 | 描述 |
|------|--------|------|
| `DEVICE_TYPE` | socket | 连接类型："serial" 或 "socket" |
| `SERIAL_PORT` | COM7 | USB-CDC连接的串口 |
| `SOCKET_ADDRESS` | 127.0.0.1:9001 | 测试用TCP套接字地址 |
| `BAUD_RATE` | 115200 | 串行通信波特率 |
| `WEB_HOST` | 127.0.0.1 | HTTP服务器绑定地址 |
| `WEB_PORT` | 8080 | HTTP服务器端口 |
| `WS_HOST` | 127.0.0.1 | WebSocket服务器绑定地址 |
| `WS_PORT` | 8081 | WebSocket服务器端口 |
| `DATA_DIR` | ./data | 数据存储目录 |
| `FILE_PREFIX` | wave | 自动生成文件名前缀 |
| `FILE_EXT` | .bin | 自动生成文件扩展名 |
| `MAX_FILES` | 200 | 数据目录最大文件数 |

### 数据处理设置

系统应用以下处理管道：
1. **协议解析**：验证帧并提取数据
2. **格式转换**：将原始ADC值转换为工程单位
3. **滤波处理**：应用5点移动平均平滑
4. **质量评估**：评估数据质量并标记异常
5. **批次管理**：自动管理触发数据批次

### 触发模式配置

- **批次缓存数量**：10个（可配置）
- **预触发采样**：1000个样本（设备配置）
- **后触发采样**：1000个样本（设备配置）
- **质量评估**：电压范围、饱和度、信号平坦度检查
- **自动清理**：按创建时间保留最新批次

## 部署指南

### Docker部署
```bash
# 构建容器
docker build -t data-processor .

# 运行，映射端口和数据目录
docker run -d \
  -p 8080:8080 \
  -p 8081:8081 \
  -v $(pwd)/data:/app/data \
  -e DEVICE_TYPE=socket \
  -e SOCKET_ADDRESS=host.docker.internal:9001 \
  data-processor
```

### 生产环境考虑

#### 安全性
- 使用最小权限运行
- 防火墙限制WebSocket访问
- 验证所有文件路径防止目录遍历
- 监控API访问和速率限制

#### 性能优化
- 为数据缓冲区分配足够内存
- 高吞吐量时监控CPU使用
- 数据目录使用SSD存储
- 考虑数据保留策略

#### 监控指标
- 定期监控系统状态端点
- 设置设备断连告警
- 跟踪内存和磁盘使用
- 分析错误日志模式

## 故障排除

### 常见问题

#### 设备连接问题
```bash
# 检查设备可用性
# 串口：验证端口存在和权限
ls -la /dev/tty* # Linux

# 套接字：测试连通性
telnet 127.0.0.1 9001
```

**解决方案：**
- 验证设备配置
- 检查线缆连接
- 确认设备模拟器运行

#### 内存使用过高
监控项目：
- WebSocket客户端连接数
- 数据目录大小
- 缓冲区配置

**解决方案：**
- 断开未使用的WebSocket客户端
- 增加MAX_FILES清理阈值
- 如果怀疑内存泄漏则重启服务

#### 数据质量问题
检查项目：
- 设备信号完整性
- 采样率设置是否合适
- 电气干扰监控

**解决方案：**
- 调整触发阈值
- 使用更好的屏蔽电缆
- 正确接地设备

## 开发指南

### 从源码构建
```bash
# 安装Rust工具链
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh

# 克隆和构建
git clone <repository>
cd data-processor
cargo build --release

# 运行测试
cargo test

# 代码检查
cargo clippy
cargo fmt
```

### 添加功能

1. **新API端点**：修改 `web_server.rs`
2. **设备命令**：扩展 `device_communication.rs` 
3. **数据处理**：更新 `data_processing.rs`
4. **配置项**：添加到 `config.rs` 和环境变量

### 协议扩展

添加新设备命令的步骤：
1. 在 `device_communication.rs` 中定义命令ID
2. 实现请求/响应处理逻辑
3. 如需要，添加对应的API端点
4. 更新协议文档

## 更新日志

### v2.0 (2024-12-31) - 触发模式增强
- **新增**：触发批次智能管理系统
- **新增**：自定义文件保存功能
- **新增**：多格式导出（JSON、CSV、Binary）
- **新增**：实时数据质量评估
- **新增**：完整的测试网页界面
- **改进**：WebSocket事件系统
- **改进**：内存管理和性能优化
- **改进**：错误处理和日志记录

### v1.0 (2024-11-01) - 初始版本
- 基础设备通信功能
- 连续模式数据采集
- REST API和WebSocket接口
- 文件管理系统

## 许可证

本项目基于MIT许可证开源 - 详见LICENSE文件。

## 技术支持

如有问题和功能请求，请使用项目的issue跟踪器。

---

**重要提示**：触发模式的增强功能专为精密测量和数据分析场景设计，提供了完整的数据生命周期管理，从实时采集到智能存储，满足专业应用的严格要求。