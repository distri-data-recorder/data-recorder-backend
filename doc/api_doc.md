# 数据采集系统前端开发API文档

## 概述

本文档详细描述了高性能数据采集与处理系统的所有前端API接口。系统提供REST API用于控制和管理，WebSocket接口用于实时数据流传输。

### 服务地址

- **HTTP API**: `http://127.0.0.1:8080`
- **WebSocket**: `ws://127.0.0.1:8081`

### 通用响应格式

所有API响应都遵循统一的格式：

```json
{
  "success": boolean,
  "data": any | null,
  "error": string | null,
  "timestamp": number
}
```

## 系统控制API

### 1. 获取系统状态

**接口**: `GET /api/control/status`

**描述**: 获取系统的完整状态信息，包括设备连接、数据处理、触发模式等状态。

**请求**: 无需参数

**响应示例**:
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
  "error": null,
  "timestamp": 1704067200000
}
```

**字段说明**:
- `data_collection_active`: 数据采集是否激活
- `device_connected`: 设备连接状态
- `connected_clients`: WebSocket连接的客户端数量
- `packets_processed`: 已处理的数据包总数
- `uptime_seconds`: 系统运行时间（秒）
- `memory_usage_mb`: 内存使用量（MB）
- `connection_type`: 连接类型（"serial"或"socket"）
- `current_mode`: 当前工作模式（"continuous"或"trigger"）
- `trigger_status`: 触发模式详细状态

### 2. 启动数据采集

**接口**: `POST /api/control/start`

**描述**: 开始数据采集流程。

**请求**: 无需参数

**响应示例**:
```json
{
  "success": true,
  "data": "Data collection started",
  "error": null,
  "timestamp": 1704067200000
}
```

### 3. 停止数据采集

**接口**: `POST /api/control/stop`

**描述**: 停止数据采集流程。

**请求**: 无需参数

**响应示例**:
```json
{
  "success": true,
  "data": "Data collection stopped",
  "error": null,
  "timestamp": 1704067200000
}
```

### 4. 设备Ping测试

**接口**: `POST /api/control/ping`

**描述**: 向设备发送ping命令测试连通性。

**请求**: 无需参数

**响应示例**:
```json
{
  "success": true,
  "data": "Ping command sent to device",
  "error": null,
  "timestamp": 1704067200000
}
```

### 5. 获取设备信息

**接口**: `POST /api/control/device_info`

**描述**: 请求获取设备详细信息。

**请求**: 无需参数

**响应示例**:
```json
{
  "success": true,
  "data": "Device info request sent",
  "error": null,
  "timestamp": 1704067200000
}
```

## 模式控制API

### 6. 设置连续模式

**接口**: `POST /api/control/continuous_mode`

**描述**: 将设备切换到连续采集模式。

**请求**: 无需参数

**响应示例**:
```json
{
  "success": true,
  "data": "Continuous mode command sent",
  "error": null,
  "timestamp": 1704067200000
}
```

### 7. 设置触发模式

**接口**: `POST /api/control/trigger_mode`

**描述**: 将设备切换到触发采集模式。

**请求**: 无需参数

**响应示例**:
```json
{
  "success": true,
  "data": "Trigger mode command sent",
  "error": null,
  "timestamp": 1704067200000
}
```

### 8. 请求触发数据

**接口**: `POST /api/control/request_trigger_data`

**描述**: 手动请求设备发送触发缓冲数据（仅在触发模式下有效）。

**请求**: 无需参数

**响应示例**:
```json
{
  "success": true,
  "data": "Buffered data request sent",
  "error": null,
  "timestamp": 1704067200000
}
```

**错误响应**:
```json
{
  "success": false,
  "data": null,
  "error": "Device not in trigger mode",
  "timestamp": 1704067200000
}
```

### 9. 配置数据流

**接口**: `POST /api/control/configure`

**描述**: 配置设备的采样参数，包括通道、采样率和数据格式。

**请求体**:
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

**参数说明**:
- `channel_id`: 通道ID（0-15）
- `sample_rate`: 采样率（Hz）
- `format`: 数据格式（1=int16, 2=int32, 4=float32）

**响应示例**:
```json
{
  "success": true,
  "data": "Stream configuration sent",
  "error": null,
  "timestamp": 1704067200000
}
```

## 触发数据管理API

### 10. 获取触发批次列表

**接口**: `GET /api/trigger/list`

**描述**: 获取当前缓存的所有触发批次摘要信息。

**请求**: 无需参数

**响应示例**:
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
  ],
  "error": null,
  "timestamp": 1704067200000
}
```

