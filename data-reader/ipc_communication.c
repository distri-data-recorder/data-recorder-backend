// ============= ipc_communication.c =============
#include "ipc_communication.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include <ctype.h>

// 前置声明（线程入口）
static DWORD WINAPI IpcWorkerThread(LPVOID param);

// ---------- 小工具 ----------
static void generateMessageId(char* buffer, size_t bufferSize) {
    if (!buffer || bufferSize == 0) return;
    // tick + rand 足够用于日志定位
    _snprintf_s(buffer, bufferSize, _TRUNCATE, "msg_%lu_%u",
                GetTickCount(), (unsigned)(rand() & 0xFFFF));
}

static void generateTimestamp(char* buffer, size_t bufferSize) {
    if (!buffer || bufferSize == 0) return;
    // ISO8601 UTC
    time_t now = time(NULL);
    struct tm utc_tm;
    gmtime_s(&utc_tm, &now);
    _snprintf_s(buffer, bufferSize, _TRUNCATE,
                "%04d-%02d-%02dT%02d:%02d:%02dZ",
                utc_tm.tm_year + 1900, utc_tm.tm_mon + 1, utc_tm.tm_mday,
                utc_tm.tm_hour, utc_tm.tm_min, utc_tm.tm_sec);
}

static void trim_quotes(char* s) {
    if (!s) return;
    size_t n = strlen(s);
    if (n >= 2 && s[0] == '\"' && s[n-1] == '\"') {
        memmove(s, s+1, n-2);
        s[n-2] = '\0';
    }
}

