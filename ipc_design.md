# 进程间通信架构设计

## 概述

本文档定义数据采集进程（data-reader）和数据处理进程（data-processor）之间的通信协议和数据结构。

## 通信方式

### 1. 共享内存（主要数据通道）
- **用途**: 传输ADC数据包的payload
- **方向**: 数据采集进程  数据处理进程
- **特点**: 高性能，低延迟

### 2. 消息队列（控制信号）
- **用途**: 传输控制信号和状态信息
- **方向**: 双向通信
- **特点**: 可靠性高，支持异步通信

## 共享内存设计

### 数据结构

`c
// 共享内存头部信息
typedef struct {
    uint32_t magic;           // 魔数标识: 0xADC12345
    uint32_t version;         // 版本号: 1
    uint32_t write_index;     // 写入索引（循环缓冲区）
    uint32_t read_index;      // 读取索引（循环缓冲区）
    uint32_t buffer_size;     // 缓冲区大小
    uint32_t packet_count;    // 总包计数
    uint8_t  status;          // 状态标志
    uint8_t  reserved[7];     // 保留字段
} SharedMemHeader;

// ADC数据包结构
typedef struct {
    uint32_t timestamp_ms;    // 时间戳（毫秒）
    uint16_t sequence;        // 序列号
    uint16_t payload_len;     // payload长度
    uint8_t  payload[4096];   // ADC数据payload（最大4KB）
} ADCDataPacket;

// 完整共享内存布局
typedef struct {
    SharedMemHeader header;
    ADCDataPacket   packets[1024];  // 循环缓冲区，1024个包
} SharedMemory;
`

### 内存布局
- **总大小**: 约4.2MB
- **头部**: 32字节
- **数据区**: 1024个ADC数据包，每个约4KB
- **访问方式**: 循环缓冲区

### 同步机制
- 使用原子操作更新write_index和read_index
- 生产者（数据采集进程）更新write_index
- 消费者（数据处理进程）更新read_index
- 当write_index追上read_index时，覆盖最旧的数据

## 消息队列设计

### 消息类型

```c
typedef enum {
    MSG_START_COLLECTION = 1,    // 开始采集
    MSG_STOP_COLLECTION = 2,     // 停止采集
    MSG_STATUS_REQUEST = 3,      // 状态查询
    MSG_STATUS_RESPONSE = 4,     // 状态响应
    MSG_SAVE_WAVEFORM = 5,       // 保存波形文件
    MSG_ERROR = 99               // 错误消息
} MessageType;

typedef struct {
    MessageType type;
    uint32_t    timestamp;
    uint32_t    data_len;
    uint8_t     data[256];       // 消息数据
} IPCMessage;
```

### 队列配置
- **队列名称**:
  - `/data_reader_to_processor` (数据采集→数据处理)
  - `/data_processor_to_reader` (数据处理→数据采集)
- **消息大小**: 最大272字节
- **队列深度**: 64条消息

## 文件系统接口

### 波形文件保存
- **触发**: 前端通过HTTPS发送保存请求
- **流程**:
  1. 数据处理进程接收HTTPS请求
  2. 通过消息队列通知数据采集进程
  3. 数据采集进程保存当前缓冲区到文件
  4. 返回文件路径给数据处理进程
  5. 数据处理进程响应前端

### 文件格式
- **原始帧文件**: 继续使用现有的`raw_frames_*.txt`格式
- **处理后数据**: JSON格式，包含时间戳和处理后的数值

## 错误处理

### 共享内存
- 检查魔数和版本号
- 处理缓冲区溢出
- 检测进程异常退出

### 消息队列
- 超时处理
- 消息丢失检测
- 队列满处理

## 性能考虑

### 共享内存优化
- 使用内存映射文件
- 避免频繁的内存拷贝
- 批量处理数据包

### 消息队列优化
- 异步处理控制消息
- 避免阻塞主数据流

## 平台兼容性

### Windows实现
- 共享内存: CreateFileMapping/MapViewOfFile
- 消息队列: 使用命名管道或Windows消息队列
- 同步: InterlockedIncrement/InterlockedCompareExchange

### 跨平台考虑
- 预留POSIX实现接口
- 使用条件编译处理平台差异