**字段说明**:
- `burst_id`: 批次唯一标识符
- `trigger_timestamp`: 触发时间戳
- `trigger_channel`: 触发通道ID
- `total_samples`: 总采样点数
- `duration_ms`: 数据持续时间（毫秒）
- `created_at`: 批次创建时间
- `quality`: 数据质量（"Good"/"Warning"/"Error"）
- `can_save`: 是否可以保存

### 11. 预览触发批次

**接口**: `GET /api/trigger/preview/{burst_id}`

**描述**: 获取指定触发批次的详细信息和完整数据。

**路径参数**:
- `burst_id`: 批次ID

**响应示例**:
```json
{
  "success": true,
  "data": {
    "burst_id": "trigger_1704067200_1704067205000",
    "trigger_timestamp": 1704067200,
    "trigger_channel": 0,
    "pre_samples": 1000,
    "post_samples": 1000,
    "data_packets": [
      {
        "timestamp": 1704067200100,
        "sequence": 12345,
        "channel_count": 2,
        "sample_rate": 10000.0,
        "data": [1.23, 1.24, 1.25],
        "metadata": {
          "packet_count": 1,
          "processing_time_us": 500,
          "data_quality": {
            "status": "Good"
          },
          "channel_info": [
            {
              "channel_id": 0,
              "sample_count": 100,
              "min_value": 1.0,
              "max_value": 3.0,
              "avg_value": 2.0
            }
          ]
        },
        "data_type": {
          "source": "Trigger",
          "trigger_info": {
            "trigger_timestamp": 1704067200,
            "is_complete": true,
            "sequence_in_burst": 1
          }
        }
      }
    ],
    "is_complete": true,
    "total_samples": 1500,
    "created_at": 1704067205000,
    "quality_summary": {
      "overall_quality": {
        "status": "Good"
      },
      "channel_stats": [
        {
          "channel_id": 0,
          "sample_count": 1500,
          "min_value": 0.5,
          "max_value": 2.8,
          "avg_value": 1.65,
          "rms_value": 1.8
        }
      ],
      "value_range": [0.5, 2.8],
      "anomaly_count": 0
    }
  },
  "error": null,
  "timestamp": 1704067200000
}
```

### 12. 保存触发批次

**接口**: `POST /api/trigger/save/{burst_id}`

**描述**: 将指定的触发批次数据保存到文件系统。

**路径参数**:
- `burst_id`: 批次ID

**请求体**:
```json
{
  "dir": "experiments/vibration_test",
  "filename": "impact_measurement_001",
  "format": "csv",
  "description": "50Hz振动冲击测试数据"
}
```

**参数说明**:
- `dir`（可选）: 相对于data目录的子目录路径
- `filename`（可选）: 自定义文件名（不含扩展名）
- `format`（必需）: 导出格式（"json"/"csv"/"binary"）
- `description`（可选）: 文件描述信息

**响应示例**:
```json
{
  "success": true,
  "data": {
    "saved_path": "experiments/vibration_test/impact_measurement_001.csv",
    "format": "csv",
    "size_bytes": 125600,
    "burst_info": {
      "burst_id": "trigger_1704067200_1704067205000",
      "trigger_timestamp": 1704067200,
      "trigger_channel": 0,
      "total_samples": 1500,
      "duration_ms": 75.5,
      "created_at": 1704067205000,
      "quality": "Good",
      "can_save": true
    }
  },
  "error": null,
  "timestamp": 1704067200000
}
```

