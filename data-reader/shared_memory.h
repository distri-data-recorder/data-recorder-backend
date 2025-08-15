#ifndef SHARED_MEMORY_H
#define SHARED_MEMORY_H

#include <stdint.h>
#include <stdbool.h>
#include <windows.h>

// 共享内存配置
#define SHARED_MEM_NAME "ADC_DATA_SHARED_MEM"
#define SHARED_MEM_MAGIC 0xADC12345
#define SHARED_MEM_VERSION 1
#define SHARED_MEM_BUFFER_SIZE 1024

// 共享内存头部结构
typedef struct {
    uint32_t magic;           // 魔数标识: 0xADC12345
    uint32_t version;         // 版本号: 1
    volatile uint32_t write_index;     // 写入索引（循环缓冲区）
    volatile uint32_t read_index;      // 读取索引（循环缓冲区）
    uint32_t buffer_size;     // 缓冲区大小: 1024
    volatile uint32_t packet_count;    // 总包计数
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
    ADCDataPacket   packets[SHARED_MEM_BUFFER_SIZE];  // 循环缓冲区
} SharedMemory;

// 共享内存管理结构
typedef struct {
    HANDLE hMapFile;
    SharedMemory* pSharedMem;
    bool initialized;
} SharedMemManager;

// 函数声明
bool initSharedMemory(SharedMemManager* manager);
void cleanupSharedMemory(SharedMemManager* manager);
bool writeADCPacket(SharedMemManager* manager, uint8_t seq, const uint8_t* payload, uint16_t payloadLen);
bool getSharedMemStatus(SharedMemManager* manager, uint32_t* writeIndex, uint32_t* readIndex, uint32_t* packetCount);

#endif // SHARED_MEMORY_H