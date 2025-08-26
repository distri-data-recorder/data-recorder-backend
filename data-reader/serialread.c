#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <conio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "io_buffer.h"
#include "protocol.h"
#include "shared_memory.h"
#include "ipc_communication.h"

#ifdef _WIN32
#pragma comment(lib, "ws2_32.lib")
#endif

// ===================== Protocol V6 Command Definitions =====================
// System Control Commands (0x00-0x0F)
#define CMD_PING                    0x01
#define CMD_PONG                    0x81
#define CMD_GET_STATUS              0x02
#define CMD_STATUS_RESPONSE         0x82
#define CMD_GET_DEVICE_INFO         0x03
#define CMD_DEVICE_INFO_RESPONSE    0x83

// Collection Configuration & Control (0x10-0x1F)
#define CMD_SET_MODE_CONTINUOUS     0x10
#define CMD_SET_MODE_TRIGGER        0x11
#define CMD_START_STREAM            0x12
#define CMD_STOP_STREAM             0x13
#define CMD_CONFIGURE_STREAM        0x14
#define CMD_ACK                     0x90
#define CMD_NACK                    0x91

// Data & Event Transmission (0x40-0x4F)
#define CMD_DATA_PACKET             0x40
#define CMD_EVENT_TRIGGERED         0x41
#define CMD_REQUEST_BUFFERED_DATA   0x42
#define CMD_BUFFER_TRANSFER_COMPLETE 0x4F

// Logging (0xE0-0xEF)
#define CMD_LOG_MESSAGE             0xE0

// ===================== Configuration =====================
#define DEFAULT_COM_PORT        "\\\\.\\COM7"
#define DEFAULT_TCP_HOST        "127.0.0.1"
#define DEFAULT_TCP_PORT        "9001"
#define BAUDRATE                CBR_115200
#define BYTE_SIZE               8
#define STOP_BITS               ONESTOPBIT
#define PARITY_MODE             NOPARITY

#define FRAME_BATCH_SAVE_COUNT  500
#define MAX_FRAMES_PER_FILE     50000
#define FILE_NAME_PATTERN       "raw_frames_%03d.txt"

// ===================== Connection Types =====================
typedef enum {
    CONN_TYPE_SERIAL,
    CONN_TYPE_SOCKET
} ConnectionType;

typedef struct {
    ConnectionType type;
    union {
        HANDLE hSerial;
        SOCKET socket;
    };
    bool connected;
} Connection;

// ===================== Global Variables =====================
static RxBuffer_t g_rx;
static FILE*      g_fp           = NULL;
static int        g_fileIndex    = 0;
static uint32_t   g_framesInFile = 0;
static Connection g_conn         = {0};
static uint8_t    g_seqCounter   = 0;

// Device status tracking
static bool       g_deviceConnected     = false;
static bool       g_dataTransmissionOn  = false;
static uint32_t   g_dataPacketCount     = 0;
static uint32_t   g_totalFrameCount     = 0;
static uint64_t   g_deviceUniqueId      = 0;
static char       g_deviceInfo[512]     = {0};

typedef struct {
    uint8_t* data;
    uint16_t len;
} RawFrame_t;

static RawFrame_t g_frameBatch[FRAME_BATCH_SAVE_COUNT];
static int        g_frameInBatch = 0;

static volatile bool g_running = true;

// Shared memory and IPC managers
static SharedMemManager g_sharedMem = {0};
static IPCManager       g_ipc = {0};

static bool send_command(uint8_t commandID, const uint8_t* payload, uint16_t payloadLen);

// ===================== IPC Message Handlers =====================

static void handleForwardToDevice(const char* payload) {
    printf("[IPC] Forward to device command received\n");
    // TODO: 解析payload中的command_id和data字段
    // 示例payload: {"command_id":"0x03","data":"Base64EncodedData"}

    // 简化实现：如果收到FORWARD_TO_DEVICE，发送设备信息查询
    if (strstr(payload, "0x03")) {
        printf("[IPC] Forwarding GET_DEVICE_INFO command to device\n");
        send_command(CMD_GET_DEVICE_INFO, NULL, 0);
    }
}

static void handleSetReaderMode(const char* payload) {
    printf("[IPC] Set reader mode: %s\n", payload);
    // TODO: 解析mode和target参数，实际切换连接模式
}

