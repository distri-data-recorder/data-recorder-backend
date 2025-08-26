// ============= ipc_communication.c =============
#include "ipc_communication.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>

// 生成唯一消息ID
static void generateMessageId(char* buffer, size_t bufferSize) {
    snprintf(buffer, bufferSize, "msg_%lu_%d", GetTickCount(), rand() % 10000);
}

// 生成ISO 8601时间戳
static void generateTimestamp(char* buffer, size_t bufferSize) {
    time_t now = time(NULL);
    struct tm* utc = gmtime(&now);
    strftime(buffer, bufferSize, "%Y-%m-%dT%H:%M:%SZ", utc);
}

// 简单的JSON解析 - 提取字段值
static bool extractJsonField(const char* json, const char* field, char* value, size_t valueSize) {
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\":", field);
    
    const char* start = strstr(json, pattern);
    if (!start) return false;
    
    start += strlen(pattern);
    while (*start && (*start == ' ' || *start == '\t')) start++; // 跳过空白
    
    if (*start == '"') {
        // 字符串值
        start++;
        const char* end = strchr(start, '"');
        if (!end) return false;
        
        size_t len = end - start;
        if (len >= valueSize) len = valueSize - 1;
        
        strncpy(value, start, len);
        value[len] = '\0';
        return true;
    } else {
        // 非字符串值（数字、对象等）
        const char* end = start;
        int braceCount = 0;
        bool inString = false;
        
        while (*end) {
            if (*end == '"' && (end == start || *(end-1) != '\\')) {
                inString = !inString;
            } else if (!inString) {
                if (*end == '{' || *end == '[') {
                    braceCount++;
                } else if (*end == '}' || *end == ']') {
                    braceCount--;
                    if (braceCount < 0) break;
                } else if (braceCount == 0 && (*end == ',' || *end == '}' || *end == '\n' || *end == '\r')) {
                    break;
                }
            }
            end++;
        }
        
        size_t len = end - start;
        if (len >= valueSize) len = valueSize - 1;
        
        strncpy(value, start, len);
        value[len] = '\0';
        
        // 移除尾部空白
        while (len > 0 && (value[len-1] == ' ' || value[len-1] == '\t')) {
            value[--len] = '\0';
        }
        
        return true;
    }
}

bool parseIPCMessage(const char* jsonLine, char* messageType, char* payload, char* messageId, char* timestamp) {
    if (!jsonLine || !messageType) return false;
    
    // 提取必需字段
    if (!extractJsonField(jsonLine, "type", messageType, 64)) return false;
    
    // 提取可选字段
    if (messageId) extractJsonField(jsonLine, "id", messageId, 64);
    if (timestamp) extractJsonField(jsonLine, "timestamp", timestamp, 32);
    if (payload) extractJsonField(jsonLine, "payload", payload, IPC_MAX_MESSAGE_SIZE);
    
    return true;
}

bool buildIPCMessage(const char* messageType, const char* payload, char* output, size_t outputSize) {
    if (!messageType || !output) return false;
    
    char messageId[64];
    char timestamp[32];
    
    generateMessageId(messageId, sizeof(messageId));
    generateTimestamp(timestamp, sizeof(timestamp));
    
    if (payload && strlen(payload) > 0) {
        snprintf(output, outputSize,
                 "{\"id\":\"%s\",\"timestamp\":\"%s\",\"type\":\"%s\",\"payload\":%s}\n",
                 messageId, timestamp, messageType, payload);
    } else {
        snprintf(output, outputSize,
                 "{\"id\":\"%s\",\"timestamp\":\"%s\",\"type\":\"%s\",\"payload\":{}}\n",
                 messageId, timestamp, messageType);
    }
    
    return true;
}

bool initIPC(IPCManager* ipc) {
    if (!ipc) return false;
    
    memset(ipc, 0, sizeof(IPCManager));
    
    // 创建命名管道
    ipc->hPipe = CreateNamedPipeA(
        IPC_PIPE_NAME,
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
        1,  // 最大实例数
        IPC_BUFFER_SIZE,  // 输出缓冲区大小
        IPC_BUFFER_SIZE,  // 输入缓冲区大小
        0,  // 默认超时
        NULL  // 默认安全属性
    );
    
    if (ipc->hPipe == INVALID_HANDLE_VALUE) {
        printf("[IPC] Failed to create named pipe: %lu\n", GetLastError());
        return false;
    }
    
    ipc->state = IPC_STATE_LISTENING;
    ipc->initialized = true;
    
    printf("[IPC] Named pipe created: %s\n", IPC_PIPE_NAME);
    return true;
}

