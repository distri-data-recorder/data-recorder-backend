# Device Simulator v2.1 - 通用协议 V6 实现

一个跨平台的设备模拟器，支持PC仿真和MCU固件部署，提供无缝的代码迁移能力。

## 核心特性

### 双环境支持
- **仿真模式**: 基于PC的TCP Socket测试
- **MCU模式**: 真实硬件通过USB CDC部署
- **无缝迁移**: 相同代码库适用于两种环境

### 完整的协议V6实现
- 系统控制（PING/PONG、设备信息、状态查询）
- 采集控制（连续/触发模式、流管理）
- 数据传输（数据包、事件、缓冲数据）
- 错误处理和日志记录

### 高级功能
- **智能触发仿真**: 随机时间间隔配置数据突发
- **灵活数据源**: CSV文件、内置信号生成、真实传感器
- **可靠通信**: CRC16校验、帧解析、错误恢复
- **内存安全**: 适当的缓冲区管理和资源清理

## 项目结构

```
device-simulator-v2.1/
├── device_simulator.h        # 主头文件与平台抽象
├── device_simulator.c        # 核心设备逻辑实现
├── platform_abstraction.c   # 平台特定实现
├── main.c                   # 程序入口点
├── config.h                 # 配置和功能标志
├── mcu_hal.h               # MCU硬件抽象层模板
├── protocol/               # 协议V6实现
│   ├── protocol.h          # 帧构建/解析
│   ├── protocol.c
│   ├── io_buffer.h         # 流缓冲区管理
│   └── io_buffer.c
├── Makefile                # 高级构建系统
├── sample_data.csv         # 测试数据文件
└── README_CN.md           # 本文档
```

## 快速开始

### 仿真模式（PC测试）

```bash
# 仿真模式构建（默认）
make

# 使用默认设置运行（端口9001）
make run

# 构建优化版本
make BUILD=release

# 构建并以socket模式运行
make BUILD=release run
```

### MCU模式（硬件部署）

```bash
# 为MCU目标构建
make MODE=mcu

# 构建并烧录到硬件
make MODE=mcu flash

# 使用特定MCU设置构建
make MODE=mcu BUILD=release
```

## 构建系统

### 构建配置
- `BUILD=debug` - 调试符号，无优化
- `BUILD=release` - 生产优化版本
- `BUILD=profile` - 启用性能分析

### 目标模式
- `MODE=simulation` - PC仿真（默认）
- `MODE=mcu` - MCU固件

### 平台支持
- **Windows**: MinGW, MSYS2, MSVC
- **Linux**: GCC, Clang
- **macOS**: GCC, Clang  
- **MCU**: ARM GCC工具链

## 配置

### 仿真模式设置
```c
#define DEFAULT_TCP_PORT "9001"
#define CSV_FILE_DEFAULT "sample_data.csv"
#define TRIGGER_MIN_INTERVAL 10    // 秒
#define TRIGGER_MAX_INTERVAL 15    // 秒
```

### MCU模式设置
```c
#define USB_CDC_BUFFER_SIZE 1024
#define ADC_RESOLUTION 4096        // 12位ADC
#define MAX_SAMPLE_RATE_HZ 100000
```

### 功能标志
```c
#define FEATURE_TRIGGER_SIMULATION 1
#define FEATURE_CSV_DATA_LOADING 1
#define FEATURE_SIGNAL_GENERATION 1
#define FEATURE_LOG_MESSAGES 1
```

## 协议V6命令

### 系统控制
| 命令 | ID | 描述 |
|---------|----|-----------| 
| PING | 0x01 | 设备发现 |
| PONG | 0x81 | 设备响应 |
| GET_DEVICE_INFO | 0x03 | 请求设备能力 |
| GET_STATUS | 0x02 | 请求当前状态 |

### 采集控制
| 命令 | ID | 描述 |
|---------|----|-----------| 
| SET_MODE_CONTINUOUS | 0x10 | 启用连续流 |
| SET_MODE_TRIGGER | 0x11 | 启用触发模式 |
| START_STREAM | 0x12 | 开始数据采集 |
| STOP_STREAM | 0x13 | 停止数据采集 |
| CONFIGURE_STREAM | 0x14 | 设置通道参数 |

### 数据传输
| 命令 | ID | 描述 |
|---------|----|-----------| 
| DATA_PACKET | 0x40 | ADC数据载荷 |
| EVENT_TRIGGERED | 0x41 | 触发事件通知 |
| REQUEST_BUFFERED_DATA | 0x42 | 请求触发数据 |
| BUFFER_TRANSFER_COMPLETE | 0x4F | 传输完成信号 |