static void handleRequestReaderStatus(const char* payload) {
    printf("[IPC] Reader status requested\n");

    // 构建状态响应JSON
    char statusPayload[1024];
    snprintf(statusPayload, sizeof(statusPayload),
             "{\"mode\":\"%s\",\"target\":\"%s\",\"device_connected\":%s,\"device_id\":\"%016llX\",\"data_transmission\":%s}",
             g_conn.type == CONN_TYPE_SERIAL ? "serial" : "socket",
             g_conn.type == CONN_TYPE_SERIAL ? "COM7" : "127.0.0.1:9001",
             g_deviceConnected ? "true" : "false",
             (unsigned long long)g_deviceUniqueId,
             g_dataTransmissionOn ? "true" : "false");

    sendIPCMessage(&g_ipc, "READER_STATUS_UPDATE", statusPayload);
}

static void onIPCMessage(const char* messageType, const char* payload, void* userData) {
    (void)userData;
    printf("[IPC] Received: %s\n", messageType);

    if (strcmp(messageType, "FORWARD_TO_DEVICE") == 0) {
        handleForwardToDevice(payload);
    } else if (strcmp(messageType, "SET_READER_MODE") == 0) {
        handleSetReaderMode(payload);
    } else if (strcmp(messageType, "REQUEST_READER_STATUS") == 0) {
        handleRequestReaderStatus(payload);
    } else {
        printf("[IPC] Unknown message type: %s\n", messageType);
    }
}

// ===================== Device Frame Notification =====================

static void notifyDeviceFrameReceived(uint8_t cmd, uint8_t seq, const uint8_t* payload, uint16_t payloadLen) {
    // 只转发非数据帧给data-processor
    if (cmd == CMD_DATA_PACKET) {
        return; // 数据包直接写入共享内存，不通过IPC
    }

    char framePayload[2048];
    char base64Data[1024] = {0};

    // 简化的Base64编码（实际项目中应使用专业的Base64库）
    if (payloadLen > 0) {
        strcpy(base64Data, "Base64PlaceholderData");
    }

    snprintf(framePayload, sizeof(framePayload),
             "{\"command_id\":\"0x%02X\",\"seq\":%u,\"payload_len\":%u,\"data\":\"%s\"}",
             cmd, seq, payloadLen, base64Data);

    sendIPCMessage(&g_ipc, "DEVICE_FRAME_RECEIVED", framePayload);
}

static void notifyDeviceLogReceived(const char* level, const char* message) {
    char logPayload[512];
    snprintf(logPayload, sizeof(logPayload),
             "{\"level\":\"%s\",\"message\":\"%s\"}",
             level, message);

    sendIPCMessage(&g_ipc, "DEVICE_LOG_RECEIVED", logPayload);
}

// ===================== Connection Management =====================
static bool conn_write_data(const uint8_t* data, uint32_t length)
{
    if (!g_conn.connected) {
        printf("[ERROR] Connection not established\n");
        return false;
    }

    if (g_conn.type == CONN_TYPE_SERIAL) {
        DWORD bytesWritten = 0;
        BOOL ok = WriteFile(g_conn.hSerial, data, length, &bytesWritten, NULL);
        return (ok && bytesWritten == length);
    } else if (g_conn.type == CONN_TYPE_SOCKET) {
        int bytesSent = send(g_conn.socket, (const char*)data, length, 0);
        return (bytesSent == (int)length);
    }
    return false;
}

static int conn_read_data(uint8_t* buffer, uint32_t bufferSize)
{
    if (!g_conn.connected) {
        return -1;
    }

    if (g_conn.type == CONN_TYPE_SERIAL) {
        DWORD bytesRead = 0;
        BOOL ok = ReadFile(g_conn.hSerial, buffer, bufferSize, &bytesRead, NULL);
        if (!ok) {
            DWORD err = GetLastError();
            if (err == ERROR_OPERATION_ABORTED) return -1;
            printf("ReadFile err=%lu\n", err);
            return -1;
        }
        return (int)bytesRead;
    } else if (g_conn.type == CONN_TYPE_SOCKET) {
        int bytesReceived = recv(g_conn.socket, (char*)buffer, bufferSize, 0);
        if (bytesReceived == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK) {
                return 0; // No data available
            }
            printf("recv error: %d\n", err);
            return -1;
        } else if (bytesReceived == 0) {
            printf("Connection closed by remote\n");
            return -1;
        }
        return bytesReceived;
    }
    return -1;
}