**错误响应示例**:
```json
{
  "success": false,
  "data": null,
  "error": "Invalid format. Supported: [\"json\", \"csv\", \"binary\"]",
  "timestamp": 1704067200000
}
```

### 13. 删除触发批次

**接口**: `DELETE /api/trigger/delete/{burst_id}`

**描述**: 从缓存中删除指定的触发批次。

**路径参数**:
- `burst_id`: 批次ID

**响应示例**:
```json
{
  "success": true,
  "data": "Trigger burst deleted",
  "error": null,
  "timestamp": 1704067200000
}
```

## 文件管理API

### 14. 列出文件

**接口**: `GET /api/files`

**描述**: 列出数据目录中的文件。

**查询参数**:
- `dir`（可选）: 子目录路径

**示例**: 
- `GET /api/files` - 列出根目录文件
- `GET /api/files?dir=experiments` - 列出experiments子目录文件

**响应示例**:
```json
{
  "success": true,
  "data": [
    {
      "filename": "wave_20240101_120000.bin",
      "size_bytes": 204800,
      "created_at": 1704067200000,
      "file_type": "binary"
    },
    {
      "filename": "experiments/test_data.csv",
      "size_bytes": 51200,
      "created_at": 1704067100000,
      "file_type": "raw_frames"
    }
  ],
  "error": null,
  "timestamp": 1704067200000
}
```

**字段说明**:
- `filename`: 相对路径文件名
- `size_bytes`: 文件大小（字节）
- `created_at`: 创建时间戳
- `file_type`: 文件类型（"binary"/"json"/"raw_frames"/"unknown"）

### 15. 下载文件

**接口**: `GET /api/files/{filename}`

**描述**: 下载指定文件。支持子目录路径。

**路径参数**:
- `filename`: 文件路径（支持子目录，如 "experiments/data.bin"）

**重要说明**:
- **URL编码**: 当文件名包含特殊字符（如斜杠、空格、中文等）时，必须进行URL编码
- **子目录路径**: 支持子目录格式，如 `subfolder/file.bin` 需编码为 `subfolder%2Ffile.bin`

**请求示例**:
```bash
# 根目录文件
GET /api/files/data.bin

# 子目录文件（需要URL编码）
GET /api/files/experiments%2Ftest_data.csv
# 对应原始路径: experiments/test_data.csv
```

**JavaScript调用示例**:
```javascript
// 正确的编码方式
const filename = "test_output/data.csv";
const url = `/api/files/${encodeURIComponent(filename)}`;
fetch(url).then(response => response.blob());
```

**Python调用示例**:
```python
import urllib.parse

filename = "test_output/data.csv"
encoded_filename = urllib.parse.quote(filename, safe='')
url = f"/api/files/{encoded_filename}"
```

**响应**: 
- **成功**: 返回二进制文件内容，包含适当的Content-Type和Content-Disposition头
- **失败**: 返回404状态码

**响应头示例**:
```
Content-Type: application/octet-stream
Content-Disposition: attachment; filename="data.bin"
```

### 16. 保存数据文件

**接口**: `POST /api/files/save`

**描述**: 保存base64编码的数据到文件系统。

**请求体**:
```json
{
  "dir": "measurements/2024-01-01",
  "filename": "test_data.bin",
  "base64": "AAABAAACAAADAAAEAAAF..."
}
```

**参数说明**:
- `dir`（可选）: 相对子目录路径
- `filename`（可选）: 文件名（不提供则自动生成）
- `base64`（必需）: base64编码的文件内容

**响应示例**:
```json
{
  "success": true,
  "data": "measurements/2024-01-01/test_data.bin",
  "error": null,
  "timestamp": 1704067200000
}
```

## 系统信息API

### 17. 健康检查

**接口**: `GET /health`