## MCU移植指南

### 需要实现的核心函数

#### 1. 硬件抽象层 (`mcu_hal.c`)

```c
// 系统初始化
hal_status_t mcu_hal_init(void) {
    // 初始化时钟系统
    SystemClock_Config();
    
    // 初始化GPIO
    MX_GPIO_Init();
    
    // 初始化定时器
    MX_TIM_Init();
    
    return HAL_OK;
}

// 调试输出（根据你的MCU选择UART/RTT/SWO等）
void debug_printf(const char* format, ...) {
    char buffer[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    
    // 方式1: UART输出
    HAL_UART_Transmit(&huart1, (uint8_t*)buffer, strlen(buffer), 1000);
    
    // 方式2: RTT输出（推荐）
    // SEGGER_RTT_printf(0, "%s", buffer);
    
    // 方式3: SWO输出
    // ITM_SendString(buffer);
}

// 系统时钟
uint32_t hal_get_tick(void) {
    return HAL_GetTick();  // STM32 HAL
    // return xTaskGetTickCount(); // FreeRTOS
}
```

#### 2. USB CDC接口 (`platform_abstraction.c`)

```c
// USB CDC初始化
usb_status_t usb_cdc_init(void) {
    // 初始化USB外设
    MX_USB_DEVICE_Init();
    
    // 等待USB枚举完成
    while (!hUsbDeviceFS.dev_state == USBD_STATE_CONFIGURED) {
        HAL_Delay(10);
    }
    
    return USB_OK;
}

// USB发送数据
usb_status_t usb_cdc_send(usb_handle_t handle, const uint8_t* data, uint32_t length) {
    uint8_t result = CDC_Transmit_FS((uint8_t*)data, length);
    return (result == USBD_OK) ? USB_OK : USB_ERROR;
}

// USB接收数据（需要在CDC接收回调中实现）
usb_status_t usb_cdc_receive(usb_handle_t handle, uint8_t* buffer, 
                             uint32_t buffer_size, uint32_t* received, 
                             uint32_t timeout_ms) {
    // 实现非阻塞接收
    // 可以使用环形缓冲区存储接收的数据
    *received = get_available_data(buffer, buffer_size);
    return (*received > 0) ? USB_OK : USB_BUSY;
}
```

#### 3. ADC数据采集

```c
// ADC初始化
hal_status_t adc_init(void) {
    MX_ADC1_Init();
    
    // 配置DMA用于连续采集
    if (HAL_ADC_Start_DMA(&hadc1, (uint32_t*)adc_buffer, ADC_BUFFER_SIZE) != HAL_OK) {
        return HAL_ERROR;
    }
    
    return HAL_OK;
}

// 读取单个通道
hal_status_t adc_read_channel(void* handle, uint8_t channel, uint16_t* value) {
    // 设置ADC通道
    ADC_ChannelConfTypeDef sConfig = {0};
    sConfig.Channel = get_adc_channel(channel);
    sConfig.Rank = 1;
    sConfig.SamplingTime = ADC_SAMPLETIME_3CYCLES;
    
    if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK) {
        return HAL_ERROR;
    }
    
    // 开始转换并等待完成
    HAL_ADC_Start(&hadc1);
    if (HAL_ADC_PollForConversion(&hadc1, 100) == HAL_OK) {
        *value = HAL_ADC_GetValue(&hadc1);
        return HAL_OK;
    }
    
    return HAL_ERROR;
}
```

### 需要修改的配置宏

#### 1. 平台检测宏 (`config.h`)
```c
// 取消仿真模式定义
#undef SIMULATION_MODE

// MCU平台定义
#define MCU_PLATFORM
#define MCU_STM32F4        // 根据你的MCU型号调整
```

#### 2. 硬件资源配置
```c
// MCU特定配置
#define MAX_CHANNELS_SUPPORTED      4       // 根据ADC通道数调整
#define ADC_RESOLUTION              4096    // 12位ADC = 4096
#define ADC_VREF_VOLTAGE            3300    // 参考电压(mV)
#define DEFAULT_SAMPLE_RATE_HZ      10000   // 根据性能调整

// 内存配置（根据MCU RAM大小）
#define RX_BUFFER_SIZE              8192    // 8KB
#define TX_BUFFER_SIZE              4096    // 4KB
#define TRIGGER_BUFFER_SIZE         2048    // 2KB

// USB配置
#define USB_CDC_BUFFER_SIZE         512     // USB包大小
#define USB_CDC_TX_TIMEOUT_MS       1000
```