static void conn_close(void)
{
    if (!g_conn.connected) return;

    if (g_conn.type == CONN_TYPE_SERIAL) {
        CloseHandle(g_conn.hSerial);
        g_conn.hSerial = INVALID_HANDLE_VALUE;
    } else if (g_conn.type == CONN_TYPE_SOCKET) {
        closesocket(g_conn.socket);
        g_conn.socket = INVALID_SOCKET;
    }
    g_conn.connected = false;
}

// ===================== Connection Establishment =====================

static bool open_serial_connection(const char* comPort)
{
    HANDLE h = CreateFileA(
        comPort,
        GENERIC_READ | GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        0,
        NULL
    );
    if (h == INVALID_HANDLE_VALUE) {
        printf("Open %s failed, err=%lu\n", comPort, GetLastError());
        return false;
    }

    DCB dcb = {0};
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(h, &dcb)) {
        printf("GetCommState failed\n");
        CloseHandle(h);
        return false;
    }

    dcb.BaudRate = BAUDRATE;
    dcb.ByteSize = BYTE_SIZE;
    dcb.StopBits = STOP_BITS;
    dcb.Parity   = PARITY_MODE;

    if (!SetCommState(h, &dcb)) {
        printf("SetCommState failed\n");
        CloseHandle(h);
        return false;
    }

    COMMTIMEOUTS to = {0};
    to.ReadIntervalTimeout         = 10;
    to.ReadTotalTimeoutConstant    = 10;
    to.ReadTotalTimeoutMultiplier  = 2;
    to.WriteTotalTimeoutConstant   = 10;
    to.WriteTotalTimeoutMultiplier = 2;
    if (!SetCommTimeouts(h, &to)) {
        printf("SetCommTimeouts failed\n");
        CloseHandle(h);
        return false;
    }

    PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR | PURGE_RXABORT | PURGE_TXABORT);

    g_conn.type = CONN_TYPE_SERIAL;
    g_conn.hSerial = h;
    g_conn.connected = true;
    return true;
}

static bool open_socket_connection(const char* host, const char* port)
{
    // Initialize Winsock
    WSADATA wsaData;
    int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0) {
        printf("WSAStartup failed: %d\n", result);
        return false;
    }

    struct addrinfo *addrResult = NULL, hints;
    ZeroMemory(&hints, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    // Resolve server address and port
    result = getaddrinfo(host, port, &hints, &addrResult);
    if (result != 0) {
        printf("getaddrinfo failed: %d\n", result);
        WSACleanup();
        return false;
    }

    // Create socket
    SOCKET connectSocket = socket(addrResult->ai_family, addrResult->ai_socktype, addrResult->ai_protocol);
    if (connectSocket == INVALID_SOCKET) {
        printf("socket failed: %d\n", (int)WSAGetLastError());
        freeaddrinfo(addrResult);
        WSACleanup();
        return false;
    }

    // Connect to server
    result = connect(connectSocket, addrResult->ai_addr, (int)addrResult->ai_addrlen);
    freeaddrinfo(addrResult);

    if (result == SOCKET_ERROR) {
        printf("connect failed: %d\n", WSAGetLastError());
        closesocket(connectSocket);
        WSACleanup();
        return false;
    }

    // Set socket to non-blocking mode
    u_long mode = 1;
    ioctlsocket(connectSocket, FIONBIO, &mode);

    g_conn.type = CONN_TYPE_SOCKET;
    g_conn.socket = connectSocket;
    g_conn.connected = true;
    return true;
}
// ===================== Utility Functions =====================

