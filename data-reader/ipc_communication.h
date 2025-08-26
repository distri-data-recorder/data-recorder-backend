// ============= ipc_communication.h =============
#ifndef IPC_COMMUNICATION_H
#define IPC_COMMUNICATION_H

#include <windows.h>
#include <stdint.h>
#include <stdbool.h>

// IPC配置
#define IPC_PIPE_NAME "\\\\.\\pipe\\data_reader_ipc"
#define IPC_BUFFER_SIZE 8192
#define IPC_MAX_MESSAGE_SIZE 4096

// IPC消息类型 - data-processor -> data-reader
typedef enum {
    IPC_MSG_FORWARD_TO_DEVICE = 1,
    IPC_MSG_SET_READER_MODE,
    IPC_MSG_REQUEST_READER_STATUS
} IPCMessageType_FromProcessor;

// IPC消息类型 - data-reader -> data-processor  
typedef enum {
    IPC_MSG_READER_STATUS_UPDATE = 1,
    IPC_MSG_DEVICE_FRAME_RECEIVED,
    IPC_MSG_DEVICE_LOG_RECEIVED,
    IPC_MSG_COMMAND_RESPONSE
} IPCMessageType_ToProcessor;

// IPC连接状态
typedef enum {
    IPC_STATE_DISCONNECTED = 0,
    IPC_STATE_LISTENING,
    IPC_STATE_CONNECTED
} IPCState;

// IPC管理器
typedef struct {
    HANDLE hPipe;
    IPCState state;
    char readBuffer[IPC_BUFFER_SIZE];
    DWORD bytesInBuffer;
    bool initialized;
} IPCManager;

// 回调函数类型
typedef void (*IPCMessageCallback)(const char* messageType, const char* payload, void* userData);

// IPC函数声明
bool initIPC(IPCManager* ipc);
void cleanupIPC(IPCManager* ipc);
bool processIPCMessages(IPCManager* ipc, IPCMessageCallback callback, void* userData);
bool sendIPCMessage(IPCManager* ipc, const char* messageType, const char* payload);

// JSON辅助函数
bool parseIPCMessage(const char* jsonLine, char* messageType, char* payload, char* messageId, char* timestamp);
bool buildIPCMessage(const char* messageType, const char* payload, char* output, size_t outputSize);

#endif // IPC_COMMUNICATION_H