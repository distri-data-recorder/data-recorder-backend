#include "shared_memory.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

bool initSharedMemory(SharedMemManager* manager) {
    if (!manager) {
        return false;
    }

    // 初始化管理器
    manager->hMapFile = NULL;
    manager->pSharedMem = NULL;
    manager->initialized = false;

    // 创建共享内存映射
    DWORD sharedMemSize = sizeof(SharedMemory);
    manager->hMapFile = CreateFileMappingA(
        INVALID_HANDLE_VALUE,    // 使用页面文件
        NULL,                    // 默认安全属性
        PAGE_READWRITE,          // 读写访问
        0,                       // 高位大小
        sharedMemSize,           // 低位大小
        SHARED_MEM_NAME          // 共享内存名称
    );

    if (manager->hMapFile == NULL) {
        printf("[ERROR] CreateFileMapping failed: %lu\n", GetLastError());
        return false;
    }

    // 检查是否是新创建的共享内存
    bool isNewMapping = (GetLastError() != ERROR_ALREADY_EXISTS);

    // 映射共享内存到进程地址空间
    manager->pSharedMem = (SharedMemory*)MapViewOfFile(
        manager->hMapFile,       // 映射对象句柄
        FILE_MAP_ALL_ACCESS,     // 读写访问
        0,                       // 高位偏移
        0,                       // 低位偏移
        sharedMemSize            // 映射大小
    );

    if (manager->pSharedMem == NULL) {
        printf("[ERROR] MapViewOfFile failed: %lu\n", GetLastError());
        CloseHandle(manager->hMapFile);
        manager->hMapFile = NULL;
        return false;
    }

    // 如果是新创建的共享内存，初始化头部
    if (isNewMapping) {
        printf("[INFO] Initializing new shared memory\n");

        // 初始化头部
        manager->pSharedMem->header.magic = SHARED_MEM_MAGIC;
        manager->pSharedMem->header.version = SHARED_MEM_VERSION;
        manager->pSharedMem->header.write_index = 0;
        manager->pSharedMem->header.read_index = 0;
        manager->pSharedMem->header.buffer_size = SHARED_MEM_BUFFER_SIZE;
        manager->pSharedMem->header.packet_count = 0;
        manager->pSharedMem->header.status = 1; // 活跃状态
        memset(manager->pSharedMem->header.reserved, 0, sizeof(manager->pSharedMem->header.reserved));

        // 清零数据包缓冲区
        memset(manager->pSharedMem->packets, 0, sizeof(manager->pSharedMem->packets));
    } else {
        printf("[INFO] Connected to existing shared memory\n");

        // 验证现有共享内存的有效性
        if (manager->pSharedMem->header.magic != SHARED_MEM_MAGIC) {
            printf("[ERROR] Invalid shared memory magic: 0x%08X\n", manager->pSharedMem->header.magic);
            cleanupSharedMemory(manager);
            return false;
        }

        if (manager->pSharedMem->header.version != SHARED_MEM_VERSION) {
            printf("[ERROR] Unsupported shared memory version: %u\n", manager->pSharedMem->header.version);
            cleanupSharedMemory(manager);
            return false;
        }
    }

    manager->initialized = true;
    printf("[INFO] Shared memory initialized successfully (size: %lu bytes)\n", sharedMemSize);

    return true;
}

void cleanupSharedMemory(SharedMemManager* manager) {
    if (!manager) {
        return;
    }

    if (manager->pSharedMem) {
        UnmapViewOfFile(manager->pSharedMem);
        manager->pSharedMem = NULL;
    }

    if (manager->hMapFile) {
        CloseHandle(manager->hMapFile);
        manager->hMapFile = NULL;
    }

    manager->initialized = false;
    printf("[INFO] Shared memory cleaned up\n");
}

bool writeADCPacket(SharedMemManager* manager, uint8_t seq, const uint8_t* payload, uint16_t payloadLen) {
    if (!manager || !manager->initialized || !manager->pSharedMem || !payload) {
        return false;
    }

    if (payloadLen > sizeof(((ADCDataPacket*)0)->payload)) {
        printf("[ERROR] Payload too large: %u bytes (max: %zu)\n", payloadLen, sizeof(((ADCDataPacket*)0)->payload));
        return false;
    }

    // 获取当前时间戳
    DWORD timestamp = GetTickCount();

    // 获取当前写入位置
    uint32_t writeIndex = manager->pSharedMem->header.write_index;
    uint32_t packetIndex = writeIndex % SHARED_MEM_BUFFER_SIZE;

    // 填充数据包
    ADCDataPacket* packet = &manager->pSharedMem->packets[packetIndex];
    packet->timestamp_ms = timestamp;
    packet->sequence = seq;
    packet->payload_len = payloadLen;
    memcpy(packet->payload, payload, payloadLen);

    // 原子性更新写入索引和包计数
    InterlockedIncrement((LONG*)&manager->pSharedMem->header.write_index);
    InterlockedIncrement((LONG*)&manager->pSharedMem->header.packet_count);

    return true;
}

bool getSharedMemStatus(SharedMemManager* manager, uint32_t* writeIndex, uint32_t* readIndex, uint32_t* packetCount) {
    if (!manager || !manager->initialized || !manager->pSharedMem) {
        return false;
    }

    if (writeIndex) {
        *writeIndex = manager->pSharedMem->header.write_index;
    }

    if (readIndex) {
        *readIndex = manager->pSharedMem->header.read_index;
    }

    if (packetCount) {
        *packetCount = manager->pSharedMem->header.packet_count;
    }

    return true;
}