static const char* get_command_name(uint8_t cmd)
{
    switch (cmd) {
        case CMD_PING:                    return "PING";
        case CMD_PONG:                    return "PONG";
        case CMD_GET_STATUS:              return "GET_STATUS";
        case CMD_STATUS_RESPONSE:         return "STATUS_RESPONSE";
        case CMD_GET_DEVICE_INFO:         return "GET_DEVICE_INFO";
        case CMD_DEVICE_INFO_RESPONSE:    return "DEVICE_INFO_RESPONSE";
        case CMD_SET_MODE_CONTINUOUS:     return "SET_MODE_CONTINUOUS";
        case CMD_SET_MODE_TRIGGER:        return "SET_MODE_TRIGGER";
        case CMD_START_STREAM:            return "START_STREAM";
        case CMD_STOP_STREAM:             return "STOP_STREAM";
        case CMD_CONFIGURE_STREAM:        return "CONFIGURE_STREAM";
        case CMD_ACK:                     return "ACK";
        case CMD_NACK:                    return "NACK";
        case CMD_DATA_PACKET:             return "DATA_PACKET";
        case CMD_EVENT_TRIGGERED:         return "EVENT_TRIGGERED";
        case CMD_REQUEST_BUFFERED_DATA:   return "REQUEST_BUFFERED_DATA";
        case CMD_BUFFER_TRANSFER_COMPLETE: return "BUFFER_TRANSFER_COMPLETE";
        case CMD_LOG_MESSAGE:             return "LOG_MESSAGE";
        default:                          return "UNKNOWN";
    }
}

static bool send_command(uint8_t commandID, const uint8_t* payload, uint16_t payloadLen)
{
    if (!g_conn.connected) {
        printf("[ERROR] Connection not established\n");
        return false;
    }

    uint8_t frameBuf[MAX_FRAME_SIZE];
    uint16_t frameLen = sizeof(frameBuf);

    int ret = buildFrame(commandID, g_seqCounter++, payload, payloadLen, frameBuf, &frameLen);
    if (ret != 0) {
        printf("[ERROR] Failed to build frame for command 0x%02X\n", commandID);
        return false;
    }

    if (!conn_write_data(frameBuf, frameLen)) {
        printf("[ERROR] Failed to send command 0x%02X (%s)\n", commandID, get_command_name(commandID));
        return false;
    }

    printf("[SENT] %s (0x%02X) seq=%u len=%u\n",
           get_command_name(commandID), commandID, g_seqCounter - 1, frameLen);
    return true;
}

// ===================== File Operations =====================

static bool open_next_file(void)
{
    if (g_fp) {
        fclose(g_fp);
        g_fp = NULL;
    }

    char name[64];
    snprintf(name, sizeof(name), FILE_NAME_PATTERN, g_fileIndex++);
    g_fp = fopen(name, "w");
    if (!g_fp) {
        printf("Open file %s failed!\n", name);
        return false;
    }
    g_framesInFile = 0;
    printf("[FILE] -> %s\n", name);
    return true;
}

static void flush_batch_to_file(void)
{
    if (!g_fp) return;

    for (int i = 0; i < g_frameInBatch; ++i) {
        if (g_framesInFile >= MAX_FRAMES_PER_FILE) {
            if (!open_next_file()) {
                free(g_frameBatch[i].data);
                continue;
            }
        }

        fprintf(g_fp, "LEN:%u HEX:", g_frameBatch[i].len);
        for (uint16_t j = 0; j < g_frameBatch[i].len; ++j) {
            fprintf(g_fp, " %02X", g_frameBatch[i].data[j]);
        }
        fputc('\n', g_fp);

        free(g_frameBatch[i].data);
        g_framesInFile++;
    }
    fflush(g_fp);
    g_frameInBatch = 0;
}

static void cache_frame(const uint8_t* frame, uint16_t len)
{
    uint8_t* copy = (uint8_t*)malloc(len);
    if (!copy) return;
    memcpy(copy, frame, len);

    g_frameBatch[g_frameInBatch].data = copy;
    g_frameBatch[g_frameInBatch].len  = len;
    g_frameInBatch++;

    if (g_frameInBatch >= FRAME_BATCH_SAVE_COUNT) {
        flush_batch_to_file();
    }
}

// ===================== Protocol V6 Message Handlers =====================

static void handle_pong_response(uint8_t seq, const uint8_t* payload, uint16_t payloadLen)
{
    printf("[RECV] PONG Response (seq=%u): ", seq);
    if (payloadLen >= 8) {
        g_deviceUniqueId = *(uint64_t*)payload;
        printf("Device ID=0x%016llX", (unsigned long long)g_deviceUniqueId);
        g_deviceConnected = true;
    } else {
        printf("Invalid payload length %u (expected 8)", payloadLen);
    }
    printf("\n");

    // 通知IPC客户端
    notifyDeviceFrameReceived(CMD_PONG, seq, payload, payloadLen);
}