#### 3. 功能特性控制
```c
// 禁用仿真专用功能
#define FEATURE_CSV_DATA_LOADING    0
#define FEATURE_FILE_LOGGING        0
#define FEATURE_CONSOLE_CONTROL     0

// 启用MCU专用功能
#define FEATURE_LOW_POWER_MODE      1
#define FEATURE_HARDWARE_WATCHDOG   1
#define FEATURE_FLASH_STORAGE       1
```

### 移植步骤

#### 第一步：硬件配置
1. 使用STM32CubeMX配置时钟、GPIO、ADC、USB
2. 生成HAL驱动代码
3. 确保USB CDC类正确配置

#### 第二步：代码集成
1. 将device-simulator源码复制到MCU项目
2. 修改`config.h`中的平台相关宏
3. 实现`mcu_hal.c`中的硬件抽象函数

#### 第三步：编译配置
1. 修改Makefile或IDE工程文件
2. 添加必要的include路径
3. 链接HAL库和USB中间件

#### 第四步：调试验证
1. 使用调试器或RTT查看运行状态
2. 验证USB CDC通信
3. 测试ADC采集功能

### 常见移植问题

**1. 内存不足**
- 减少缓冲区大小
- 使用静态内存分配
- 优化数据结构

**2. USB通信异常**
- 检查USB时钟配置
- 验证CDC描述符
- 确保USB中断正确处理

**3. ADC采集问题**
- 验证ADC时钟和采样时间
- 检查DMA配置
- 确保参考电压稳定

**4. 实时性要求**
- 使用DMA减少CPU负载
- 优化中断处理时间
- 考虑使用RTOS任务调度

### 性能指标

#### 仿真模式
- **吞吐量**: 高达1MB/s
- **延迟**: < 50ms端到端
- **CPU使用率**: < 5%典型值
- **内存**: ~6MB包含缓冲区

#### MCU模式
- **采样率**: 每通道高达100kHz
- **通道数**: 最多8个同时采集
- **延迟**: < 20ms
- **功耗**: 支持低功耗模式

## 测试与集成

### 与Data Reader集成

系统设计为与data-reader组件无缝协作：

1. **启动device simulator**:
```bash
./device-simulator
```

2. **启动data-processor**（socket模式）:
```bash
../data-processor/start_app.bat
```
3. **测试完整管道**:
- 模拟器生成数据
- Data-processor处理分析
- 前端显示结果

### 调试方法

#### 启用调试输出
```c
#define DEBUG 1
#define DEBUG_PRINT_FRAMES 1
#define DEBUG_PRINT_COMMANDS 1
```

#### 常见问题排查

**连接失败**：
- 检查端口可用性
- 验证防火墙设置
- 确保正确的主机/端口配置

**帧解析错误**：
- 验证CRC16实现
- 检查帧对齐
- 验证缓冲区大小

**内存问题**：
- 监控缓冲区使用
- 检查内存泄漏
- 验证分配限制

## 开发工具

### 构建工具
```bash
# 格式化代码
make format

# 生成IDE数据库  
make compile_commands.json

# 构建所有配置
make all-configs

# 打包发布
make package
```

### 测试工具
```bash
# 构建并基础测试
make test

# 内存泄漏检查（Linux/macOS）
make memcheck

# 性能分析
make BUILD=profile run
```

## 自定义扩展

### 添加新通道
```c
// 在device_init()中
g_device_state.channels[2].channel_id = 2;
g_device_state.channels[2].max_sample_rate_hz = 50000;
g_device_state.channels[2].supported_formats_mask = FORMAT_INT16 | FORMAT_FLOAT32;
strcpy(g_device_state.channels[2].name, "Temperature");
g_device_state.num_channels = 3;
```

### 自定义数据源
```c
// 在data_source_get_sample()中
case 2: // 温度通道
    return read_temperature_sensor();
case 3: // 压力通道  
    return read_pressure_sensor();
```

### 触发逻辑定制
```c
// 在device_handle_trigger_simulation()中
// 自定义触发条件
if (sample_value > custom_threshold && 
    derivative > min_slope &&
    noise_level < max_noise) {
    trigger_detected = true;
}
```

## 许可证

MIT许可证 - 详见LICENSE文件。

## 技术支持

### 文档资源
- 协议V6规范（protocol_doc.md）
- API文档（使用Doxygen生成）
- 硬件集成指南

### 社区支持
- GitHub Issues用于问题报告
- Discussions用于功能请求
- Wiki提供额外示例

---

**Device Simulator v2.1** - 连接仿真与现实的协议V6桥梁