void cleanupIPC(IPCManager* ipc) {
    if (!ipc || !ipc->initialized) return;
    
    if (ipc->hPipe != INVALID_HANDLE_VALUE) {
        if (ipc->state == IPC_STATE_CONNECTED) {
            DisconnectNamedPipe(ipc->hPipe);
        }
        CloseHandle(ipc->hPipe);
        ipc->hPipe = INVALID_HANDLE_VALUE;
    }
    
    ipc->state = IPC_STATE_DISCONNECTED;
    ipc->initialized = false;
    
    printf("[IPC] Cleaned up\n");
}

bool processIPCMessages(IPCManager* ipc, IPCMessageCallback callback, void* userData) {
    if (!ipc || !ipc->initialized) return false;
    
    // 检查连接状态
    if (ipc->state == IPC_STATE_LISTENING) {
        // 非阻塞检查是否有客户端连接
        BOOL connected = ConnectNamedPipe(ipc->hPipe, NULL);
        DWORD error = GetLastError();
        
        if (connected || error == ERROR_PIPE_CONNECTED) {
            ipc->state = IPC_STATE_CONNECTED;
            printf("[IPC] Client connected\n");
        } else if (error != ERROR_IO_PENDING && error != ERROR_NO_DATA) {
            printf("[IPC] ConnectNamedPipe failed: %lu\n", error);
            return false;
        }
        return true; // 还在等待连接
    }
    
    if (ipc->state != IPC_STATE_CONNECTED) {
        return false;
    }
    
    // 读取消息
    DWORD bytesRead = 0;
    BOOL success = ReadFile(
        ipc->hPipe,
        ipc->readBuffer + ipc->bytesInBuffer,
        IPC_BUFFER_SIZE - ipc->bytesInBuffer - 1,
        &bytesRead,
        NULL
    );
    
    if (!success) {
        DWORD error = GetLastError();
        if (error == ERROR_BROKEN_PIPE || error == ERROR_NO_DATA) {
            printf("[IPC] Client disconnected\n");
            DisconnectNamedPipe(ipc->hPipe);
            ipc->state = IPC_STATE_LISTENING;
        } else {
            printf("[IPC] ReadFile failed: %lu\n", error);
        }
        return false;
    }
    
    if (bytesRead == 0) {
        return true; // 没有数据
    }
    
    ipc->bytesInBuffer += bytesRead;
    ipc->readBuffer[ipc->bytesInBuffer] = '\0';
    
    // 处理完整的JSON行
    char* lineStart = ipc->readBuffer;
    char* lineEnd;
    
    while ((lineEnd = strchr(lineStart, '\n')) != NULL) {
        *lineEnd = '\0'; // 终止行
        
        // 移除回车符
        if (lineEnd > lineStart && *(lineEnd-1) == '\r') {
            *(lineEnd-1) = '\0';
        }
        
        // 处理这行消息
        if (strlen(lineStart) > 0 && callback) {
            char messageType[64] = {0};
            char payload[IPC_MAX_MESSAGE_SIZE] = {0};
            char messageId[64] = {0};
            char timestamp[32] = {0};
            
            if (parseIPCMessage(lineStart, messageType, payload, messageId, timestamp)) {
                callback(messageType, payload, userData);
            }
        }
        
        lineStart = lineEnd + 1;
    }
    
    // 移动剩余数据到缓冲区开始
    if (lineStart < ipc->readBuffer + ipc->bytesInBuffer) {
        size_t remainingBytes = ipc->readBuffer + ipc->bytesInBuffer - lineStart;
        memmove(ipc->readBuffer, lineStart, remainingBytes);
        ipc->bytesInBuffer = remainingBytes;
    } else {
        ipc->bytesInBuffer = 0;
    }
    
    return true;
}

bool sendIPCMessage(IPCManager* ipc, const char* messageType, const char* payload) {
    if (!ipc || !ipc->initialized || ipc->state != IPC_STATE_CONNECTED || !messageType) {
        return false;
    }
    
    char message[IPC_BUFFER_SIZE];
    if (!buildIPCMessage(messageType, payload, message, sizeof(message))) {
        return false;
    }
    
    DWORD bytesWritten = 0;
    BOOL success = WriteFile(
        ipc->hPipe,
        message,
        strlen(message),
        &bytesWritten,
        NULL
    );
    
    if (!success) {
        printf("[IPC] WriteFile failed: %lu\n", GetLastError());
        return false;
    }
    
    FlushFileBuffers(ipc->hPipe);
    return true;
}