static void handle_device_info_response(uint8_t seq, const uint8_t* payload, uint16_t payloadLen)
{
    printf("[RECV] Device Info Response (seq=%u):\n", seq);
    if (payloadLen < 4) {
        printf("  Invalid payload length\n");
        return;
    }

    uint16_t offset = 0;
    uint8_t protocol_version = payload[offset++];
    uint16_t fw_version = *(uint16_t*)(payload + offset);
    offset += 2;
    uint8_t num_channels = payload[offset++];

    printf("  Protocol Version: %u\n", protocol_version);
    printf("  Firmware Version: v%u.%u\n", fw_version >> 8, fw_version & 0xFF);
    printf("  Number of Channels: %u\n", num_channels);

    for (uint8_t ch = 0; ch < num_channels && offset < payloadLen; ch++) {
        if (offset + 8 > payloadLen) break;

        uint8_t channel_id = payload[offset++];
        uint32_t max_rate = *(uint32_t*)(payload + offset);
        offset += 4;
        uint16_t formats = *(uint16_t*)(payload + offset);
        offset += 2;
        uint8_t name_len = payload[offset++];

        if (offset + name_len > payloadLen) break;

        printf("  Channel %u: %.*s, Max Rate: %u Hz, Formats: 0x%04X\n",
               channel_id, name_len, (char*)(payload + offset), max_rate, formats);
        offset += name_len;
    }

    snprintf(g_deviceInfo, sizeof(g_deviceInfo),
             "Protocol V%u, FW v%u.%u, %u channels",
             protocol_version, fw_version >> 8, fw_version & 0xFF, num_channels);

    // 通知IPC客户端
    notifyDeviceFrameReceived(CMD_DEVICE_INFO_RESPONSE, seq, payload, payloadLen);
}

static void handle_status_response(uint8_t seq, const uint8_t* payload, uint16_t payloadLen)
{
    printf("[RECV] Status Response (seq=%u): ", seq);
    if (payloadLen >= 4) {
        uint8_t mode = payload[0];
        uint8_t stream_status = payload[1];
        uint8_t error_flag = payload[2];
        uint8_t error_code = payload[3];

        printf("Mode=%s, Stream=%s",
               mode == 0 ? "Continuous" : "Trigger",
               stream_status == 1 ? "Running" : "Stopped");

        if (error_flag) {
            printf(", Error=0x%02X", error_code);
        }

        g_dataTransmissionOn = (stream_status == 1);
    }
    printf("\n");

    // 通知IPC客户端
    notifyDeviceFrameReceived(CMD_STATUS_RESPONSE, seq, payload, payloadLen);
}

static void handle_data_packet(uint8_t seq, const uint8_t* payload, uint16_t payloadLen)
{
    (void)seq;
    g_dataPacketCount++;

    if (payloadLen < 8) {
        printf("[RECV] Invalid Data Packet #%u (len=%u)\n",
               g_dataPacketCount, payloadLen);
        return;
    }

    uint32_t timestamp = *(uint32_t*)payload;
    uint16_t channel_mask = *(uint16_t*)(payload + 4);
    uint16_t sample_count = *(uint16_t*)(payload + 6);

    printf("[RECV] Data Packet #%u: timestamp=%u, channels=0x%04X, samples=%u, len=%u\n",
           g_dataPacketCount, timestamp, channel_mask, sample_count, payloadLen);

    // Write to shared memory
    if (g_sharedMem.initialized) {
        if (!writeADCPacket(&g_sharedMem, seq, payload, payloadLen)) {
            printf("[SHARED_MEM] Failed to write data packet\n");
        }
    }
    // 数据包不通过IPC转发，直接写入共享内存
}