**描述**: 检查系统健康状态。

**请求**: 无需参数

**响应示例**:
```json
{
  "success": true,
  "data": {
    "status": "healthy",
    "service": "data-processor",
    "version": "2.0",
    "trigger_support": true,
    "timestamp": "2024-01-01T12:00:00Z"
  },
  "error": null,
  "timestamp": 1704067200000
}
```

### 18. API信息

**接口**: `GET /`

**描述**: 获取API的基本信息和可用端点列表。

**响应示例**:
```json
{
  "name": "Integrated Data Processor API",
  "version": "2.0",
  "description": "High-performance data acquisition and processing system with enhanced trigger support",
  "features": {
    "continuous_mode": true,
    "trigger_mode": true,
    "websocket_streaming": true,
    "file_management": true,
    "real_time_processing": true,
    "trigger_data_management": true,
    "custom_file_saving": true
  },
  "endpoints": {
    "health": "/health",
    "status": "/api/control/status",
    "start": "/api/control/start",
    "stop": "/api/control/stop",
    "ping": "/api/control/ping",
    "device_info": "/api/control/device_info",
    "modes": {
      "continuous": "/api/control/continuous_mode",
      "trigger": "/api/control/trigger_mode"
    },
    "trigger": {
      "request_data": "/api/control/request_trigger_data",
      "list_bursts": "/api/trigger/list",
      "preview_burst": "/api/trigger/preview/{burst_id}",
      "save_burst": "/api/trigger/save/{burst_id}",
      "delete_burst": "/api/trigger/delete/{burst_id}"
    },
    "configuration": "/api/control/configure",
    "files": {
      "list": "/api/files?dir=<optional>",
      "download": "/api/files/{filename}",
      "save": "/api/files/save"
    },
    "websocket": "ws://<host>:<port>"
  },
  "documentation": "https://github.com/your-repo/data-processor"
}
```

## WebSocket实时接口

### 连接地址

`ws://127.0.0.1:8081`

### 连接流程

1. 建立WebSocket连接
2. 接收欢迎消息
3. 发送订阅消息（可选）
4. 接收实时数据

### 订阅控制

**发送订阅消息**:
```json
{
  "type": "subscribe",
  "channels": ["all"]
}
```

**可用频道**:
- `"all"`: 订阅所有事件
- `"data"`: 订阅数据流
- `"trigger_events"`: 订阅触发事件通知
- `"trigger_bursts"`: 订阅触发批次完成通知
- `"continuous_only"`: 仅订阅连续模式数据
- `"trigger_only"`: 仅订阅触发模式数据

**订阅确认响应**:
```json
{
  "type": "subscription_updated",
  "client_id": "uuid-client-id",
  "subscriptions": {
    "data_stream": true,
    "trigger_events": true,
    "trigger_bursts": true,
    "continuous_only": false,
    "trigger_only": false
  },
  "timestamp": 1704067200000
}
```

### WebSocket消息类型

#### 欢迎消息
```json
{
  "type": "welcome",
  "client_id": "uuid-client-id",
  "timestamp": 1704067200000,
  "server_capabilities": {
    "data_streaming": true,
    "trigger_events": true,
    "trigger_burst_complete": true,
    "subscription_control": true
  }
}
```

#### 实时数据流
```json
{
  "type": "data",
  "timestamp": 1704067200000,
  "sequence": 12345,
  "channel_count": 2,
  "sample_rate": 10000.0,
  "data": [1.23, 1.24, 1.25, 1.26, 1.27],
  "metadata": {
    "packet_count": 12345,
    "processing_time_us": 500,
    "data_quality": {
      "status": "Good"
    },
    "channel_info": [
      {
        "channel_id": 0,
        "sample_count": 100,
        "min_value": 1.0,
        "max_value": 3.0,
        "avg_value": 2.0
      }
    ]
  },
  "data_type": {
    "source": "Continuous",
    "trigger_info": null
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

#### 触发批次完成通知
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
  "created_at": 1704067205000,
  "preview_samples": [1.23, 1.24, 1.25, 1.26, 1.27],
  "channel_stats": [
    {
      "channel_id": 0,
      "sample_count": 1500,
      "min_value": 0.5,
      "max_value": 2.8,
      "avg_value": 1.65,
      "rms_value": 1.8
    }
  ],
  "voltage_range": [0.1, 3.2],
  "event_time": 1704067210000
}
```