static void json_unescape_inplace(char* s) {
    // 简易反转义：只处理 \" \\ \n \r \t
    if (!s) return;
    char* r = s;
    char* w = s;
    while (*r) {
        if (*r == '\\') {
            r++;
            if (*r == 'n') { *w++ = '\n'; r++; }
            else if (*r == 'r') { *w++ = '\r'; r++; }
            else if (*r == 't') { *w++ = '\t'; r++; }
            else if (*r == '\"') { *w++ = '\"'; r++; }
            else if (*r == '\\') { *w++ = '\\'; r++; }
            else { *w++ = '\\'; if (*r) *w++ = *r++; }
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

// ---------- JSON 构造/解析 ----------
bool buildIPCMessage(const char* messageType,
                     const char* payload,
                     char* output,
                     size_t outputSize)
{
    if (!messageType || !output || outputSize == 0) return false;

    char id[64] = {0};
    char ts[64] = {0};
    generateMessageId(id, sizeof(id));
    generateTimestamp(ts, sizeof(ts));

    const char* pl = payload ? payload : "";

    // payload 作为字符串包装（上游如需 JSON 对象，请让 payload 自己包含花括号并在上游 build）
    int written = _snprintf_s(output, outputSize, _TRUNCATE,
        "{\"id\":\"%s\",\"timestamp\":\"%s\",\"type\":\"%s\",\"payload\":\"%s\"}\n",
        id, ts, messageType, pl);
    return written > 0;
}

static char* find_key_value(const char* json, const char* key, char* out, size_t outSize) {
    // 非严格 JSON 解析：找到 "key": 然后读取到逗号或右花括号
    // 仅支持 value 为 JSON 字符串（"xxx"）或简单字面量（不带逗号的片段）
    if (!json || !key || !out || outSize == 0) return NULL;

    const char* p = json;
    size_t keyLen = strlen(key);

    while ((p = strstr(p, key)) != NULL) {
        // 确认是 "key"
        if (p == json || *(p-1) == '\"') {
            const char* colon = strchr(p + keyLen, ':');
            if (!colon) { p += keyLen; continue; }

            // 跳过空白
            const char* v = colon + 1;
            while (*v == ' ' || *v == '\t') v++;

            // 字符串值
            if (*v == '\"') {
                v++;
                const char* endq = v;
                while (*endq) {
                    if (*endq == '\\') { endq++; if (*endq) endq++; else break; }
                    else if (*endq == '\"') break;
                    else endq++;
                }
                if (*endq != '\"') break;
                size_t len = (size_t)(endq - v);
                if (len >= outSize) len = outSize - 1;
                memcpy(out, v, len);
                out[len] = '\0';
                return out;
            }

            // 非字符串：读取到 , 或 }
            const char* end = v;
            while (*end && *end != ',' && *end != '}') end++;
            size_t len = (size_t)(end - v);
            if (len >= outSize) len = outSize - 1;
            memcpy(out, v, len);
            out[len] = '\0';
            return out;
        }
        p += keyLen;
    }
    return NULL;
}

bool parseIPCMessage(const char* jsonLine,
                     char* messageType,
                     char* payload,
                     char* messageId,
                     char* timestamp)
{
    if (!jsonLine || !messageType || !payload) return false;

    char typeTmp[128] = {0};
    char payloadTmp[IPC_MAX_MESSAGE_SIZE] = {0};
    char idTmp[128] = {0};
    char tsTmp[128] = {0};

    // 关键字段
    if (!find_key_value(jsonLine, "\"type\"", typeTmp, sizeof(typeTmp))) return false;
    trim_quotes(typeTmp);

    // payload 可空
    if (find_key_value(jsonLine, "\"payload\"", payloadTmp, sizeof(payloadTmp))) {
        trim_quotes(payloadTmp);
        json_unescape_inplace(payloadTmp);
    } else {
        payloadTmp[0] = '\0';
    }

    // 可选
    if (messageId && find_key_value(jsonLine, "\"id\"", idTmp, sizeof(idTmp))) {
        trim_quotes(idTmp);
        strncpy(messageId, idTmp, 127);
        messageId[127] = '\0';
    }
    if (timestamp && find_key_value(jsonLine, "\"timestamp\"", tsTmp, sizeof(tsTmp))) {
        trim_quotes(tsTmp);
        strncpy(timestamp, tsTmp, 127);
        timestamp[127] = '\0';
    }

    strncpy(messageType, typeTmp, 127);
    messageType[127] = '\0';
    strncpy(payload, payloadTmp, IPC_MAX_MESSAGE_SIZE - 1);
    payload[IPC_MAX_MESSAGE_SIZE - 1] = '\0';
    return true;
}

// ---------- 基础生命周期 ----------
bool initIPC(IPCManager* ipc) {
    if (!ipc) return false;

    ZeroMemory(ipc, sizeof(*ipc));
    ipc->state = IPC_STATE_LISTENING;

    // 使用阻塞模式（线程里阻塞读）
    ipc->hPipe = CreateNamedPipeA(
        IPC_PIPE_NAME,
        PIPE_ACCESS_DUPLEX,                   // 读写
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1,                                    // 实例数
        IPC_BUFFER_SIZE,                      // 出站缓冲
        IPC_BUFFER_SIZE,                      // 入站缓冲
        0,                                    // 超时（阻塞）
        NULL);

    if (ipc->hPipe == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "[IPC] CreateNamedPipe failed: %lu\n", GetLastError());
        return false;
    }

    ipc->hStopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!ipc->hStopEvent) {
        fprintf(stderr, "[IPC] CreateEvent failed: %lu\n", GetLastError());
        CloseHandle(ipc->hPipe);
        ipc->hPipe = NULL;
        return false;
    }

    ipc->bytesInBuffer = 0;
    ipc->readBuffer[0] = '\0';
    ipc->initialized = true;
    return true;
}

void cleanupIPC(IPCManager* ipc) {
    if (!ipc) return;

    // 确保线程已停
    stopIPCThread(ipc);

    if (ipc->hPipe && ipc->hPipe != INVALID_HANDLE_VALUE) {
        // 若仍在连接状态，断开
        if (ipc->state == IPC_STATE_CONNECTED || ipc->state == IPC_STATE_LISTENING) {
            DisconnectNamedPipe(ipc->hPipe);
        }
        CloseHandle(ipc->hPipe);
        ipc->hPipe = NULL;
    }

    if (ipc->hStopEvent) {
        CloseHandle(ipc->hStopEvent);
        ipc->hStopEvent = NULL;
    }

    ipc->initialized = false;
    ipc->state = IPC_STATE_DISCONNECTED;
    ipc->bytesInBuffer = 0;
    ipc->readBuffer[0] = '\0';
    ipc->threadCb = NULL;
    ipc->threadUser = NULL;
}

// 兼容旧轮询接口：不再真正读取，直接保存回调（以便发送端复用）
bool processIPCMessages(IPCManager* ipc, IPCMessageCallback callback, void* userData) {
    if (!ipc || !ipc->initialized) return false;
    ipc->threadCb = callback;
    ipc->threadUser = userData;
    // 真正读取由线程完成，这里避免阻塞主循环
    return true;
}

// ---------- 发送 ----------
bool sendIPCMessage(IPCManager* ipc, const char* messageType, const char* payload) {
    if (!ipc || !ipc->initialized) return false;

    if (ipc->state != IPC_STATE_CONNECTED) {
        // 没有客户端连接，静默丢弃或打印日志
        // fprintf(stderr, "[IPC] send: no client connected\n");
        return false;
    }

    char line[IPC_MAX_MESSAGE_SIZE + 256];
    if (!buildIPCMessage(messageType, payload, line, sizeof(line))) return false;

    DWORD written = 0;
    BOOL ok = WriteFile(ipc->hPipe, line, (DWORD)strlen(line), &written, NULL);
    if (!ok) {
        fprintf(stderr, "[IPC] WriteFile failed: %lu\n", GetLastError());
        return false;
    }
    return true;
}

// ---------- 线程实现 ----------
static void reset_read_buffer(IPCManager* ipc) {
    ipc->bytesInBuffer = 0;
    ipc->readBuffer[0] = '\0';
}

static BOOL accept_connection_blocking(IPCManager* ipc) {
    BOOL ok = ConnectNamedPipe(ipc->hPipe, NULL);
    DWORD err = GetLastError();
    if (ok || err == ERROR_PIPE_CONNECTED) {
        ipc->state = IPC_STATE_CONNECTED;
        reset_read_buffer(ipc);
        printf("[IPC] Client connected.\n");
        return TRUE;
    }
    return FALSE;
}

static void handle_disconnect(IPCManager* ipc) {
    printf("[IPC] Client disconnected.\n");
    DisconnectNamedPipe(ipc->hPipe);
    ipc->state = IPC_STATE_LISTENING;
    reset_read_buffer(ipc);
}

static DWORD WINAPI IpcWorkerThread(LPVOID param) {
    IPCManager* ipc = (IPCManager*)param;

    // 连接阶段：允许被停止
    for (;;) {
        if (WaitForSingleObject(ipc->hStopEvent, 0) == WAIT_OBJECT_0) return 0;
        if (accept_connection_blocking(ipc)) break;
        // 避免自旋
        if (WaitForSingleObject(ipc->hStopEvent, 50) == WAIT_OBJECT_0) return 0;
    }

    // 已连接：阻塞读循环
    for (;;) {
        if (WaitForSingleObject(ipc->hStopEvent, 0) == WAIT_OBJECT_0) break;

        DWORD canRead = IPC_BUFFER_SIZE - ipc->bytesInBuffer - 1;
        if (canRead == 0) { reset_read_buffer(ipc); canRead = IPC_BUFFER_SIZE - 1; }

        DWORD bytesRead = 0;
        BOOL ok = ReadFile(ipc->hPipe,
                           ipc->readBuffer + ipc->bytesInBuffer,
                           canRead, &bytesRead, NULL); // 阻塞
        if (!ok) {
            DWORD err = GetLastError();
            if (err == ERROR_BROKEN_PIPE || err == ERROR_NO_DATA) {
                handle_disconnect(ipc);
                // 回到连接阶段
                for (;;) {
                    if (WaitForSingleObject(ipc->hStopEvent, 0) == WAIT_OBJECT_0) return 0;
                    if (accept_connection_blocking(ipc)) break;
                    if (WaitForSingleObject(ipc->hStopEvent, 50) == WAIT_OBJECT_0) return 0;
                }
                continue;
            } else {
                fprintf(stderr, "[IPC] ReadFile failed: %lu\n", err);
                Sleep(5);
                continue;
            }
        }

        if (bytesRead == 0) continue;
        ipc->bytesInBuffer += bytesRead;
        ipc->readBuffer[ipc->bytesInBuffer] = '\0';

        // 按行拆包并回调
        char* start = ipc->readBuffer;
        for (;;) {
            char* eol = strchr(start, '\n');
            if (!eol) break;
            *eol = '\0';

            if (ipc->threadCb) {
                char type[128] = {0};
                char payload[IPC_MAX_MESSAGE_SIZE] = {0};
                char id[128] = {0};
                char ts[128] = {0};
                if (parseIPCMessage(start, type, payload, id, ts)) {
                    ipc->threadCb(type, payload, ipc->threadUser);
                } else {
                    // 容错：若不是 JSON，就把整行当 payload
                    ipc->threadCb("RAW", start, ipc->threadUser);
                }
            }

            start = eol + 1;
        }

        // 半行前移
        size_t remain = (size_t)(ipc->readBuffer + ipc->bytesInBuffer - start);
        memmove(ipc->readBuffer, start, remain);
        ipc->bytesInBuffer = (DWORD)remain;
        ipc->readBuffer[ipc->bytesInBuffer] = '\0';
    }

    return 0;
}

bool startIPCThread(IPCManager* ipc, IPCMessageCallback callback, void* userData) {
    if (!ipc || !ipc->initialized) return false;

    // 保存回调
    ipc->threadCb   = callback;
    ipc->threadUser = userData;

    // 复位停止事件
    if (!ipc->hStopEvent) {
        ipc->hStopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
        if (!ipc->hStopEvent) {
            fprintf(stderr, "[IPC] CreateEvent failed: %lu\n", GetLastError());
            return false;
        }
    } else {
        ResetEvent(ipc->hStopEvent);
    }

    // 启动线程
    ipc->hThread = CreateThread(NULL, 0, IpcWorkerThread, ipc, 0, NULL);
    if (!ipc->hThread) {
        fprintf(stderr, "[IPC] CreateThread failed: %lu\n", GetLastError());
        return false;
    }
    return true;
}

void stopIPCThread(IPCManager* ipc) {
    if (!ipc) return;
    if (ipc->hStopEvent) SetEvent(ipc->hStopEvent);
    if (ipc->hThread) {
        WaitForSingleObject(ipc->hThread, INFINITE);
        CloseHandle(ipc->hThread);
        ipc->hThread = NULL;
    }
}