static void handle_log_message(uint8_t seq, const uint8_t* payload, uint16_t payloadLen)
{
    (void)seq;
    printf("[DEVICE LOG] ");
    if (payloadLen >= 2) {
        uint8_t log_level = payload[0];
        uint8_t msg_len = payload[1];
        const char* levelStr = "UNKNOWN";

        switch (log_level) {
            case 0: levelStr = "DEBUG"; break;
            case 1: levelStr = "INFO"; break;
            case 2: levelStr = "WARN"; break;
            case 3: levelStr = "ERROR"; break;
        }
        printf("[%s] ", levelStr);

        char message[256] = {0};
        if (payloadLen >= 2 + msg_len) {
            memcpy(message, payload + 2, msg_len);
            message[msg_len] = '\0';
            printf("%s", message);

            // 通知IPC客户端
            notifyDeviceLogReceived(levelStr, message);
        }
    }
    printf("\n");
}

static void handle_event_triggered(uint8_t seq, const uint8_t* payload, uint16_t payloadLen)
{
    printf("[RECV] Event Triggered (seq=%u): ", seq);
    if (payloadLen >= 4) {
        uint32_t timestamp = *(uint32_t*)payload;
        printf("timestamp=%u", timestamp);
        if (payloadLen >= 6) {
            uint16_t channel = *(uint16_t*)(payload + 4);
            printf(", channel=%u", channel);
        }
    }
    printf("\n");

    // 通知IPC客户端
    notifyDeviceFrameReceived(CMD_EVENT_TRIGGERED, seq, payload, payloadLen);

    // Automatically request buffered data
    printf("Requesting buffered trigger data...\n");
    send_command(CMD_REQUEST_BUFFERED_DATA, NULL, 0);
}

// ===================== Frame Processing =====================

static void onFrameParsed(const uint8_t* frame, uint16_t frameLen)
{
    cache_frame(frame, frameLen);
    g_totalFrameCount++;

    uint8_t  cmd = 0;
    uint8_t  seq = 0;
    uint8_t  payload[MAX_FRAME_SIZE];
    uint16_t payloadLen = 0;

    int ret = parseFrame(frame, frameLen, &cmd, &seq, payload, &payloadLen);
    if (ret == 0) {
        switch (cmd) {
            case CMD_PONG:
                handle_pong_response(seq, payload, payloadLen);
                break;
            case CMD_DEVICE_INFO_RESPONSE:
                handle_device_info_response(seq, payload, payloadLen);
                break;
            case CMD_STATUS_RESPONSE:
                handle_status_response(seq, payload, payloadLen);
                break;
            case CMD_DATA_PACKET:
                handle_data_packet(seq, payload, payloadLen);
                break;
            case CMD_EVENT_TRIGGERED:
                handle_event_triggered(seq, payload, payloadLen);
                break;
            case CMD_BUFFER_TRANSFER_COMPLETE:
                printf("[RECV] Buffer Transfer Complete (seq=%u)\n", seq);
                notifyDeviceFrameReceived(cmd, seq, payload, payloadLen);
                break;
            case CMD_LOG_MESSAGE:
                handle_log_message(seq, payload, payloadLen);
                break;
            case CMD_ACK:
                printf("[RECV] ACK (seq=%u)\n", seq);
                notifyDeviceFrameReceived(cmd, seq, payload, payloadLen);
                break;
            case CMD_NACK:
                printf("[RECV] NACK (seq=%u)\n", seq);
                notifyDeviceFrameReceived(cmd, seq, payload, payloadLen);
                break;
            default:
                printf("[RECV] Unknown Command 0x%02X (seq=%u, len=%u)\n", cmd, seq, payloadLen);
                break;
        }
    } else {
        printf("[Parse ERR] ret=%d (len=%u)\n", ret, frameLen);
    }
}

// ===================== User Interface =====================

static void print_help(void)
{
    printf("\n=== Protocol V6 Commands ===\n");
    printf("ESC/q/Q - Quit program\n");
    printf("h/H     - Show this help\n");
    printf("s       - Show status\n");
    printf("p       - Send PING\n");
    printf("i       - Get device info\n");
    printf("1       - Set continuous mode\n");
    printf("2       - Set trigger mode\n");
    printf("3       - Start stream\n");
    printf("4       - Stop stream\n");
    printf("c       - Configure stream (demo)\n");
    printf("========================\n\n");
}