#### Ping/Pong心跳

**发送Ping**:
```json
{
  "type": "ping"
}
```

**接收Pong**:
```json
{
  "type": "pong",
  "timestamp": 1704067200000
}
```

## 错误处理

### HTTP状态码

- `200 OK`: 请求成功
- `400 Bad Request`: 请求参数错误
- `404 Not Found`: 资源不存在
- `500 Internal Server Error`: 服务器内部错误

### 错误响应格式

```json
{
  "success": false,
  "data": null,
  "error": "具体错误信息",
  "timestamp": 1704067200000
}
```

### 常见错误

1. **设备未连接**: "Device not connected"
2. **无效格式**: "Invalid format. Supported: [\"json\", \"csv\", \"binary\"]"
3. **批次不存在**: "Trigger burst not found"
4. **模式错误**: "Device not in trigger mode"
5. **参数错误**: "Invalid parameter"

## 开发建议

### 前端开发最佳实践

1. **状态轮询**: 定期调用`/api/control/status`获取系统状态
2. **WebSocket重连**: 实现自动重连机制处理网络中断
3. **错误处理**: 统一处理API错误响应
4. **数据可视化**: 使用WebSocket数据流进行实时图表绘制
5. **批次管理**: 实现触发批次的列表、预览、保存界面
6. **文件管理**: 提供文件上传下载功能

### 性能优化

1. **数据缓冲**: WebSocket数据量大时使用缓冲机制
2. **选择性订阅**: 根据需要订阅特定数据类型
3. **文件分页**: 文件列表支持分页显示
4. **内存管理**: 及时清理不需要的数据和DOM元素

### 安全考虑

1. **输入验证**: 验证所有用户输入
2. **文件路径**: 防止路径遍历攻击
3. **数据大小**: 限制上传文件大小
4. **连接限制**: 控制WebSocket连接数量

## 示例代码

### JavaScript WebSocket客户端

```javascript
const ws = new WebSocket('ws://127.0.0.1:8081');

ws.onopen = function() {
    console.log('WebSocket连接已建立');
    
    // 订阅所有事件
    ws.send(JSON.stringify({
        type: 'subscribe',
        channels: ['all']
    }));
};

ws.onmessage = function(event) {
    const message = JSON.parse(event.data);
    
    switch(message.type) {
        case 'welcome':
            console.log('收到欢迎消息:', message.client_id);
            break;
            
        case 'data':
            // 处理实时数据
            updateChart(message.data);
            break;
            
        case 'trigger_event':
            console.log('触发事件:', message);
            break;
            
        case 'trigger_burst_complete':
            console.log('触发批次完成:', message.burst_id);
            refreshTriggerList();
            break;
    }
};
```

### JavaScript API调用示例

```javascript
// 获取系统状态
async function getSystemStatus() {
    const response = await fetch('/api/control/status');
    const result = await response.json();
    return result;
}

// 启动数据采集
async function startCollection() {
    const response = await fetch('/api/control/start', {
        method: 'POST'
    });
    const result = await response.json();
    return result;
}

// 保存触发批次
async function saveTriggerBurst(burstId, options) {
    const response = await fetch(`/api/trigger/save/${burstId}`, {
        method: 'POST',
        headers: {
            'Content-Type': 'application/json'
        },
        body: JSON.stringify(options)
    });
    const result = await response.json();
    return result;
}
```

---

**文档版本**: v2.0  
**更新日期**: 2024-01-01  
**适用系统**: 数据采集与处理系统 v2.0+