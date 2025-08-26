// ============= ipc_communication.h =============
#ifndef IPC_COMMUNICATION_H
#define IPC_COMMUNICATION_H

#include <windows.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// ---------- 配置 ----------
#define IPC_PIPE_NAME         "\\\\.\\pipe\\data_reader_ipc"
#define IPC_BUFFER_SIZE       8192
#define IPC_MAX_MESSAGE_SIZE  4096

// ---------- 状态 ----------
typedef enum {
    IPC_STATE_DISCONNECTED = 0,
    IPC_STATE_LISTENING,
    IPC_STATE_CONNECTED
} IPCState;

// ---------- 回调 ----------
// 回调在“IPC后台线程”中被调用；如需改共享状态，请自行加锁
typedef void (*IPCMessageCallback)(const char* messageType, const char* payload, void* userData);

// ---------- 管理器 ----------
typedef struct {
    HANDLE   hPipe;
    IPCState state;
    char     readBuffer[IPC_BUFFER_SIZE];
    DWORD    bytesInBuffer;
    bool     initialized;

    // 线程化 IPC
    HANDLE   hThread;        // 工作线程
    HANDLE   hStopEvent;     // 通知线程安全退出
    IPCMessageCallback threadCb;
    void*    threadUser;
} IPCManager;

// ---------- API ----------
// 初始化/清理
bool initIPC(IPCManager* ipc);
void cleanupIPC(IPCManager* ipc);

// 兼容旧接口：现在不再从主循环里轮询；保留以便不改调用方
bool processIPCMessages(IPCManager* ipc, IPCMessageCallback callback, void* userData);

// 发送一条消息（自动在末尾追加'\n'）
bool sendIPCMessage(IPCManager* ipc, const char* messageType, const char* payload);

// 启停后台线程
bool startIPCThread(IPCManager* ipc, IPCMessageCallback callback, void* userData);
void stopIPCThread(IPCManager* ipc);

// JSON 辅助（宽松实现：仅需能取出 type 与 payload）
bool parseIPCMessage(const char* jsonLine,
                     char* messageType,     // out
                     char* payload,         // out
                     char* messageId,       // out，可为NULL
                     char* timestamp);      // out，可为NULL

bool buildIPCMessage(const char* messageType,
                     const char* payload,   // 允许 NULL 或 ""
                     char* output,
                     size_t outputSize);

#endif // IPC_COMMUNICATION_H