static void print_status(void)
{
    printf("\n=== Current Status ===\n");
    printf("Connection: %s (%s)\n",
           g_conn.connected ? "CONNECTED" : "DISCONNECTED",
           g_conn.type == CONN_TYPE_SERIAL ? "Serial" : "Socket");
    printf("IPC: %s\n", g_ipc.state == IPC_STATE_CONNECTED ? "CONNECTED" : "LISTENING");
    printf("Device Connected: %s\n", g_deviceConnected ? "YES" : "NO");
    if (g_deviceUniqueId != 0) {
        printf("Device ID: 0x%016llX\n", (unsigned long long)g_deviceUniqueId);
    }
    if (strlen(g_deviceInfo) > 0) {
        printf("Device Info: %s\n", g_deviceInfo);
    }
    printf("Data Transmission: %s\n", g_dataTransmissionOn ? "ON" : "OFF");
    printf("Total Frames: %u\n", g_totalFrameCount);
    printf("Data Packets: %u\n", g_dataPacketCount);
    printf("Current Seq: %u\n", g_seqCounter);
    printf("===================\n\n");
}

static void send_demo_stream_config(void)
{
    uint8_t config_payload[13];
    uint16_t offset = 0;

    config_payload[offset++] = 2;

    config_payload[offset++] = 0;
    *(uint32_t*)(config_payload + offset) = 10000;
    offset += 4;
    config_payload[offset++] = 0x01;

    config_payload[offset++] = 1;
    *(uint32_t*)(config_payload + offset) = 10000;
    offset += 4;
    config_payload[offset++] = 0x01;

    printf("Sending stream configuration (2 channels @ 10kHz, int16)...\n");
    send_command(CMD_CONFIGURE_STREAM, config_payload, offset);
}

static bool handle_user_input(void)
{
    if (_kbhit()) {
        int ch = _getch();
        switch (ch) {
            case 27: case 'q': case 'Q':
                printf("Quit key pressed.\n");
                return true;
            case 'h': case 'H':
                print_help();
                break;
            case 's': case 'S':
                print_status();
                break;
            case 'p': case 'P':
                printf("Sending PING...\n");
                send_command(CMD_PING, NULL, 0);
                break;
            case 'i': case 'I':
                printf("Getting device info...\n");
                send_command(CMD_GET_DEVICE_INFO, NULL, 0);
                break;
            case '1':
                printf("Setting continuous mode...\n");
                send_command(CMD_SET_MODE_CONTINUOUS, NULL, 0);
                break;
            case '2':
                printf("Setting trigger mode...\n");
                send_command(CMD_SET_MODE_TRIGGER, NULL, 0);
                break;
            case '3':
                printf("Starting stream...\n");
                send_command(CMD_START_STREAM, NULL, 0);
                break;
            case '4':
                printf("Stopping stream...\n");
                send_command(CMD_STOP_STREAM, NULL, 0);
                break;
            case 'c': case 'C':
                send_demo_stream_config();
                break;
            default:
                printf("Unknown command '%c'. Press 'h' for help.\n", ch);
                break;
        }
    }
    return false;
}

// ===================== Main Communication Loop =====================

static void communication_loop(void)
{
    uint8_t buf[10000];

    initRxBuffer(&g_rx);

    printf("Communication started (Protocol V6). Press 'h' for help.\n");
    printf("Connection type: %s\n", g_conn.type == CONN_TYPE_SERIAL ? "Serial" : "TCP Socket");
    printf("IPC pipe: %s\n", IPC_PIPE_NAME);

    printf("Sending initial PING to detect device...\n");
    send_command(CMD_PING, NULL, 0);

    while (g_running) {
        // 处理设备通信
        int bytesRead = conn_read_data(buf, sizeof(buf));
        if (bytesRead > 0) {
            feedRxBuffer(&g_rx, buf, (uint16_t)bytesRead);
            tryParseFramesFromRx(&g_rx, onFrameParsed);
        } else if (bytesRead < 0) {
            printf("Connection error or closed\n");
            break;
        }

        // (已移除) 处理IPC消息由后台线程完成
        // processIPCMessages(&g_ipc, onIPCMessage, NULL);

        // 处理用户输入
        if (handle_user_input()) {
            g_running = false;
        }

        Sleep(1);
    }

    if (g_frameInBatch > 0)
        flush_batch_to_file();
}

// ===================== Usage =====================

static void print_usage(const char* progName)
{
    printf("Usage: %s [OPTIONS]\n", progName);
    printf("\nConnection Options:\n");
    printf("  %s COM_NUMBER           # Serial mode - use COMx port\n", progName);
    printf("  %s -s [HOST] [PORT]     # Socket mode - connect to TCP server\n", progName);
    printf("  %s                      # Default: COM7\n", progName);
    printf("\nExamples:\n");
    printf("  %s 3                    # Use COM3\n", progName);
    printf("  %s -s                   # Use TCP 127.0.0.1:9001\n", progName);
    printf("  %s -s 192.168.1.100     # Use TCP 192.168.1.100:9001\n", progName);
    printf("  %s -s 192.168.1.100 8080 # Use TCP 192.168.1.100:8080\n", progName);
    printf("\nFeatures:\n");
    printf("  - Protocol V6 support\n");
    printf("  - Shared memory for data exchange\n");
    printf("  - IPC communication via named pipes\n");
    printf("  - Raw frame logging to files\n");
}

// ===================== Main Function =====================

int main(int argc, char* argv[])
{
    bool useSocket = false;
    char host[64] = DEFAULT_TCP_HOST;
    char port[16] = DEFAULT_TCP_PORT;
    char comPort[32] = DEFAULT_COM_PORT;

    // Parse command line arguments (保持之前的解析逻辑)
    if (argc == 1) {
        strcpy(comPort, DEFAULT_COM_PORT);
    } else if (argc == 2) {
        if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[1], "-s") == 0) {
            useSocket = true;
        } else {
            int comNum = atoi(argv[1]);
            if (comNum <= 0 || comNum > 999) {
                printf("Error: Invalid COM port number.\n");
                print_usage(argv[0]);
                return 1;
            }
            snprintf(comPort, sizeof(comPort), "\\\\.\\COM%d", comNum);
        }
    } else if (argc == 3 && strcmp(argv[1], "-s") == 0) {
        useSocket = true;
        strcpy(host, argv[2]);
    } else if (argc == 4 && strcmp(argv[1], "-s") == 0) {
        useSocket = true;
        strcpy(host, argv[2]);
        strcpy(port, argv[3]);
    } else {
        printf("Error: Invalid arguments.\n");
        print_usage(argv[0]);
        return 1;
    }

    printf("=== Data Reader - Protocol V6 with IPC ===\n");
    if (useSocket) {
        printf("Mode: TCP Socket\n");
        printf("Target: %s:%s\n", host, port);
    } else {
        printf("Mode: Serial Port\n");
        printf("Port: %s\n", comPort);
        printf("Baud Rate: %u\n", (unsigned int)BAUDRATE);
    }
    printf("==========================================\n\n");

    if (!open_next_file()) {
        printf("Warning: Cannot open output file, frames won't be saved.\n");
    }

    // Initialize shared memory
    if (initSharedMemory(&g_sharedMem)) {
        printf("Shared memory initialized successfully.\n");
    } else {
        printf("Warning: Failed to initialize shared memory.\n");
    }

    // Initialize IPC
    if (initIPC(&g_ipc)) {
        printf("IPC initialized successfully.\n");
        // ★ 启动后台 IPC 线程（回调在后台线程里执行）
        if (!startIPCThread(&g_ipc, onIPCMessage, NULL)) {
            printf("Warning: Failed to start IPC thread.\n");
        }
    } else {
        printf("Warning: Failed to initialize IPC.\n");
    }

    // Establish connection
    bool connected = false;
    if (useSocket) {
        printf("Connecting to %s:%s...\n", host, port);
        connected = open_socket_connection(host, port);
    } else {
        printf("Opening serial port %s...\n", comPort);
        connected = open_serial_connection(comPort);
    }

    if (!connected) {
        if (g_fp) fclose(g_fp);
        cleanupSharedMemory(&g_sharedMem);
        // 停止线程并清理 IPC
        stopIPCThread(&g_ipc);
        cleanupIPC(&g_ipc);
        return 1;
    }

    printf("Starting communication... (ESC/q to quit)\n\n");
    communication_loop();

    // Cleanup
    conn_close();
    if (g_fp) fclose(g_fp);
    cleanupSharedMemory(&g_sharedMem);

    // ★ 退出前先停线程，再清理 IPC
    stopIPCThread(&g_ipc);
    cleanupIPC(&g_ipc);

    if (useSocket) {
        WSACleanup();
    }

    puts("Bye.");
    return 0;
}
