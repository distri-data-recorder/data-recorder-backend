# 项目源代码文档

> 本文档由Python脚本自动生成于: 2025-08-25 14:31:33

---

## 文件: `data-reader/Makefile`

```makefile
# ====== Config ======
TARGET     := serialread
SRC        := serialread.c protocol/protocol.c protocol/io_buffer.c shared_memory.c
INC_DIRS   := protocol
CC         := gcc

# Build type: make 或 make BUILD=release/debug
BUILD      ?= debug

# ====== Flags ======
CFLAGS_COMMON := -std=c11 -Wall -Wextra -Wno-unused-parameter -I$(INC_DIRS) -MMD -MP
ifeq ($(BUILD),release)
    CFLAGS := $(CFLAGS_COMMON) -O2
else
    CFLAGS := $(CFLAGS_COMMON) -O0 -g
endif

LDFLAGS    :=
LDLIBS     := -lkernel32 -luser32

# ====== Derived ======
OBJ        := $(SRC:.c=.o)
DEP        := $(OBJ:.o=.d)
EXE        := $(TARGET).exe

# ====== Rules ======
.PHONY: all run clean rebuild

all: $(EXE)

$(EXE): $(OBJ)
	$(CC) $(OBJ) -o $@ $(LDFLAGS) $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

run: $(EXE)
	./$(EXE)

clean:
ifeq ($(OS),Windows_NT)
	-del /Q $(subst /,\,$(OBJ)) $(subst /,\,$(DEP)) $(EXE) 2>nul
else
	$(RM) $(OBJ) $(DEP) $(EXE)
endif

rebuild: clean all

# 自动依赖
-include $(DEP)
```

## 文件: `data-reader/raw_frames_000.txt`

```

```

## 文件: `data-reader/serialread.c`

```c
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <conio.h>      // _kbhit/_getch
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "io_buffer.h"
#include "protocol.h"
#include "shared_memory.h"

// ===================== Command ID Definitions =====================
// Status and Parameter Commands (PC -> Device: 0x00-0x7F)
#define CMD_REQUEST_DEVICE_STATUS       0x10
#define CMD_SET_DEVICE_PARAMETERS       0x11
#define CMD_REQUEST_DEVICE_PARAMETERS   0x12

// Control Commands (PC -> Device)
#define CMD_START_DATA_TRANSMISSION     0x20
#define CMD_STOP_DATA_TRANSMISSION      0x21
#define CMD_DEVICE_RESET                0x22
#define CMD_PING                        0x2F

// Device Responses (Device -> PC: 0x80-0xFF)
#define CMD_DEVICE_STATUS_RESPONSE      0x90
#define CMD_PARAMETER_SET_RESPONSE      0x91
#define CMD_DEVICE_PARAMETERS_RESPONSE  0x92
#define CMD_START_COMMAND_RESPONSE      0xA0
#define CMD_STOP_COMMAND_RESPONSE       0xA1
#define CMD_RESET_COMMAND_RESPONSE      0xA2
#define CMD_PONG                        0xAF

// Data Transmission Commands (Device -> PC)
#define CMD_ADC_DATA_PACKET             0x40
#define CMD_OTHER_SENSOR_DATA           0x41

// Logging and Debugging Commands (Device -> PC)
#define CMD_LOG_MESSAGE                 0xE0

// ===================== 配置 =====================
#define DEFAULT_COM_PORT        "\\\\.\\COM7"
#define BAUDRATE                CBR_115200
#define BYTE_SIZE               8
#define STOP_BITS               ONESTOPBIT
#define PARITY_MODE             NOPARITY

#define FRAME_BATCH_SAVE_COUNT  500         // 内存中凑够 N 帧再写文件
#define MAX_FRAMES_PER_FILE     50000       // 每个文件最多保存 5 万帧
#define FILE_NAME_PATTERN       "raw_frames_%03d.txt"

// ===================== 全局变量 =====================
static RxBuffer_t g_rx;
static FILE*      g_fp           = NULL;
static int        g_fileIndex    = 0;
static uint32_t   g_framesInFile = 0;
static HANDLE     g_hSerial      = INVALID_HANDLE_VALUE;
static uint8_t    g_seqCounter   = 0;

// Device status tracking
static bool       g_deviceConnected     = false;
static bool       g_dataTransmissionOn  = false;
static uint32_t   g_adcPacketCount      = 0;
static uint32_t   g_totalFrameCount     = 0;

typedef struct {
    uint8_t* data;
    uint16_t len;
} RawFrame_t;

static RawFrame_t g_frameBatch[FRAME_BATCH_SAVE_COUNT];
static int        g_frameInBatch = 0;

static volatile bool g_running = true;

// 共享内存管理器
static SharedMemManager g_sharedMem = {0};

// ===================== 工具函数 =====================

static const char* get_command_name(uint8_t cmd)
{
    switch (cmd) {
        case CMD_REQUEST_DEVICE_STATUS:       return "REQUEST_DEVICE_STATUS";
        case CMD_SET_DEVICE_PARAMETERS:       return "SET_DEVICE_PARAMETERS";
        case CMD_REQUEST_DEVICE_PARAMETERS:   return "REQUEST_DEVICE_PARAMETERS";
        case CMD_START_DATA_TRANSMISSION:     return "START_DATA_TRANSMISSION";
        case CMD_STOP_DATA_TRANSMISSION:      return "STOP_DATA_TRANSMISSION";
        case CMD_DEVICE_RESET:                return "DEVICE_RESET";
        case CMD_PING:                        return "PING";
        case CMD_DEVICE_STATUS_RESPONSE:      return "DEVICE_STATUS_RESPONSE";
        case CMD_PARAMETER_SET_RESPONSE:      return "PARAMETER_SET_RESPONSE";
        case CMD_DEVICE_PARAMETERS_RESPONSE:  return "DEVICE_PARAMETERS_RESPONSE";
        case CMD_START_COMMAND_RESPONSE:      return "START_COMMAND_RESPONSE";
        case CMD_STOP_COMMAND_RESPONSE:       return "STOP_COMMAND_RESPONSE";
        case CMD_RESET_COMMAND_RESPONSE:      return "RESET_COMMAND_RESPONSE";
        case CMD_PONG:                        return "PONG";
        case CMD_ADC_DATA_PACKET:             return "ADC_DATA_PACKET";
        case CMD_OTHER_SENSOR_DATA:           return "OTHER_SENSOR_DATA";
        case CMD_LOG_MESSAGE:                 return "LOG_MESSAGE";
        default:                              return "UNKNOWN";
    }
}

static bool send_command(uint8_t commandID, const uint8_t* payload, uint16_t payloadLen)
{
    if (g_hSerial == INVALID_HANDLE_VALUE) {
        printf("[ERROR] Serial port not open\n");
        return false;
    }

    uint8_t frameBuf[MAX_FRAME_SIZE];
    uint16_t frameLen = sizeof(frameBuf);

    int ret = buildFrame(commandID, g_seqCounter++, payload, payloadLen, frameBuf, &frameLen);
    if (ret != 0) {
        printf("[ERROR] Failed to build frame for command 0x%02X\n", commandID);
        return false;
    }

    DWORD bytesWritten = 0;
    BOOL ok = WriteFile(g_hSerial, frameBuf, frameLen, &bytesWritten, NULL);
    if (!ok || bytesWritten != frameLen) {
        printf("[ERROR] Failed to send command 0x%02X (%s)\n", commandID, get_command_name(commandID));
        return false;
    }

    printf("[SENT] %s (0x%02X) seq=%u len=%u\n",
           get_command_name(commandID), commandID, g_seqCounter - 1, frameLen);
    return true;
}

static void print_usage(const char* progName)
{
    printf("Usage: %s [COM_NUMBER]\n", progName);
    printf("  COM_NUMBER: COM port number (e.g., 7 for COM7)\n");
    printf("  If not specified, defaults to COM7\n");
    printf("\nExamples:\n");
    printf("  %s 3      # Use COM3\n", progName);
    printf("  %s        # Use default COM7\n", progName);
    printf("\nInteractive Commands (during execution):\n");
    printf("  h/H     - Show help\n");
    printf("  s       - Show status\n");
    printf("  p       - Send PING\n");
    printf("  1       - Request device status\n");
    printf("  2       - Start data transmission\n");
    printf("  3       - Stop data transmission\n");
    printf("  r       - Reset device\n");
    printf("  ESC/q/Q - Quit program\n");
}

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

// 写批量缓存到文件
static void flush_batch_to_file(void)
{
    if (!g_fp) return;

    for (int i = 0; i < g_frameInBatch; ++i) {
        // 如果当前文件剩余容量不足，先切文件
        if (g_framesInFile >= MAX_FRAMES_PER_FILE) {
            if (!open_next_file()) {
                // 打不开就只能丢弃
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

// 缓存一帧到内存队列
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

// 处理设备状态响应
static void handle_device_status_response(uint8_t seq, const uint8_t* payload, uint16_t payloadLen)
{
    printf("[RECV] Device Status Response (seq=%u): ", seq);
    if (payloadLen > 0) {
        printf("Status=0x%02X", payload[0]);
        if (payloadLen > 1) {
            printf(" Data:");
            for (uint16_t i = 1; i < payloadLen; i++) {
                printf(" %02X", payload[i]);
            }
        }
    }
    printf("\n");
    g_deviceConnected = true;
}

// 处理ADC数据包
static void handle_adc_data_packet(uint8_t seq, const uint8_t* payload, uint16_t payloadLen)
{
    g_adcPacketCount++;
    printf("[RECV] ADC Data Packet #%u (seq=%u, len=%u): ", g_adcPacketCount, seq, payloadLen);

    // 显示前几个字节的数据
    uint16_t displayLen = (payloadLen > 16) ? 16 : payloadLen;
    for (uint16_t i = 0; i < displayLen; i++) {
        printf("%02X ", payload[i]);
    }
    if (payloadLen > 16) {
        printf("... (%u more bytes)", payloadLen - 16);
    }
    printf("\n");

    // 写入共享内存
    if (g_sharedMem.initialized) {
        if (writeADCPacket(&g_sharedMem, seq, payload, payloadLen)) {
            printf("[SHARED_MEM] ADC packet written (seq=%u, len=%u)\n", seq, payloadLen);
        } else {
            printf("[SHARED_MEM] Failed to write ADC packet\n");
        }
    }
}

// 处理日志消息
static void handle_log_message(uint8_t seq, const uint8_t* payload, uint16_t payloadLen)
{
    printf("[DEVICE LOG] ");
    if (payloadLen > 0) {
        // 假设第一个字节是日志级别
        uint8_t logLevel = payload[0];
        const char* levelStr = "UNKNOWN";
        switch (logLevel) {
            case 0: levelStr = "DEBUG"; break;
            case 1: levelStr = "INFO"; break;
            case 2: levelStr = "WARN"; break;
            case 3: levelStr = "ERROR"; break;
        }
        printf("[%s] ", levelStr);

        // 剩余字节作为消息内容
        if (payloadLen > 1) {
            for (uint16_t i = 1; i < payloadLen; i++) {
                if (payload[i] >= 32 && payload[i] <= 126) {
                    printf("%c", payload[i]);
                } else {
                    printf("\\x%02X", payload[i]);
                }
            }
        }
    }
    printf("\n");
}

// 处理其他响应命令
static void handle_command_response(uint8_t cmd, uint8_t seq, const uint8_t* payload, uint16_t payloadLen)
{
    printf("[RECV] %s (seq=%u): ", get_command_name(cmd), seq);

    switch (cmd) {
        case CMD_START_COMMAND_RESPONSE:
            if (payloadLen > 0 && payload[0] == 0) {
                printf("SUCCESS - Data transmission started");
                g_dataTransmissionOn = true;
            } else {
                printf("FAILED - Error code: %u", payloadLen > 0 ? payload[0] : 255);
            }
            break;

        case CMD_STOP_COMMAND_RESPONSE:
            if (payloadLen > 0 && payload[0] == 0) {
                printf("SUCCESS - Data transmission stopped");
                g_dataTransmissionOn = false;
            } else {
                printf("FAILED - Error code: %u", payloadLen > 0 ? payload[0] : 255);
            }
            break;

        case CMD_PONG:
            printf("PONG received - Device is alive");
            g_deviceConnected = true;
            break;

        case CMD_PARAMETER_SET_RESPONSE:
        case CMD_DEVICE_PARAMETERS_RESPONSE:
        case CMD_RESET_COMMAND_RESPONSE:
        default:
            if (payloadLen > 0) {
                printf("Data:");
                for (uint16_t i = 0; i < payloadLen; i++) {
                    printf(" %02X", payload[i]);
                }
            } else {
                printf("No payload");
            }
            break;
    }
    printf("\n");
}

// 解析成功回调（给 tryParseFramesFromRx 调用）
static void onFrameParsed(const uint8_t* frame, uint16_t frameLen)
{
    // 保存原始帧
    cache_frame(frame, frameLen);
    g_totalFrameCount++;

    // 再做协议解析
    uint8_t  cmd = 0;
    uint8_t  seq = 0;
    uint8_t  payload[MAX_FRAME_SIZE];
    uint16_t payloadLen = 0;

    int ret = parseFrame(frame, frameLen, &cmd, &seq, payload, &payloadLen);
    if (ret == 0) {
        // 根据CommandID处理不同类型的消息
        switch (cmd) {
            case CMD_DEVICE_STATUS_RESPONSE:
                handle_device_status_response(seq, payload, payloadLen);
                break;

            case CMD_ADC_DATA_PACKET:
                handle_adc_data_packet(seq, payload, payloadLen);
                break;

            case CMD_OTHER_SENSOR_DATA:
                printf("[RECV] Other Sensor Data (seq=%u, len=%u)\n", seq, payloadLen);
                break;

            case CMD_LOG_MESSAGE:
                handle_log_message(seq, payload, payloadLen);
                break;

            case CMD_START_COMMAND_RESPONSE:
            case CMD_STOP_COMMAND_RESPONSE:
            case CMD_RESET_COMMAND_RESPONSE:
            case CMD_PONG:
            case CMD_PARAMETER_SET_RESPONSE:
            case CMD_DEVICE_PARAMETERS_RESPONSE:
                handle_command_response(cmd, seq, payload, payloadLen);
                break;

            default:
                printf("[RECV] Unknown Command 0x%02X (seq=%u, len=%u)\n", cmd, seq, payloadLen);
                break;
        }
    } else {
        printf("[Parse ERR] ret=%d (len=%u)\n", ret, frameLen);
    }
}

static void print_help(void)
{
    printf("\n=== Available Commands ===\n");
    printf("ESC/q/Q - Quit program\n");
    printf("h/H     - Show this help\n");
    printf("s       - Show status\n");
    printf("p       - Send PING\n");
    printf("1       - Request device status\n");
    printf("2       - Start data transmission\n");
    printf("3       - Stop data transmission\n");
    printf("r       - Reset device\n");
    printf("========================\n\n");
}

static void print_status(void)
{
    printf("\n=== Current Status ===\n");
    printf("Device Connected: %s\n", g_deviceConnected ? "YES" : "NO");
    printf("Data Transmission: %s\n", g_dataTransmissionOn ? "ON" : "OFF");
    printf("Total Frames: %u\n", g_totalFrameCount);
    printf("ADC Packets: %u\n", g_adcPacketCount);
    printf("Current Seq: %u\n", g_seqCounter);
    printf("===================\n\n");
}

static bool handle_user_input(void)
{
    if (_kbhit()) {
        int ch = _getch();

        switch (ch) {
            case 27:  // ESC
            case 'q':
            case 'Q':
                printf("Quit key pressed.\n");
                return true;

            case 'h':
            case 'H':
                print_help();
                break;

            case 's':
            case 'S':
                print_status();
                break;

            case 'p':
            case 'P':
                printf("Sending PING...\n");
                send_command(CMD_PING, NULL, 0);
                break;

            case '1':
                printf("Requesting device status...\n");
                send_command(CMD_REQUEST_DEVICE_STATUS, NULL, 0);
                break;

            case '2':
                printf("Starting data transmission...\n");
                send_command(CMD_START_DATA_TRANSMISSION, NULL, 0);
                break;

            case '3':
                printf("Stopping data transmission...\n");
                send_command(CMD_STOP_DATA_TRANSMISSION, NULL, 0);
                break;

            case 'r':
            case 'R':
                printf("Sending device reset...\n");
                send_command(CMD_DEVICE_RESET, NULL, 0);
                break;

            default:
                printf("Unknown command '%c'. Press 'h' for help.\n", ch);
                break;
        }
    }
    return false;
}

static HANDLE open_serial(const char* comPort)
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
        return INVALID_HANDLE_VALUE;
    }

    DCB dcb = {0};
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(h, &dcb)) {
        printf("GetCommState failed\n");
        CloseHandle(h);
        return INVALID_HANDLE_VALUE;
    }

    dcb.BaudRate = BAUDRATE;
    dcb.ByteSize = BYTE_SIZE;
    dcb.StopBits = STOP_BITS;
    dcb.Parity   = PARITY_MODE;

    if (!SetCommState(h, &dcb)) {
        printf("SetCommState failed\n");
        CloseHandle(h);
        return INVALID_HANDLE_VALUE;
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
        return INVALID_HANDLE_VALUE;
    }

    PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR | PURGE_RXABORT | PURGE_TXABORT);
    return h;
}

static void serial_loop(HANDLE hSerial)
{
    uint8_t buf[10000];
    DWORD   bytesRead = 0;

    g_hSerial = hSerial;  // 设置全局句柄供发送命令使用
    initRxBuffer(&g_rx);

    printf("Serial communication started. Press 'h' for help.\n");

    // 发送初始PING命令检测设备
    printf("Sending initial PING to detect device...\n");
    send_command(CMD_PING, NULL, 0);

    while (g_running) {
        BOOL ok = ReadFile(hSerial, buf, sizeof(buf), &bytesRead, NULL);
        if (!ok) {
            DWORD err = GetLastError();
            if (err == ERROR_OPERATION_ABORTED) break;
            printf("ReadFile err=%lu\n", err);
            break;
        }

        if (bytesRead > 0) {
            // ★ 修正：使用 feedRxBuffer()
            feedRxBuffer(&g_rx, buf, (uint16_t)bytesRead);

            // 尝试解析
            tryParseFramesFromRx(&g_rx, onFrameParsed);
        }

        // 处理用户输入
        if (handle_user_input()) {
            g_running = false;
        }

        Sleep(1);
    }

    // 收尾：把剩余缓存写入
    if (g_frameInBatch > 0)
        flush_batch_to_file();

    g_hSerial = INVALID_HANDLE_VALUE;
}

int main(int argc, char* argv[])
{
    char comPort[32];

    // 解析命令行参数
    if (argc > 2) {
        printf("Error: Too many arguments.\n\n");
        print_usage(argv[0]);
        return 1;
    }

    if (argc == 2) {
        // 检查是否是帮助参数
        if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "/?") == 0) {
            print_usage(argv[0]);
            return 0;
        }

        // 解析COM端口号
        int comNum = atoi(argv[1]);
        if (comNum <= 0 || comNum > 999) {
            printf("Error: Invalid COM port number '%s'. Must be between 1 and 999.\n\n", argv[1]);
            print_usage(argv[0]);
            return 1;
        }
        snprintf(comPort, sizeof(comPort), "\\\\.\\COM%d", comNum);
    } else {
        // 使用默认COM端口
        strcpy(comPort, DEFAULT_COM_PORT);
    }

    if (!open_next_file()) {
        // 没法写文件也可以继续，只是不会存原始帧
        printf("Warning: cannot open output file, frames won't be saved.\n");
    }

    // 初始化共享内存
    if (initSharedMemory(&g_sharedMem)) {
        printf("Shared memory initialized successfully.\n");
    } else {
        printf("Warning: Failed to initialize shared memory. Data processing will not be available.\n");
    }

    HANDLE hSerial = open_serial(comPort);
    if (hSerial == INVALID_HANDLE_VALUE) {
        if (g_fp) fclose(g_fp);
        return 1;
    }

    printf("Start reading %s ... (ESC/q to quit)\n", comPort);
    serial_loop(hSerial);

    CloseHandle(hSerial);
    if (g_fp) fclose(g_fp);

    // 清理共享内存
    cleanupSharedMemory(&g_sharedMem);

    puts("Bye.");
    return 0;
}
```

## 文件: `data-reader/shared_memory.c`

```c
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
```

## 文件: `data-reader/shared_memory.h`

```c
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
```

## 文件: `data-reader/protocol/io_buffer.c`

```c
#include "io_buffer.h"
#include <string.h>

// ========== RxBuffer ==========

void initRxBuffer(RxBuffer_t* r)
{
    r->head = 0;
    r->tail = 0;
}

static uint16_t rxBufferFreeSpace(const RxBuffer_t* r)
{
    // 典型环形队列剩余空间计算
    // (tail + size - head - 1) % size
    // 这里 size = RX_BUFFER_SIZE
    return (uint16_t)((r->head + RX_BUFFER_SIZE - r->tail - 1) % RX_BUFFER_SIZE);
}

uint16_t feedRxBuffer(RxBuffer_t* r, const uint8_t* data, uint16_t len)
{
    uint16_t freeSpace = rxBufferFreeSpace(r);
    uint16_t toWrite = (len <= freeSpace) ? len : freeSpace;

    // 一次写入可能需要两段拷贝(若环形队列“折返”)
    for (uint16_t i = 0; i < toWrite; i++) {
        r->buf[r->tail] = data[i];
        r->tail = (r->tail + 1) % RX_BUFFER_SIZE;
    }
    return toWrite;
}

// 根据前面 protocol.c 的最小帧长度，定义 MIN_FRAME_LEN=8
#define MIN_FRAME_LEN 8

static bool tryExtractOneFrame(RxBuffer_t* r, uint8_t* outFrame, uint16_t* outFrameLen)
{
    // 思路：从 r->head 开始，寻找帧头(0xAA,0x55)，找到后再看能否凑齐一帧最小长度
    // 若凑齐一帧，拷贝到 outFrame 并返回 true，否则返回 false

    uint16_t available = (uint16_t)((r->tail + RX_BUFFER_SIZE - r->head) % RX_BUFFER_SIZE);
    if (available < MIN_FRAME_LEN) {
        return false; // 不够最小长度
    }

    // 用临时索引遍历
    uint16_t idx = r->head;
    while (available >= MIN_FRAME_LEN) {
        // 检查帧头
        uint8_t b0 = r->buf[idx];
        uint8_t b1 = r->buf[(idx+1) % RX_BUFFER_SIZE];
        if (b0 == 0xAA && b1 == 0x55) {
            // 可能是帧头，先至少需要 4 字节读到 length
            if (available < MIN_FRAME_LEN) {
                return false;
            }
            // 读取 length
            uint8_t l0 = r->buf[(idx+2) % RX_BUFFER_SIZE];
            uint8_t l1 = r->buf[(idx+3) % RX_BUFFER_SIZE];
            uint16_t lengthField = (uint16_t)l0 | ((uint16_t)l1 << 8);
            uint16_t frameSize = 2 + 2 + lengthField + 2; // = 6 + lengthField

            if (frameSize > MAX_FRAME_SIZE) {
                // 帧长度超过单帧最大值 -> 可能是垃圾数据，跳过此帧头
                idx = (idx + 1) % RX_BUFFER_SIZE;
                available--;
                continue;
            }
            if (frameSize > available) {
                // 数据还不够整帧
                return false;
            }

            // 拿到一个完整帧，拷贝
            for (uint16_t i = 0; i < frameSize; i++) {
                outFrame[i] = r->buf[(idx + i) % RX_BUFFER_SIZE];
            }
            *outFrameLen = frameSize;

            // head 前进 frameSize
            r->head = (uint16_t)((idx + frameSize) % RX_BUFFER_SIZE);
            return true;
        } else {
            // 不是帧头，继续往后找
            idx = (idx + 1) % RX_BUFFER_SIZE;
            available--;
        }
    }

    return false;
}

void tryParseFramesFromRx(
    RxBuffer_t* r,
    void (*onFrame)(const uint8_t* frame, uint16_t frameLen)
)
{
    uint8_t tempFrame[MAX_FRAME_SIZE];
    uint16_t frameLen;

    // 尝试不断提取帧头 -> 复制完整帧 -> 调用回调
    while (1) {
        bool got = tryExtractOneFrame(r, tempFrame, &frameLen);
        if (!got) {
            break;
        }
        // 找到完整帧，调用回调
        onFrame(tempFrame, frameLen);
    }
}

// ========== TxBuffer ==========

void initTxBuffer(TxBuffer_t* t)
{
    t->head = 0;
    t->tail = 0;
}

static uint16_t txBufferFreeSpace(const TxBuffer_t* t)
{
    return (uint16_t)((t->head + TX_BUFFER_SIZE - t->tail - 1) % TX_BUFFER_SIZE);
}

int enqueueTxFrame(TxBuffer_t* t, const uint8_t* frame, uint16_t frameLen)
{
    // 这里为了区分多帧，可以先写 2 字节表示本帧长度，再写帧内容
    // [frameLenLow][frameLenHigh][frameData...]

    // 需要 frameLen + 2 字节
    uint16_t needed = frameLen + 2;
    if (needed > txBufferFreeSpace(t)) {
        return -1; // 空间不足
    }

    // 写入 frameLen(小端)
    t->buf[t->tail] = (uint8_t)(frameLen & 0xFF);
    t->tail = (t->tail + 1) % TX_BUFFER_SIZE;
    t->buf[t->tail] = (uint8_t)(frameLen >> 8);
    t->tail = (t->tail + 1) % TX_BUFFER_SIZE;

    // 写入frame本身
    for (uint16_t i = 0; i < frameLen; i++) {
        t->buf[t->tail] = frame[i];
        t->tail = (t->tail + 1) % TX_BUFFER_SIZE;
    }
    return 0;
}

uint16_t dequeueTxFrame(TxBuffer_t* t, uint8_t* outFrame, uint16_t maxLen)
{
    // 如果没有足够的数据来读出 2 字节长度，说明无数据
    uint16_t available = (uint16_t)((t->tail + TX_BUFFER_SIZE - t->head) % TX_BUFFER_SIZE);
    if (available < 2) {
        return 0;
    }

    // 读出 frameLen
    uint8_t l0 = t->buf[t->head];
    t->head = (t->head + 1) % TX_BUFFER_SIZE;
    uint8_t l1 = t->buf[t->head];
    t->head = (t->head + 1) % TX_BUFFER_SIZE;
    uint16_t frameLen = (uint16_t)l0 | ((uint16_t)l1 << 8);

    // 如果剩余可读数据 < frameLen，说明数据不完整 -> 回退指针(简单处理)
    available -= 2;
    if (frameLen > available) {
        // 回退指针
        t->head = (uint16_t)( (t->head + TX_BUFFER_SIZE - 2) % TX_BUFFER_SIZE );
        return 0;
    }

    // 如果 caller 提供的 outFrame 不够存放该帧，则丢弃或只读取部分(看需求)
    uint16_t copyLen = (frameLen <= maxLen) ? frameLen : maxLen;

    for (uint16_t i = 0; i < copyLen; i++) {
        outFrame[i] = t->buf[t->head];
        t->head = (t->head + 1) % TX_BUFFER_SIZE;
    }
    // 如果 frameLen > copyLen，剩余的字节也需要在环形队列里跳过
    for (uint16_t i = copyLen; i < frameLen; i++) {
        t->head = (t->head + 1) % TX_BUFFER_SIZE;
    }

    return frameLen;
}
```

## 文件: `data-reader/protocol/io_buffer.h`

```c
#ifndef IO_BUFFER_H
#define IO_BUFFER_H

#include <stdint.h>
#include <stdbool.h>

#define RX_BUFFER_SIZE (65535)
#define TX_BUFFER_SIZE 8192
#define MAX_FRAME_SIZE 5120

// 环形队列结构
typedef struct {
    uint8_t  buf[RX_BUFFER_SIZE];
    uint16_t head;
    uint16_t tail;
} RxBuffer_t;

typedef struct {
    uint8_t  buf[TX_BUFFER_SIZE];
    uint16_t head;
    uint16_t tail;
} TxBuffer_t;

// 初始化
void initRxBuffer(RxBuffer_t* r);
void initTxBuffer(TxBuffer_t* t);

// 向 Rx 环形队列压入数据(原始字节流)，返回实际存入的字节数
uint16_t feedRxBuffer(RxBuffer_t* r, const uint8_t* data, uint16_t len);

// 从 RxBuffer 中解析出尽可能多的完整帧，并调用回调函数处理
// 回调函数原型： void onFrame(const uint8_t* frame, uint16_t frameLen)
void tryParseFramesFromRx(
    RxBuffer_t* r,
    void (*onFrame)(const uint8_t* frame, uint16_t frameLen)
);

// ---------------------- Tx 相关 ----------------------

// 向 TxBuffer 中添加一帧(完整帧)。返回 0=成功, -1=空间不足
int enqueueTxFrame(TxBuffer_t* t, const uint8_t* frame, uint16_t frameLen);

// 从 TxBuffer 中取出一帧(若有)，返回帧长度，0=无数据
// outFrame 缓冲区由调用者提供
uint16_t dequeueTxFrame(TxBuffer_t* t, uint8_t* outFrame, uint16_t maxLen);

#endif
```

## 文件: `data-reader/protocol/protocol.c`

```c
#include "protocol.h"
#include <string.h> // for memcpy
 
/*
* CRC16 (MODBUS) 常见实现
* 多项式 0x8005 或 0xA001 都常见，不同库对左右移、反转、初始值等可能有差异
* 这里给出一个常用示例，可根据具体项目修改
*/
uint16_t CRC16_Calc(const uint8_t* data, uint16_t length, uint16_t initVal)
{
    uint16_t crc = initVal; // 通常初值可设 0xFFFF, 0x0000, 等
 
    for (uint16_t i = 0; i < length; i++) {
        crc ^= (uint16_t)data[i];
        for (uint8_t j = 0; j < 8; j++) {
            if (crc & 0x0001) {
                crc >>= 1;
                crc ^= 0xA001; // 常见多项式
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}
 
int buildFrame(
    uint8_t commandID,
    uint8_t seq,
    const uint8_t* payload,
    uint16_t payloadLen,
    uint8_t* outBuf,
    uint16_t* outBufLen
)
{
    // 计算本帧的 length (不含 FrameHead 和 FrameTail)
    // length = CommandID(1) + Seq(1) + Payload(N) + CheckSum(2)
    uint16_t lengthField = 1 + 1 + payloadLen + 2;
    uint16_t totalFrameSize = 2 + 2 + lengthField + 2;
    // 即：帧头(2) + Length(2) + lengthField + 帧尾(2)
 
    if (totalFrameSize > *outBufLen) {
        // 缓冲区不够
        return -1;
    }
 
    // 开始组帧
    uint16_t offset = 0;
 
    // 帧头
    outBuf[offset++] = FRAME_HEAD_0;
    outBuf[offset++] = FRAME_HEAD_1;
 
    // Length: 小端存储
    outBuf[offset++] = (uint8_t)(lengthField & 0xFF);
    outBuf[offset++] = (uint8_t)((lengthField >> 8) & 0xFF);
 
    // CommandID
    outBuf[offset++] = commandID;
    // Seq
    outBuf[offset++] = seq;
 
    // Payload
    if (payloadLen > 0 && payload != NULL) {
        memcpy(&outBuf[offset], payload, payloadLen);
        offset += payloadLen;
    }
 
    // 计算 CRC16
    // 计算范围：从 CommandID 到 Payload 末尾
    uint16_t crc = CRC16_Calc(&outBuf[4], (uint16_t)(2 + payloadLen), 0xFFFF);
    //   &outBuf[4] -> 是 CommandID 的地址
    //   长度 = CommandID(1) + Seq(1) + Payload(payloadLen)
    //   initVal = 0xFFFF (常用 MODBUS 初始值)
 
    // 写入校验
    outBuf[offset++] = (uint8_t)(crc & 0xFF);
    outBuf[offset++] = (uint8_t)((crc >> 8) & 0xFF);
 
    // 帧尾
    outBuf[offset++] = FRAME_TAIL_0;
    outBuf[offset++] = FRAME_TAIL_1;
 
    *outBufLen = offset; // 实际帧长度
    return 0;
}
 
int parseFrame(
    const uint8_t* inBuf,
    uint16_t inLen,
    uint8_t* pCmd,
    uint8_t* pSeq,
    uint8_t* pPayload,
    uint16_t* pPayloadLen
)
{
    // 基本长度检查
    if (inLen < 8) {
        // 连最小的帧结构都放不下
        return -3;
    }
 
    // 检查帧头
    if ( (inBuf[0] != FRAME_HEAD_0) || (inBuf[1] != FRAME_HEAD_1) ) {
        return -1;
    }
 
    // 检查帧尾
    if ( (inBuf[inLen - 2] != FRAME_TAIL_0) || (inBuf[inLen - 1] != FRAME_TAIL_1) ) {
        return -2;
    }
 
    // 解析 Length
    //   inBuf[2] = Length 低字节
    //   inBuf[3] = Length 高字节
    uint16_t lengthField = (uint16_t)(inBuf[2]) | ((uint16_t)(inBuf[3]) << 8);
 
    // lengthField 应 = (CommandID(1) + Seq(1) + Payload + CheckSum(2))
    // 实际帧大小 = 2 + 2 + lengthField + 2 = 6 + lengthField
    uint16_t expectedFrameSize = 6 + lengthField;
    if (expectedFrameSize != inLen) {
        // 长度不一致
        return -3;
    }
 
    // 提取 CommandID 和 Seq
    uint16_t offset = 4;
    *pCmd = inBuf[offset++];
    *pSeq = inBuf[offset++];
 
    // 计算 payload 的大小
    // lengthField = 1(cmd) + 1(seq) + N(payload) + 2(checksum)
    uint16_t payloadLen = lengthField - 4;
    // 其中 4 = cmd(1) + seq(1) + checksum(2)
 
    // 读取 payload
    if (pPayload && payloadLen > 0) {
        memcpy(pPayload, &inBuf[offset], payloadLen);
    }
    if (pPayloadLen) {
        *pPayloadLen = payloadLen;
    }
    offset += payloadLen;
 
    // 读取校验
    uint16_t recvCRC = (uint16_t)(inBuf[offset])
                     | ((uint16_t)(inBuf[offset + 1]) << 8);
    
    // 计算校验
    uint16_t calcCRC = CRC16_Calc(&inBuf[4], (uint16_t)(2 + payloadLen), 0xFFFF);
    // 同 buildFrame 对齐：
    //   &inBuf[4] => CommandID处
    //   长度 = CommandID(1) + Seq(1) + payloadLen
 
    if (recvCRC != calcCRC) {
        return -4;
    }
 
    return 0; // 成功
}
```

## 文件: `data-reader/protocol/protocol.h`

```c
#ifndef PROTOCOL_H
#define PROTOCOL_H
 
#include <stdint.h>
 
// 帧头和帧尾定义
#define FRAME_HEAD_0        0xAA
#define FRAME_HEAD_1        0x55
#define FRAME_TAIL_0        0x55
#define FRAME_TAIL_1        0xAA
 
// 允许的最大帧长度 (示例设为 1024，可按需调整)
#define MAX_FRAME_SIZE      5120
 
// CRC16 预留
uint16_t CRC16_Calc(const uint8_t* data, uint16_t length, uint16_t initVal);
 
// 打包帧
// 参数：
//   commandID: 命令码
//   seq: 序列号
//   payload: 负载指针
//   payloadLen: 负载长度
//   outBuf: 输出缓冲区
//   outBufLen: 输出缓冲区长度(传入), 返回实际帧字节数(传出)
// 返回值：0=成功，-1=输出缓冲区不够
int buildFrame(
    uint8_t commandID,
    uint8_t seq,
    const uint8_t* payload,
    uint16_t payloadLen,
    uint8_t* outBuf,
    uint16_t* outBufLen
);
 
// 解析帧
// 参数：
//   inBuf: 输入数据缓冲区
//   inLen: 输入数据长度
//   pCmd:  [输出]解析到的 CommandID
//   pSeq:  [输出]解析到的 Seq
//   pPayload: [输出]解析到的负载, 需调用方准备好足够空间
//   pPayloadLen: [输出]实际负载长度
// 返回值：0=成功，-1=帧头不对，-2=帧尾不对，-3=长度非法，-4=CRC错误
int parseFrame(
    const uint8_t* inBuf,
    uint16_t inLen,
    uint8_t* pCmd,
    uint8_t* pSeq,
    uint8_t* pPayload,
    uint16_t* pPayloadLen
);
 
#endif // PROTOCOL_H
```

## 文件: `data-reader/protocol/readme.md`

```markdown
# Communication Protocol Documentation

## 1. Frame Format

The
 communication protocol employs a structured frame to guarantee 
reliability and facilitate easy parsing. The frame structure is detailed
 as follows:

| Field         | Size               | Description                                                                                                                         |
| ------------- | ------------------ | ----------------------------------------------------------------------------------------------------------------------------------- |
| **FrameHead** | 2 Bytes            | Frame header, with a fixed value (e.g., `0xAA 0x55`) for straightforward identification.                                            |
| **Length**    | 2 Bytes            | Total frame length, spanning from **CommandID** to **CheckSum**, excluding FrameHead and FrameTail. Stored in little-endian format. |
| **CommandID** | 1 Byte             | Command identifier, utilized to distinguish various commands or data types.                                                         |
| **Seq**       | 1 Byte             | Sequence number for pairing requests and responses or for frame counting.                                                           |
| **Payload**   | Variable (N Bytes) | Data payload specific to the command.                                                                                               |
| **CheckSum**  | 2 Bytes            | CRC16 checksum computed from **CommandID** to the end of the **Payload**.                                                           |
| **FrameTail** | 2 Bytes            | Frame tail, possessing a fixed value (e.g., `0x55 0xAA`) for frame boundary validation.                                             |

## 2. Frame Layout

| Field     | Size               | Value                                                                   |
| --------- | ------------------ | ----------------------------------------------------------------------- |
| FrameHead | 2 Bytes            | `0xAA 0x55`                                                             |
| Length    | 2 Bytes            | Total frame length (from CommandID to CheckSum) in little-endian format |
| CommandID | 1 Byte             | Command identifier                                                      |
| Seq       | 1 Byte             | Sequence number                                                         |
| Payload   | Variable (N Bytes) | Command-specific data                                                   |
| CheckSum  | 2 Bytes            | CRC16 value calculated from CommandID to the end of Payload             |
| FrameTail | 2 Bytes            | `0x55 0xAA`                                                             |

**Frame Structure Visualization**:  
`FrameHead (2B) | Length (2B) | CommandID (1B) | Seq (1B) | Payload (N B) | CheckSum (2B) | FrameTail (2B)`  
Example: `0xAA 0x55 0xXX 0xXX 0x?? 0x?? ... 0x?? 0x?? 0x55 0xAA`

## 3. Command Definitions

CommandID is used to distinguish different operation types. The following allocation rules are adopted:

- **PC -> Device**: 0x00 - 0x7F (requests or control commands)
- **Device -> PC**: 0x80 - 0xFF (responses or active reports)
- **Response Command ID**: Typically, the highest bit of the corresponding request command ID is set to 1 (i.e., Request_ID | 0x80).

### (1) Status and Parameter Commands

| CommandID | Direction    | Meaning                    | Remarks                                                                      |
| --------- | ------------ | -------------------------- | ---------------------------------------------------------------------------- |
| 0x10      | PC -> Device | Request Device Status      | Payload can be empty. Used to query the current state of the device.         |
| 0x90      | Device -> PC | Device Status Response     | Payload contains device status information (e.g., running, idle, error).     |
| 0x11      | PC -> Device | Set Device Parameters      | Payload contains the parameter identifiers and their values.                 |
| 0x91      | Device -> PC | Parameter Set Response     | Payload indicates success or failure. On failure, can include an error code. |
| 0x12      | PC -> Device | Request Device Parameters  | Payload specifies which parameters to read. Can be empty to request all.     |
| 0x92      | Device -> PC | Device Parameters Response | Payload contains the requested parameter values.                             |

### (2) Control Commands

| CommandID | Direction    | Meaning                 | Remarks                                                               |
| --------- | ------------ | ----------------------- | --------------------------------------------------------------------- |
| 0x20      | PC -> Device | Start Data Transmission | Commands the device to begin sending data streams (e.g., ADC data).   |
| 0xA0      | Device -> PC | Start Command Response  | Acknowledges the start command, indicating success or failure.        |
| 0x21      | PC -> Device | Stop Data Transmission  | Commands the device to halt data streams.                             |
| 0xA1      | Device -> PC | Stop Command Response   | Acknowledges the stop command.                                        |
| 0x22      | PC -> Device | Device Reset            | Requests a software reset of the device.                              |
| 0xA2      | Device -> PC | Reset Command Response  | Acknowledges the reset command. Usually sent before the reset occurs. |
| 0x2F      | PC -> Device | Ping                    | Used to check if the communication link is active.                    |
| 0xAF      | Device -> PC | Pong (Ping Response)    | The response to a Ping command, confirming the link is active.        |

### (3) Data Transmission Commands

| CommandID | Direction    | Meaning           | Remarks                                                                                                                                                                            |
| --------- | ------------ | ----------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 0x40      | Device -> PC | ADC Data Packet   | Device<br> proactively sends ADC data. The payload will contain data from one or <br>more channels. This command is for streaming and does not require a <br>response from the PC. |
| 0x41      | Device -> PC | Other Sensor Data | Can be used for other types of data packets, like IMU, temperature, etc.                                                                                                           |

### (4) Logging and Debugging Commands

| CommandID | Direction    | Meaning     | Remarks                                                                                                                                                     |
| --------- | ------------ | ----------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 0xE0      | Device -> PC | Log Message | Device<br> sends a log string for debugging or status updates. The payload can <br>contain the log level (e.g., INFO, WARN, ERROR) and the message content. |

## 4. Summary

1. **Protocol Frame Format**:
   - By means of fixed frame head/tail, length field, and checksum, a 
     frame of data can be reliably identified within a continuous byte 
     stream.
2. **CommandID of Instructions/Commands**:
   - Used to distinguish different requests, responses, or data types.
   - The Seq field is employed for matching requests-responses or for frame counting, preventing out-of-order or lost packets.
3. **CRC16**:
   - Although USB CDC or serial ports can ensure the reliability of the 
     physical layer, adding CRC16 at the application layer further guarantees
     the integrity of the frame structure and can detect logical errors 
     (such as buffer overflows, alignment errors, etc.).
   - Here is the function for calculating CRC16 in C language:

c

运行

```c
#include <stdio.h>
#include <stdint.h>

// Function to calculate CRC16
uint16_t CRC16_Calc(const uint8_t* data, uint16_t length, uint16_t initVal)
{
    uint16_t crc = initVal;  // The initial value can usually be set to 0xFFFF, 0x0000, etc.

    for (uint16_t i = 0; i < length; i++) {
        crc ^= (uint16_t)data[i];
        for (uint8_t j = 0; j < 8; j++) {
            if (crc & 0x0001) {
                crc >>= 1;
                crc ^= 0xA001;  // Common polynomial
            }
            else {
                crc >>= 1;
            }
        }
    }
    return crc;
}
```

4. **C Language Example**:
   - The functions buildFrame() and parseFrame() are provided, which can 
     be directly tested on a PC and also transplanted to a dual-end of MCU + 
     PC.
5. **Expansion**:
   - In actual projects, more command words can be defined.
   - The Payload part can add structures or binary data as needed.
   - The CRC algorithm can be replaced with the version existing in the project.
   - If larger data volumes need to be supported, segmented transmission 
     or multi-frame splicing can be implemented at the application layer.

# Implementation of I/O Buffer with Multi-Frame Support

To
 enable continuous sending and receiving of multiple frames, a circular 
queue or FIFO is required to store the original input data and the 
frames to be transmitted. The following presents an example 
implementation, including:

- **Rx Buffer**: A cache for the original input byte 
  stream, supporting the placement of new bytes via the function 
  feedRxBuffer() and the parsing of one or more frames within it using 
  tryParseFramesFromRx().
- **Tx Buffer**: Queues the constructed frames in sequence and retrieves and transmits them in a sending thread or interrupt.

### Summary:

1. The capacity of the circular queue can be increased according to 
   requirements. The RX_BUFFER_SIZE and TX_BUFFER_SIZE need to be set based
   on the actual communication rate and business data volume.
2. If the receiver cannot keep up with the sender, it may lead to a 
   circular queue overflow. Appropriate actions such as packet loss, 
   blocking wait, or other treatments can be performed when detecting 
   insufficient space in feedRxBuffer() / enqueueTxFrame().
3. In the example, when placing multiple frames into the TxBuffer, the 
   method of frameLen + frameData is used for separation; alternatively, 
   the "frame head-frame tail" method can be adopted to store the entire 
   frame in the TxBuffer. The key is to distinguish the boundary of each 
   frame during dequeue.
4. For actual projects, serial port sending and receiving usually occur
   in interrupts or DMA callbacks. Once new data is available, it is fed 
   into the RxBuffer(). The function tryParseFramesFromRx() is called 
   regularly in the main loop or task, and multiple frames may be parsed 
   each time.
5. Similarly, the sending end can also be in an independent task or 
   timer, and each frame is sent to the hardware peripheral based on 
   dequeueTxFrame().
6. Through this approach, multiple frames of data can be stably sent 
   and received, and the single-frame logic can be handled at the 
   application layer using buildFrame() / parseFrame(), with a clear 
   hierarchy and easy extensibility.

## 5. Revision History

- **2024/12/31 by Zhiyuan**:
  - Unified the document format to English.
  - Added the implementation of I/O buffer with multi-frame support.
  - Updated the summary and remarks for better clarity and comprehensiveness.
  - Added the function for CRC16 calculation to illustrate its usage in the protocol.
  - Expanded the Command Definitions section with detailed command categories and descriptions.
```

## 文件: `data-processor/Cargo.lock`

```
# This file is automatically @generated by Cargo.
# It is not intended for manual editing.
version = 4

[[package]]
name = "addr2line"
version = "0.24.2"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "dfbe277e56a376000877090da837660b4427aad530e3028d44e0bffe4f89a1c1"
dependencies = [
 "gimli",
]

[[package]]
name = "adler2"
version = "2.0.1"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "320119579fcad9c21884f5c4861d16174d0e06250625266f50fe6898340abefa"

[[package]]
name = "android-tzdata"
version = "0.1.1"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "e999941b234f3131b00bc13c22d06e8c5ff726d1b6318ac7eb276997bbb4fef0"

[[package]]
name = "android_system_properties"
version = "0.1.5"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "819e7219dbd41043ac279b19830f2efc897156490d7fd6ea916720117ee66311"
dependencies = [
 "libc",
]

[[package]]
name = "anyhow"
version = "1.0.99"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "b0674a1ddeecb70197781e945de4b3b8ffb61fa939a5597bcf48503737663100"

[[package]]
name = "async-trait"
version = "0.1.89"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "9035ad2d096bed7955a320ee7e2230574d28fd3c3a0f186cbea1ff3c7eed5dbb"
dependencies = [
 "proc-macro2",
 "quote",
 "syn",
]

[[package]]
name = "autocfg"
version = "1.5.0"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "c08606f8c3cbf4ce6ec8e28fb0014a2c086708fe954eaa885384a6165172e7e8"

[[package]]
name = "axum"
version = "0.7.9"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "edca88bc138befd0323b20752846e6587272d3b03b0343c8ea28a6f819e6e71f"
dependencies = [
 "async-trait",
 "axum-core",
 "bytes",
 "futures-util",
 "http",
 "http-body",
 "http-body-util",
 "hyper",
 "hyper-util",
 "itoa",
 "matchit",
 "memchr",
 "mime",
 "percent-encoding",
 "pin-project-lite",
 "rustversion",
 "serde",
 "serde_json",
 "serde_path_to_error",
 "serde_urlencoded",
 "sync_wrapper",
 "tokio",
 "tower 0.5.2",
 "tower-layer",
 "tower-service",
 "tracing",
]

[[package]]
name = "axum-core"
version = "0.4.5"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "09f2bd6146b97ae3359fa0cc6d6b376d9539582c7b4220f041a33ec24c226199"
dependencies = [
 "async-trait",
 "bytes",
 "futures-util",
 "http",
 "http-body",
 "http-body-util",
 "mime",
 "pin-project-lite",
 "rustversion",
 "sync_wrapper",
 "tower-layer",
 "tower-service",
 "tracing",
]

[[package]]
name = "backtrace"
version = "0.3.75"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "6806a6321ec58106fea15becdad98371e28d92ccbc7c8f1b3b6dd724fe8f1002"
dependencies = [
 "addr2line",
 "cfg-if",
 "libc",
 "miniz_oxide",
 "object",
 "rustc-demangle",
 "windows-targets 0.52.6",
]

[[package]]
name = "bitflags"
version = "1.3.2"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "bef38d45163c2f1dde094a7dfd33ccf595c92905c8f8f4fdc18d06fb1037718a"

[[package]]
name = "bitflags"
version = "2.9.1"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "1b8e56985ec62d17e9c1001dc89c88ecd7dc08e47eba5ec7c29c7b5eeecde967"

[[package]]
name = "block-buffer"
version = "0.10.4"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "3078c7629b62d3f0439517fa394996acacc5cbc91c5a20d8c658e77abd503a71"
dependencies = [
 "generic-array",
]

[[package]]
name = "bumpalo"
version = "3.19.0"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "46c5e41b57b8bba42a04676d81cb89e9ee8e859a1a66f80a5a72e1cb76b34d43"

[[package]]
name = "byteorder"
version = "1.5.0"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "1fd0f2584146f6f2ef48085050886acf353beff7305ebd1ae69500e27c67f64b"

[[package]]
name = "bytes"
version = "1.10.1"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "d71b6127be86fdcfddb610f7182ac57211d4b18a3e9c82eb2d17662f2227ad6a"

[[package]]
name = "cc"
version = "1.2.32"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "2352e5597e9c544d5e6d9c95190d5d27738ade584fa8db0a16e130e5c2b5296e"
dependencies = [
 "shlex",
]

[[package]]
name = "cfg-if"
version = "1.0.1"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "9555578bc9e57714c812a1f84e4fc5b4d21fcb063490c624de019f7464c91268"

[[package]]
name = "chrono"
version = "0.4.41"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "c469d952047f47f91b68d1cba3f10d63c11d73e4636f24f08daf0278abf01c4d"
dependencies = [
 "android-tzdata",
 "iana-time-zone",
 "js-sys",
 "num-traits",
 "serde",
 "wasm-bindgen",
 "windows-link",
]

[[package]]
name = "core-foundation-sys"
version = "0.8.7"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "773648b94d0e5d620f64f280777445740e61fe701025087ec8b57f45c791888b"

[[package]]
name = "cpufeatures"
version = "0.2.17"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "59ed5838eebb26a2bb2e58f6d5b5316989ae9d08bab10e0e6d103e656d1b0280"
dependencies = [
 "libc",
]

[[package]]
name = "crypto-common"
version = "0.1.6"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "1bfb12502f3fc46cca1bb51ac28df9d618d813cdc3d2f25b9fe775a34af26bb3"
dependencies = [
 "generic-array",
 "typenum",
]

[[package]]
name = "data-encoding"
version = "2.9.0"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "2a2330da5de22e8a3cb63252ce2abb30116bf5265e89c0e01bc17015ce30a476"

[[package]]
name = "data-processor"
version = "0.1.0"
dependencies = [
 "anyhow",
 "async-trait",
 "axum",
 "chrono",
 "futures-util",
 "hyper",
 "mio 0.8.11",
 "rustfft",
 "serde",
 "serde_json",
 "shared_memory",
 "tempfile",
 "thiserror",
 "tokio",
 "tokio-tungstenite",
 "tower 0.4.13",
 "tower-http",
 "tracing",
 "tracing-subscriber",
 "uuid",
 "winapi",
]

[[package]]
name = "digest"
version = "0.10.7"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "9ed9a281f7bc9b7576e61468ba615a66a5c8cfdff42420a70aa82701a3b1e292"
dependencies = [
 "block-buffer",
 "crypto-common",
]

[[package]]
name = "displaydoc"
version = "0.2.5"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "97369cbbc041bc366949bc74d34658d6cda5621039731c6310521892a3a20ae0"
dependencies = [
 "proc-macro2",
 "quote",
 "syn",
]

[[package]]
name = "errno"
version = "0.3.13"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "778e2ac28f6c47af28e4907f13ffd1e1ddbd400980a9abd7c8df189bf578a5ad"
dependencies = [
 "libc",
 "windows-sys 0.60.2",
]

[[package]]
name = "fastrand"
version = "2.3.0"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "37909eebbb50d72f9059c3b6d82c0463f2ff062c9e95845c43a6c9c0355411be"

[[package]]
name = "fnv"
version = "1.0.7"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "3f9eec918d3f24069decb9af1554cad7c880e2da24a9afd88aca000531ab82c1"

[[package]]
name = "form_urlencoded"
version = "1.2.1"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "e13624c2627564efccf4934284bdd98cbaa14e79b0b5a141218e507b3a823456"
dependencies = [
 "percent-encoding",
]

[[package]]
name = "futures-channel"
version = "0.3.31"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "2dff15bf788c671c1934e366d07e30c1814a8ef514e1af724a602e8a2fbe1b10"
dependencies = [
 "futures-core",
]

[[package]]
name = "futures-core"
version = "0.3.31"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "05f29059c0c2090612e8d742178b0580d2dc940c837851ad723096f87af6663e"

[[package]]
name = "futures-macro"
version = "0.3.31"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "162ee34ebcb7c64a8abebc059ce0fee27c2262618d7b60ed8faf72fef13c3650"
dependencies = [
 "proc-macro2",
 "quote",
 "syn",
]

[[package]]
name = "futures-sink"
version = "0.3.31"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "e575fab7d1e0dcb8d0c7bcf9a63ee213816ab51902e6d244a95819acacf1d4f7"

[[package]]
name = "futures-task"
version = "0.3.31"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "f90f7dce0722e95104fcb095585910c0977252f286e354b5e3bd38902cd99988"

[[package]]
name = "futures-util"
version = "0.3.31"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "9fa08315bb612088cc391249efdc3bc77536f16c91f6cf495e6fbe85b20a4a81"
dependencies = [
 "futures-core",
 "futures-macro",
 "futures-sink",
 "futures-task",
 "pin-project-lite",
 "pin-utils",
 "slab",
]

[[package]]
name = "generic-array"
version = "0.14.7"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "85649ca51fd72272d7821adaf274ad91c288277713d9c18820d8499a7ff69e9a"
dependencies = [
 "typenum",
 "version_check",
]

[[package]]
name = "getrandom"
version = "0.2.16"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "335ff9f135e4384c8150d6f27c6daed433577f86b4750418338c01a1a2528592"
dependencies = [
 "cfg-if",
 "libc",
 "wasi 0.11.1+wasi-snapshot-preview1",
]

[[package]]
name = "getrandom"
version = "0.3.3"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "26145e563e54f2cadc477553f1ec5ee650b00862f0a58bcd12cbdc5f0ea2d2f4"
dependencies = [
 "cfg-if",
 "libc",
 "r-efi",
 "wasi 0.14.2+wasi-0.2.4",
]

[[package]]
name = "gimli"
version = "0.31.1"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "07e28edb80900c19c28f1072f2e8aeca7fa06b23cd4169cefe1af5aa3260783f"

[[package]]
name = "http"
version = "1.3.1"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "f4a85d31aea989eead29a3aaf9e1115a180df8282431156e533de47660892565"
dependencies = [
 "bytes",
 "fnv",
 "itoa",
]

[[package]]
name = "http-body"
version = "1.0.1"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "1efedce1fb8e6913f23e0c92de8e62cd5b772a67e7b3946df930a62566c93184"
dependencies = [
 "bytes",
 "http",
]

[[package]]
name = "http-body-util"
version = "0.1.3"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "b021d93e26becf5dc7e1b75b1bed1fd93124b374ceb73f43d4d4eafec896a64a"
dependencies = [
 "bytes",
 "futures-core",
 "http",
 "http-body",
 "pin-project-lite",
]

[[package]]
name = "http-range-header"
version = "0.4.2"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "9171a2ea8a68358193d15dd5d70c1c10a2afc3e7e4c5bc92bc9f025cebd7359c"

[[package]]
name = "httparse"
version = "1.10.1"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "6dbf3de79e51f3d586ab4cb9d5c3e2c14aa28ed23d180cf89b4df0454a69cc87"

[[package]]
name = "httpdate"
version = "1.0.3"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "df3b46402a9d5adb4c86a0cf463f42e19994e3ee891101b1841f30a545cb49a9"

[[package]]
name = "hyper"
version = "1.6.0"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "cc2b571658e38e0c01b1fdca3bbbe93c00d3d71693ff2770043f8c29bc7d6f80"
dependencies = [
 "bytes",
 "futures-channel",
 "futures-util",
 "http",
 "http-body",
 "httparse",
 "httpdate",
 "itoa",
 "pin-project-lite",
 "smallvec",
 "tokio",
]

[[package]]
name = "hyper-util"
version = "0.1.16"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "8d9b05277c7e8da2c93a568989bb6207bef0112e8d17df7a6eda4a3cf143bc5e"
dependencies = [
 "bytes",
 "futures-core",
 "http",
 "http-body",
 "hyper",
 "pin-project-lite",
 "tokio",
 "tower-service",
]

[[package]]
name = "iana-time-zone"
version = "0.1.63"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "b0c919e5debc312ad217002b8048a17b7d83f80703865bbfcfebb0458b0b27d8"
dependencies = [
 "android_system_properties",
 "core-foundation-sys",
 "iana-time-zone-haiku",
 "js-sys",
 "log",
 "wasm-bindgen",
 "windows-core",
]

[[package]]
name = "iana-time-zone-haiku"
version = "0.1.2"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "f31827a206f56af32e590ba56d5d2d085f558508192593743f16b2306495269f"
dependencies = [
 "cc",
]

[[package]]
name = "icu_collections"
version = "2.0.0"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "200072f5d0e3614556f94a9930d5dc3e0662a652823904c3a75dc3b0af7fee47"
dependencies = [
 "displaydoc",
 "potential_utf",
 "yoke",
 "zerofrom",
 "zerovec",
]

[[package]]
name = "icu_locale_core"
version = "2.0.0"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "0cde2700ccaed3872079a65fb1a78f6c0a36c91570f28755dda67bc8f7d9f00a"
dependencies = [
 "displaydoc",
 "litemap",
 "tinystr",
 "writeable",
 "zerovec",
]

[[package]]
name = "icu_normalizer"
version = "2.0.0"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "436880e8e18df4d7bbc06d58432329d6458cc84531f7ac5f024e93deadb37979"
dependencies = [
 "displaydoc",
 "icu_collections",
 "icu_normalizer_data",
 "icu_properties",
 "icu_provider",
 "smallvec",
 "zerovec",
]

[[package]]
name = "icu_normalizer_data"
version = "2.0.0"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "00210d6893afc98edb752b664b8890f0ef174c8adbb8d0be9710fa66fbbf72d3"

[[package]]
name = "icu_properties"
version = "2.0.1"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "016c619c1eeb94efb86809b015c58f479963de65bdb6253345c1a1276f22e32b"
dependencies = [
 "displaydoc",
 "icu_collections",
 "icu_locale_core",
 "icu_properties_data",
 "icu_provider",
 "potential_utf",
 "zerotrie",
 "zerovec",
]

[[package]]
name = "icu_properties_data"
version = "2.0.1"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "298459143998310acd25ffe6810ed544932242d3f07083eee1084d83a71bd632"

[[package]]
name = "icu_provider"
version = "2.0.0"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "03c80da27b5f4187909049ee2d72f276f0d9f99a42c306bd0131ecfe04d8e5af"
dependencies = [
 "displaydoc",
 "icu_locale_core",
 "stable_deref_trait",
 "tinystr",
 "writeable",
 "yoke",
 "zerofrom",
 "zerotrie",
 "zerovec",
]

[[package]]
name = "idna"
version = "1.0.3"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "686f825264d630750a544639377bae737628043f20d38bbc029e8f29ea968a7e"
dependencies = [
 "idna_adapter",
 "smallvec",
 "utf8_iter",
]

[[package]]
name = "idna_adapter"
version = "1.2.1"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "3acae9609540aa318d1bc588455225fb2085b9ed0c4f6bd0d9d5bcd86f1a0344"
dependencies = [
 "icu_normalizer",
 "icu_properties",
]

[[package]]
name = "io-uring"
version = "0.7.9"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "d93587f37623a1a17d94ef2bc9ada592f5465fe7732084ab7beefabe5c77c0c4"
dependencies = [
 "bitflags 2.9.1",
 "cfg-if",
 "libc",
]

[[package]]
name = "itoa"
version = "1.0.15"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "4a5f13b858c8d314ee3e8f639011f7ccefe71f97f96e50151fb991f267928e2c"

[[package]]
name = "js-sys"
version = "0.3.77"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "1cfaf33c695fc6e08064efbc1f72ec937429614f25eef83af942d0e227c3a28f"
dependencies = [
 "once_cell",
 "wasm-bindgen",
]

[[package]]
name = "lazy_static"
version = "1.5.0"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "bbd2bcb4c963f2ddae06a2efc7e9f3591312473c50c6685e1f298068316e66fe"

[[package]]
name = "libc"
version = "0.2.175"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "6a82ae493e598baaea5209805c49bbf2ea7de956d50d7da0da1164f9c6d28543"

[[package]]
name = "linux-raw-sys"
version = "0.9.4"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "cd945864f07fe9f5371a27ad7b52a172b4b499999f1d97574c9fa68373937e12"

[[package]]
name = "litemap"
version = "0.8.0"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "241eaef5fd12c88705a01fc1066c48c4b36e0dd4377dcdc7ec3942cea7a69956"

[[package]]
name = "lock_api"
version = "0.4.13"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "96936507f153605bddfcda068dd804796c84324ed2510809e5b2a624c81da765"
dependencies = [
 "autocfg",
 "scopeguard",
]

[[package]]
name = "log"
version = "0.4.27"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "13dc2df351e3202783a1fe0d44375f7295ffb4049267b0f3018346dc122a1d94"

[[package]]
name = "matchit"
version = "0.7.3"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "0e7465ac9959cc2b1404e8e2367b43684a6d13790fe23056cc8c6c5a6b7bcb94"

[[package]]
name = "memchr"
version = "2.7.5"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "32a282da65faaf38286cf3be983213fcf1d2e2a58700e808f83f4ea9a4804bc0"

[[package]]
name = "memoffset"
version = "0.6.5"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "5aa361d4faea93603064a027415f07bd8e1d5c88c9fbf68bf56a285428fd79ce"
dependencies = [
 "autocfg",
]

[[package]]
name = "mime"
version = "0.3.17"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "6877bb514081ee2a7ff5ef9de3281f14a4dd4bceac4c09388074a6b5df8a139a"

[[package]]
name = "mime_guess"
version = "2.0.5"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "f7c44f8e672c00fe5308fa235f821cb4198414e1c77935c1ab6948d3fd78550e"
dependencies = [
 "mime",
 "unicase",
]

[[package]]
name = "miniz_oxide"
version = "0.8.9"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "1fa76a2c86f704bdb222d66965fb3d63269ce38518b83cb0575fca855ebb6316"
dependencies = [
 "adler2",
]

[[package]]
name = "mio"
version = "0.8.11"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "a4a650543ca06a924e8b371db273b2756685faae30f8487da1b56505a8f78b0c"
dependencies = [
 "libc",
 "log",
 "wasi 0.11.1+wasi-snapshot-preview1",
 "windows-sys 0.48.0",
]

[[package]]
name = "mio"
version = "1.0.4"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "78bed444cc8a2160f01cbcf811ef18cac863ad68ae8ca62092e8db51d51c761c"
dependencies = [
 "libc",
 "wasi 0.11.1+wasi-snapshot-preview1",
 "windows-sys 0.59.0",
]

[[package]]
name = "nix"
version = "0.23.2"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "8f3790c00a0150112de0f4cd161e3d7fc4b2d8a5542ffc35f099a2562aecb35c"
dependencies = [
 "bitflags 1.3.2",
 "cc",
 "cfg-if",
 "libc",
 "memoffset",
]

[[package]]
name = "nu-ansi-term"
version = "0.46.0"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "77a8165726e8236064dbb45459242600304b42a5ea24ee2948e18e023bf7ba84"
dependencies = [
 "overload",
 "winapi",
]

[[package]]
name = "num-complex"
version = "0.4.6"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "73f88a1307638156682bada9d7604135552957b7818057dcef22705b4d509495"
dependencies = [
 "num-traits",
]

[[package]]
name = "num-integer"
version = "0.1.46"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "7969661fd2958a5cb096e56c8e1ad0444ac2bbcd0061bd28660485a44879858f"
dependencies = [
 "num-traits",
]

[[package]]
name = "num-traits"
version = "0.2.19"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "071dfc062690e90b734c0b2273ce72ad0ffa95f0c74596bc250dcfd960262841"
dependencies = [
 "autocfg",
]

[[package]]
name = "object"
version = "0.36.7"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "62948e14d923ea95ea2c7c86c71013138b66525b86bdc08d2dcc262bdb497b87"
dependencies = [
 "memchr",
]

[[package]]
name = "once_cell"
version = "1.21.3"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "42f5e15c9953c5e4ccceeb2e7382a716482c34515315f7b03532b8b4e8393d2d"

[[package]]
name = "overload"
version = "0.1.1"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "b15813163c1d831bf4a13c3610c05c0d03b39feb07f7e09fa234dac9b15aaf39"

[[package]]
name = "parking_lot"
version = "0.12.4"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "70d58bf43669b5795d1576d0641cfb6fbb2057bf629506267a92807158584a13"
dependencies = [
 "lock_api",
 "parking_lot_core",
]

[[package]]
name = "parking_lot_core"
version = "0.9.11"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "bc838d2a56b5b1a6c25f55575dfc605fabb63bb2365f6c2353ef9159aa69e4a5"
dependencies = [
 "cfg-if",
 "libc",
 "redox_syscall",
 "smallvec",
 "windows-targets 0.52.6",
]

[[package]]
name = "percent-encoding"
version = "2.3.1"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "e3148f5046208a5d56bcfc03053e3ca6334e51da8dfb19b6cdc8b306fae3283e"

[[package]]
name = "pin-project-lite"
version = "0.2.16"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "3b3cff922bd51709b605d9ead9aa71031d81447142d828eb4a6eba76fe619f9b"

[[package]]
name = "pin-utils"
version = "0.1.0"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "8b870d8c151b6f2fb93e84a13146138f05d02ed11c7e7c54f8826aaaf7c9f184"

[[package]]
name = "potential_utf"
version = "0.1.2"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "e5a7c30837279ca13e7c867e9e40053bc68740f988cb07f7ca6df43cc734b585"
dependencies = [
 "zerovec",
]

[[package]]
name = "ppv-lite86"
version = "0.2.21"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "85eae3c4ed2f50dcfe72643da4befc30deadb458a9b590d720cde2f2b1e97da9"
dependencies = [
 "zerocopy",
]

[[package]]
name = "primal-check"
version = "0.3.4"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "dc0d895b311e3af9902528fbb8f928688abbd95872819320517cc24ca6b2bd08"
dependencies = [
 "num-integer",
]

[[package]]
name = "proc-macro2"
version = "1.0.97"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "d61789d7719defeb74ea5fe81f2fdfdbd28a803847077cecce2ff14e1472f6f1"
dependencies = [
 "unicode-ident",
]

[[package]]
name = "quote"
version = "1.0.40"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "1885c039570dc00dcb4ff087a89e185fd56bae234ddc7f056a945bf36467248d"
dependencies = [
 "proc-macro2",
]

[[package]]
name = "r-efi"
version = "5.3.0"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "69cdb34c158ceb288df11e18b4bd39de994f6657d83847bdffdbd7f346754b0f"

[[package]]
name = "rand"
version = "0.8.5"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "34af8d1a0e25924bc5b7c43c079c942339d8f0a8b57c39049bef581b46327404"
dependencies = [
 "libc",
 "rand_chacha",
 "rand_core",
]

[[package]]
name = "rand_chacha"
version = "0.3.1"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "e6c10a63a0fa32252be49d21e7709d4d4baf8d231c2dbce1eaa8141b9b127d88"
dependencies = [
 "ppv-lite86",
 "rand_core",
]

[[package]]
name = "rand_core"
version = "0.6.4"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "ec0be4795e2f6a28069bec0b5ff3e2ac9bafc99e6a9a7dc3547996c5c816922c"
dependencies = [
 "getrandom 0.2.16",
]

[[package]]
name = "redox_syscall"
version = "0.5.17"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "5407465600fb0548f1442edf71dd20683c6ed326200ace4b1ef0763521bb3b77"
dependencies = [
 "bitflags 2.9.1",
]

[[package]]
name = "rustc-demangle"
version = "0.1.26"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "56f7d92ca342cea22a06f2121d944b4fd82af56988c270852495420f961d4ace"

[[package]]
name = "rustfft"
version = "6.4.0"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "c6f140db74548f7c9d7cce60912c9ac414e74df5e718dc947d514b051b42f3f4"
dependencies = [
 "num-complex",
 "num-integer",
 "num-traits",
 "primal-check",
 "strength_reduce",
 "transpose",
]

[[package]]
name = "rustix"
version = "1.0.8"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "11181fbabf243db407ef8df94a6ce0b2f9a733bd8be4ad02b4eda9602296cac8"
dependencies = [
 "bitflags 2.9.1",
 "errno",
 "libc",
 "linux-raw-sys",
 "windows-sys 0.60.2",
]

[[package]]
name = "rustversion"
version = "1.0.22"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "b39cdef0fa800fc44525c84ccb54a029961a8215f9619753635a9c0d2538d46d"

[[package]]
name = "ryu"
version = "1.0.20"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "28d3b2b1366ec20994f1fd18c3c594f05c5dd4bc44d8bb0c1c632c8d6829481f"

[[package]]
name = "scopeguard"
version = "1.2.0"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "94143f37725109f92c262ed2cf5e59bce7498c01bcc1502d7b9afe439a4e9f49"

[[package]]
name = "serde"
version = "1.0.219"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "5f0e2c6ed6606019b4e29e69dbaba95b11854410e5347d525002456dbbb786b6"
dependencies = [
 "serde_derive",
]

[[package]]
name = "serde_derive"
version = "1.0.219"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "5b0276cf7f2c73365f7157c8123c21cd9a50fbbd844757af28ca1f5925fc2a00"
dependencies = [
 "proc-macro2",
 "quote",
 "syn",
]

[[package]]
name = "serde_json"
version = "1.0.142"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "030fedb782600dcbd6f02d479bf0d817ac3bb40d644745b769d6a96bc3afc5a7"
dependencies = [
 "itoa",
 "memchr",
 "ryu",
 "serde",
]

[[package]]
name = "serde_path_to_error"
version = "0.1.17"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "59fab13f937fa393d08645bf3a84bdfe86e296747b506ada67bb15f10f218b2a"
dependencies = [
 "itoa",
 "serde",
]

[[package]]
name = "serde_urlencoded"
version = "0.7.1"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "d3491c14715ca2294c4d6a88f15e84739788c1d030eed8c110436aafdaa2f3fd"
dependencies = [
 "form_urlencoded",
 "itoa",
 "ryu",
 "serde",
]

[[package]]
name = "sha1"
version = "0.10.6"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "e3bf829a2d51ab4a5ddf1352d8470c140cadc8301b2ae1789db023f01cedd6ba"
dependencies = [
 "cfg-if",
 "cpufeatures",
 "digest",
]

[[package]]
name = "sharded-slab"
version = "0.1.7"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "f40ca3c46823713e0d4209592e8d6e826aa57e928f09752619fc696c499637f6"
dependencies = [
 "lazy_static",
]

[[package]]
name = "shared_memory"
version = "0.12.4"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "ba8593196da75d9dc4f69349682bd4c2099f8cde114257d1ef7ef1b33d1aba54"
dependencies = [
 "cfg-if",
 "libc",
 "nix",
 "rand",
 "win-sys",
]

[[package]]
name = "shlex"
version = "1.3.0"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "0fda2ff0d084019ba4d7c6f371c95d8fd75ce3524c3cb8fb653a3023f6323e64"

[[package]]
name = "signal-hook-registry"
version = "1.4.6"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "b2a4719bff48cee6b39d12c020eeb490953ad2443b7055bd0b21fca26bd8c28b"
dependencies = [
 "libc",
]

[[package]]
name = "slab"
version = "0.4.11"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "7a2ae44ef20feb57a68b23d846850f861394c2e02dc425a50098ae8c90267589"

[[package]]
name = "smallvec"
version = "1.15.1"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "67b1b7a3b5fe4f1376887184045fcf45c69e92af734b7aaddc05fb777b6fbd03"

[[package]]
name = "socket2"
version = "0.6.0"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "233504af464074f9d066d7b5416c5f9b894a5862a6506e306f7b816cdd6f1807"
dependencies = [
 "libc",
 "windows-sys 0.59.0",
]

[[package]]
name = "stable_deref_trait"
version = "1.2.0"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "a8f112729512f8e442d81f95a8a7ddf2b7c6b8a1a6f509a95864142b30cab2d3"

[[package]]
name = "strength_reduce"
version = "0.2.4"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "fe895eb47f22e2ddd4dabc02bce419d2e643c8e3b585c78158b349195bc24d82"

[[package]]
name = "syn"
version = "2.0.105"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "7bc3fcb250e53458e712715cf74285c1f889686520d79294a9ef3bd7aa1fc619"
dependencies = [
 "proc-macro2",
 "quote",
 "unicode-ident",
]

[[package]]
name = "sync_wrapper"
version = "1.0.2"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "0bf256ce5efdfa370213c1dabab5935a12e49f2c58d15e9eac2870d3b4f27263"

[[package]]
name = "synstructure"
version = "0.13.2"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "728a70f3dbaf5bab7f0c4b1ac8d7ae5ea60a4b5549c8a5914361c99147a709d2"
dependencies = [
 "proc-macro2",
 "quote",
 "syn",
]

[[package]]
name = "tempfile"
version = "3.20.0"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "e8a64e3985349f2441a1a9ef0b853f869006c3855f2cda6862a94d26ebb9d6a1"
dependencies = [
 "fastrand",
 "getrandom 0.3.3",
 "once_cell",
 "rustix",
 "windows-sys 0.59.0",
]

[[package]]
name = "thiserror"
version = "1.0.69"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "b6aaf5339b578ea85b50e080feb250a3e8ae8cfcdff9a461c9ec2904bc923f52"
dependencies = [
 "thiserror-impl",
]

[[package]]
name = "thiserror-impl"
version = "1.0.69"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "4fee6c4efc90059e10f81e6d42c60a18f76588c3d74cb83a0b242a2b6c7504c1"
dependencies = [
 "proc-macro2",
 "quote",
 "syn",
]

[[package]]
name = "thread_local"
version = "1.1.9"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "f60246a4944f24f6e018aa17cdeffb7818b76356965d03b07d6a9886e8962185"
dependencies = [
 "cfg-if",
]

[[package]]
name = "tinystr"
version = "0.8.1"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "5d4f6d1145dcb577acf783d4e601bc1d76a13337bb54e6233add580b07344c8b"
dependencies = [
 "displaydoc",
 "zerovec",
]

[[package]]
name = "tokio"
version = "1.47.1"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "89e49afdadebb872d3145a5638b59eb0691ea23e46ca484037cfab3b76b95038"
dependencies = [
 "backtrace",
 "bytes",
 "io-uring",
 "libc",
 "mio 1.0.4",
 "parking_lot",
 "pin-project-lite",
 "signal-hook-registry",
 "slab",
 "socket2",
 "tokio-macros",
 "windows-sys 0.59.0",
]

[[package]]
name = "tokio-macros"
version = "2.5.0"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "6e06d43f1345a3bcd39f6a56dbb7dcab2ba47e68e8ac134855e7e2bdbaf8cab8"
dependencies = [
 "proc-macro2",
 "quote",
 "syn",
]

[[package]]
name = "tokio-tungstenite"
version = "0.21.0"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "c83b561d025642014097b66e6c1bb422783339e0909e4429cde4749d1990bc38"
dependencies = [
 "futures-util",
 "log",
 "tokio",
 "tungstenite",
]

[[package]]
name = "tokio-util"
version = "0.7.16"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "14307c986784f72ef81c89db7d9e28d6ac26d16213b109ea501696195e6e3ce5"
dependencies = [
 "bytes",
 "futures-core",
 "futures-sink",
 "pin-project-lite",
 "tokio",
]

[[package]]
name = "tower"
version = "0.4.13"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "b8fa9be0de6cf49e536ce1851f987bd21a43b771b09473c3549a6c853db37c1c"
dependencies = [
 "tower-layer",
 "tower-service",
 "tracing",
]

[[package]]
name = "tower"
version = "0.5.2"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "d039ad9159c98b70ecfd540b2573b97f7f52c3e8d9f8ad57a24b916a536975f9"
dependencies = [
 "futures-core",
 "futures-util",
 "pin-project-lite",
 "sync_wrapper",
 "tokio",
 "tower-layer",
 "tower-service",
 "tracing",
]

[[package]]
name = "tower-http"
version = "0.5.2"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "1e9cd434a998747dd2c4276bc96ee2e0c7a2eadf3cae88e52be55a05fa9053f5"
dependencies = [
 "bitflags 2.9.1",
 "bytes",
 "futures-util",
 "http",
 "http-body",
 "http-body-util",
 "http-range-header",
 "httpdate",
 "mime",
 "mime_guess",
 "percent-encoding",
 "pin-project-lite",
 "tokio",
 "tokio-util",
 "tower-layer",
 "tower-service",
 "tracing",
]

[[package]]
name = "tower-layer"
version = "0.3.3"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "121c2a6cda46980bb0fcd1647ffaf6cd3fc79a013de288782836f6df9c48780e"

[[package]]
name = "tower-service"
version = "0.3.3"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "8df9b6e13f2d32c91b9bd719c00d1958837bc7dec474d94952798cc8e69eeec3"

[[package]]
name = "tracing"
version = "0.1.41"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "784e0ac535deb450455cbfa28a6f0df145ea1bb7ae51b821cf5e7927fdcfbdd0"
dependencies = [
 "log",
 "pin-project-lite",
 "tracing-attributes",
 "tracing-core",
]

[[package]]
name = "tracing-attributes"
version = "0.1.30"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "81383ab64e72a7a8b8e13130c49e3dab29def6d0c7d76a03087b3cf71c5c6903"
dependencies = [
 "proc-macro2",
 "quote",
 "syn",
]

[[package]]
name = "tracing-core"
version = "0.1.34"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "b9d12581f227e93f094d3af2ae690a574abb8a2b9b7a96e7cfe9647b2b617678"
dependencies = [
 "once_cell",
 "valuable",
]

[[package]]
name = "tracing-log"
version = "0.2.0"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "ee855f1f400bd0e5c02d150ae5de3840039a3f54b025156404e34c23c03f47c3"
dependencies = [
 "log",
 "once_cell",
 "tracing-core",
]

[[package]]
name = "tracing-subscriber"
version = "0.3.19"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "e8189decb5ac0fa7bc8b96b7cb9b2701d60d48805aca84a238004d665fcc4008"
dependencies = [
 "nu-ansi-term",
 "sharded-slab",
 "smallvec",
 "thread_local",
 "tracing-core",
 "tracing-log",
]

[[package]]
name = "transpose"
version = "0.2.3"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "1ad61aed86bc3faea4300c7aee358b4c6d0c8d6ccc36524c96e4c92ccf26e77e"
dependencies = [
 "num-integer",
 "strength_reduce",
]

[[package]]
name = "tungstenite"
version = "0.21.0"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "9ef1a641ea34f399a848dea702823bbecfb4c486f911735368f1f137cb8257e1"
dependencies = [
 "byteorder",
 "bytes",
 "data-encoding",
 "http",
 "httparse",
 "log",
 "rand",
 "sha1",
 "thiserror",
 "url",
 "utf-8",
]

[[package]]
name = "typenum"
version = "1.18.0"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "1dccffe3ce07af9386bfd29e80c0ab1a8205a2fc34e4bcd40364df902cfa8f3f"

[[package]]
name = "unicase"
version = "2.8.1"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "75b844d17643ee918803943289730bec8aac480150456169e647ed0b576ba539"

[[package]]
name = "unicode-ident"
version = "1.0.18"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "5a5f39404a5da50712a4c1eecf25e90dd62b613502b7e925fd4e4d19b5c96512"

[[package]]
name = "url"
version = "2.5.4"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "32f8b686cadd1473f4bd0117a5d28d36b1ade384ea9b5069a1c40aefed7fda60"
dependencies = [
 "form_urlencoded",
 "idna",
 "percent-encoding",
]

[[package]]
name = "utf-8"
version = "0.7.6"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "09cc8ee72d2a9becf2f2febe0205bbed8fc6615b7cb429ad062dc7b7ddd036a9"

[[package]]
name = "utf8_iter"
version = "1.0.4"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "b6c140620e7ffbb22c2dee59cafe6084a59b5ffc27a8859a5f0d494b5d52b6be"

[[package]]
name = "uuid"
version = "1.18.0"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "f33196643e165781c20a5ead5582283a7dacbb87855d867fbc2df3f81eddc1be"
dependencies = [
 "getrandom 0.3.3",
 "js-sys",
 "wasm-bindgen",
]

[[package]]
name = "valuable"
version = "0.1.1"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "ba73ea9cf16a25df0c8caa16c51acb937d5712a8429db78a3ee29d5dcacd3a65"

[[package]]
name = "version_check"
version = "0.9.5"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "0b928f33d975fc6ad9f86c8f283853ad26bdd5b10b7f1542aa2fa15e2289105a"

[[package]]
name = "wasi"
version = "0.11.1+wasi-snapshot-preview1"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "ccf3ec651a847eb01de73ccad15eb7d99f80485de043efb2f370cd654f4ea44b"

[[package]]
name = "wasi"
version = "0.14.2+wasi-0.2.4"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "9683f9a5a998d873c0d21fcbe3c083009670149a8fab228644b8bd36b2c48cb3"
dependencies = [
 "wit-bindgen-rt",
]

[[package]]
name = "wasm-bindgen"
version = "0.2.100"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "1edc8929d7499fc4e8f0be2262a241556cfc54a0bea223790e71446f2aab1ef5"
dependencies = [
 "cfg-if",
 "once_cell",
 "rustversion",
 "wasm-bindgen-macro",
]

[[package]]
name = "wasm-bindgen-backend"
version = "0.2.100"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "2f0a0651a5c2bc21487bde11ee802ccaf4c51935d0d3d42a6101f98161700bc6"
dependencies = [
 "bumpalo",
 "log",
 "proc-macro2",
 "quote",
 "syn",
 "wasm-bindgen-shared",
]

[[package]]
name = "wasm-bindgen-macro"
version = "0.2.100"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "7fe63fc6d09ed3792bd0897b314f53de8e16568c2b3f7982f468c0bf9bd0b407"
dependencies = [
 "quote",
 "wasm-bindgen-macro-support",
]

[[package]]
name = "wasm-bindgen-macro-support"
version = "0.2.100"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "8ae87ea40c9f689fc23f209965b6fb8a99ad69aeeb0231408be24920604395de"
dependencies = [
 "proc-macro2",
 "quote",
 "syn",
 "wasm-bindgen-backend",
 "wasm-bindgen-shared",
]

[[package]]
name = "wasm-bindgen-shared"
version = "0.2.100"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "1a05d73b933a847d6cccdda8f838a22ff101ad9bf93e33684f39c1f5f0eece3d"
dependencies = [
 "unicode-ident",
]

[[package]]
name = "win-sys"
version = "0.3.1"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "5b7b128a98c1cfa201b09eb49ba285887deb3cbe7466a98850eb1adabb452be5"
dependencies = [
 "windows",
]

[[package]]
name = "winapi"
version = "0.3.9"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "5c839a674fcd7a98952e593242ea400abe93992746761e38641405d28b00f419"
dependencies = [
 "winapi-i686-pc-windows-gnu",
 "winapi-x86_64-pc-windows-gnu",
]

[[package]]
name = "winapi-i686-pc-windows-gnu"
version = "0.4.0"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "ac3b87c63620426dd9b991e5ce0329eff545bccbbb34f3be09ff6fb6ab51b7b6"

[[package]]
name = "winapi-x86_64-pc-windows-gnu"
version = "0.4.0"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "712e227841d057c1ee1cd2fb22fa7e5a5461ae8e48fa2ca79ec42cfc1931183f"

[[package]]
name = "windows"
version = "0.34.0"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "45296b64204227616fdbf2614cefa4c236b98ee64dfaaaa435207ed99fe7829f"
dependencies = [
 "windows_aarch64_msvc 0.34.0",
 "windows_i686_gnu 0.34.0",
 "windows_i686_msvc 0.34.0",
 "windows_x86_64_gnu 0.34.0",
 "windows_x86_64_msvc 0.34.0",
]

[[package]]
name = "windows-core"
version = "0.61.2"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "c0fdd3ddb90610c7638aa2b3a3ab2904fb9e5cdbecc643ddb3647212781c4ae3"
dependencies = [
 "windows-implement",
 "windows-interface",
 "windows-link",
 "windows-result",
 "windows-strings",
]

[[package]]
name = "windows-implement"
version = "0.60.0"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "a47fddd13af08290e67f4acabf4b459f647552718f683a7b415d290ac744a836"
dependencies = [
 "proc-macro2",
 "quote",
 "syn",
]

[[package]]
name = "windows-interface"
version = "0.59.1"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "bd9211b69f8dcdfa817bfd14bf1c97c9188afa36f4750130fcdf3f400eca9fa8"
dependencies = [
 "proc-macro2",
 "quote",
 "syn",
]

[[package]]
name = "windows-link"
version = "0.1.3"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "5e6ad25900d524eaabdbbb96d20b4311e1e7ae1699af4fb28c17ae66c80d798a"

[[package]]
name = "windows-result"
version = "0.3.4"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "56f42bd332cc6c8eac5af113fc0c1fd6a8fd2aa08a0119358686e5160d0586c6"
dependencies = [
 "windows-link",
]

[[package]]
name = "windows-strings"
version = "0.4.2"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "56e6c93f3a0c3b36176cb1327a4958a0353d5d166c2a35cb268ace15e91d3b57"
dependencies = [
 "windows-link",
]

[[package]]
name = "windows-sys"
version = "0.48.0"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "677d2418bec65e3338edb076e806bc1ec15693c5d0104683f2efe857f61056a9"
dependencies = [
 "windows-targets 0.48.5",
]

[[package]]
name = "windows-sys"
version = "0.59.0"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "1e38bc4d79ed67fd075bcc251a1c39b32a1776bbe92e5bef1f0bf1f8c531853b"
dependencies = [
 "windows-targets 0.52.6",
]

[[package]]
name = "windows-sys"
version = "0.60.2"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "f2f500e4d28234f72040990ec9d39e3a6b950f9f22d3dba18416c35882612bcb"
dependencies = [
 "windows-targets 0.53.3",
]

[[package]]
name = "windows-targets"
version = "0.48.5"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "9a2fa6e2155d7247be68c096456083145c183cbbbc2764150dda45a87197940c"
dependencies = [
 "windows_aarch64_gnullvm 0.48.5",
 "windows_aarch64_msvc 0.48.5",
 "windows_i686_gnu 0.48.5",
 "windows_i686_msvc 0.48.5",
 "windows_x86_64_gnu 0.48.5",
 "windows_x86_64_gnullvm 0.48.5",
 "windows_x86_64_msvc 0.48.5",
]

[[package]]
name = "windows-targets"
version = "0.52.6"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "9b724f72796e036ab90c1021d4780d4d3d648aca59e491e6b98e725b84e99973"
dependencies = [
 "windows_aarch64_gnullvm 0.52.6",
 "windows_aarch64_msvc 0.52.6",
 "windows_i686_gnu 0.52.6",
 "windows_i686_gnullvm 0.52.6",
 "windows_i686_msvc 0.52.6",
 "windows_x86_64_gnu 0.52.6",
 "windows_x86_64_gnullvm 0.52.6",
 "windows_x86_64_msvc 0.52.6",
]

[[package]]
name = "windows-targets"
version = "0.53.3"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "d5fe6031c4041849d7c496a8ded650796e7b6ecc19df1a431c1a363342e5dc91"
dependencies = [
 "windows-link",
 "windows_aarch64_gnullvm 0.53.0",
 "windows_aarch64_msvc 0.53.0",
 "windows_i686_gnu 0.53.0",
 "windows_i686_gnullvm 0.53.0",
 "windows_i686_msvc 0.53.0",
 "windows_x86_64_gnu 0.53.0",
 "windows_x86_64_gnullvm 0.53.0",
 "windows_x86_64_msvc 0.53.0",
]

[[package]]
name = "windows_aarch64_gnullvm"
version = "0.48.5"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "2b38e32f0abccf9987a4e3079dfb67dcd799fb61361e53e2882c3cbaf0d905d8"

[[package]]
name = "windows_aarch64_gnullvm"
version = "0.52.6"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "32a4622180e7a0ec044bb555404c800bc9fd9ec262ec147edd5989ccd0c02cd3"

[[package]]
name = "windows_aarch64_gnullvm"
version = "0.53.0"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "86b8d5f90ddd19cb4a147a5fa63ca848db3df085e25fee3cc10b39b6eebae764"

[[package]]
name = "windows_aarch64_msvc"
version = "0.34.0"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "17cffbe740121affb56fad0fc0e421804adf0ae00891205213b5cecd30db881d"

[[package]]
name = "windows_aarch64_msvc"
version = "0.48.5"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "dc35310971f3b2dbbf3f0690a219f40e2d9afcf64f9ab7cc1be722937c26b4bc"

[[package]]
name = "windows_aarch64_msvc"
version = "0.52.6"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "09ec2a7bb152e2252b53fa7803150007879548bc709c039df7627cabbd05d469"

[[package]]
name = "windows_aarch64_msvc"
version = "0.53.0"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "c7651a1f62a11b8cbd5e0d42526e55f2c99886c77e007179efff86c2b137e66c"

[[package]]
name = "windows_i686_gnu"
version = "0.34.0"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "2564fde759adb79129d9b4f54be42b32c89970c18ebf93124ca8870a498688ed"

[[package]]
name = "windows_i686_gnu"
version = "0.48.5"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "a75915e7def60c94dcef72200b9a8e58e5091744960da64ec734a6c6e9b3743e"

[[package]]
name = "windows_i686_gnu"
version = "0.52.6"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "8e9b5ad5ab802e97eb8e295ac6720e509ee4c243f69d781394014ebfe8bbfa0b"

[[package]]
name = "windows_i686_gnu"
version = "0.53.0"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "c1dc67659d35f387f5f6c479dc4e28f1d4bb90ddd1a5d3da2e5d97b42d6272c3"

[[package]]
name = "windows_i686_gnullvm"
version = "0.52.6"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "0eee52d38c090b3caa76c563b86c3a4bd71ef1a819287c19d586d7334ae8ed66"

[[package]]
name = "windows_i686_gnullvm"
version = "0.53.0"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "9ce6ccbdedbf6d6354471319e781c0dfef054c81fbc7cf83f338a4296c0cae11"

[[package]]
name = "windows_i686_msvc"
version = "0.34.0"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "9cd9d32ba70453522332c14d38814bceeb747d80b3958676007acadd7e166956"

[[package]]
name = "windows_i686_msvc"
version = "0.48.5"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "8f55c233f70c4b27f66c523580f78f1004e8b5a8b659e05a4eb49d4166cca406"

[[package]]
name = "windows_i686_msvc"
version = "0.52.6"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "240948bc05c5e7c6dabba28bf89d89ffce3e303022809e73deaefe4f6ec56c66"

[[package]]
name = "windows_i686_msvc"
version = "0.53.0"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "581fee95406bb13382d2f65cd4a908ca7b1e4c2f1917f143ba16efe98a589b5d"

[[package]]
name = "windows_x86_64_gnu"
version = "0.34.0"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "cfce6deae227ee8d356d19effc141a509cc503dfd1f850622ec4b0f84428e1f4"

[[package]]
name = "windows_x86_64_gnu"
version = "0.48.5"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "53d40abd2583d23e4718fddf1ebec84dbff8381c07cae67ff7768bbf19c6718e"

[[package]]
name = "windows_x86_64_gnu"
version = "0.52.6"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "147a5c80aabfbf0c7d901cb5895d1de30ef2907eb21fbbab29ca94c5b08b1a78"

[[package]]
name = "windows_x86_64_gnu"
version = "0.53.0"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "2e55b5ac9ea33f2fc1716d1742db15574fd6fc8dadc51caab1c16a3d3b4190ba"

[[package]]
name = "windows_x86_64_gnullvm"
version = "0.48.5"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "0b7b52767868a23d5bab768e390dc5f5c55825b6d30b86c844ff2dc7414044cc"

[[package]]
name = "windows_x86_64_gnullvm"
version = "0.52.6"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "24d5b23dc417412679681396f2b49f3de8c1473deb516bd34410872eff51ed0d"

[[package]]
name = "windows_x86_64_gnullvm"
version = "0.53.0"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "0a6e035dd0599267ce1ee132e51c27dd29437f63325753051e71dd9e42406c57"

[[package]]
name = "windows_x86_64_msvc"
version = "0.34.0"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "d19538ccc21819d01deaf88d6a17eae6596a12e9aafdbb97916fb49896d89de9"

[[package]]
name = "windows_x86_64_msvc"
version = "0.48.5"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "ed94fce61571a4006852b7389a063ab983c02eb1bb37b47f8272ce92d06d9538"

[[package]]
name = "windows_x86_64_msvc"
version = "0.52.6"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "589f6da84c646204747d1270a2a5661ea66ed1cced2631d546fdfb155959f9ec"

[[package]]
name = "windows_x86_64_msvc"
version = "0.53.0"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "271414315aff87387382ec3d271b52d7ae78726f5d44ac98b4f4030c91880486"

[[package]]
name = "wit-bindgen-rt"
version = "0.39.0"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "6f42320e61fe2cfd34354ecb597f86f413484a798ba44a8ca1165c58d42da6c1"
dependencies = [
 "bitflags 2.9.1",
]

[[package]]
name = "writeable"
version = "0.6.1"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "ea2f10b9bb0928dfb1b42b65e1f9e36f7f54dbdf08457afefb38afcdec4fa2bb"

[[package]]
name = "yoke"
version = "0.8.0"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "5f41bb01b8226ef4bfd589436a297c53d118f65921786300e427be8d487695cc"
dependencies = [
 "serde",
 "stable_deref_trait",
 "yoke-derive",
 "zerofrom",
]

[[package]]
name = "yoke-derive"
version = "0.8.0"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "38da3c9736e16c5d3c8c597a9aaa5d1fa565d0532ae05e27c24aa62fb32c0ab6"
dependencies = [
 "proc-macro2",
 "quote",
 "syn",
 "synstructure",
]

[[package]]
name = "zerocopy"
version = "0.8.26"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "1039dd0d3c310cf05de012d8a39ff557cb0d23087fd44cad61df08fc31907a2f"
dependencies = [
 "zerocopy-derive",
]

[[package]]
name = "zerocopy-derive"
version = "0.8.26"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "9ecf5b4cc5364572d7f4c329661bcc82724222973f2cab6f050a4e5c22f75181"
dependencies = [
 "proc-macro2",
 "quote",
 "syn",
]

[[package]]
name = "zerofrom"
version = "0.1.6"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "50cc42e0333e05660c3587f3bf9d0478688e15d870fab3346451ce7f8c9fbea5"
dependencies = [
 "zerofrom-derive",
]

[[package]]
name = "zerofrom-derive"
version = "0.1.6"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "d71e5d6e06ab090c67b5e44993ec16b72dcbaabc526db883a360057678b48502"
dependencies = [
 "proc-macro2",
 "quote",
 "syn",
 "synstructure",
]

[[package]]
name = "zerotrie"
version = "0.2.2"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "36f0bbd478583f79edad978b407914f61b2972f5af6fa089686016be8f9af595"
dependencies = [
 "displaydoc",
 "yoke",
 "zerofrom",
]

[[package]]
name = "zerovec"
version = "0.11.4"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "e7aa2bd55086f1ab526693ecbe444205da57e25f4489879da80635a46d90e73b"
dependencies = [
 "yoke",
 "zerofrom",
 "zerovec-derive",
]

[[package]]
name = "zerovec-derive"
version = "0.11.1"
source = "registry+https://github.com/rust-lang/crates.io-index"
checksum = "5b96237efa0c878c64bd89c436f661be4e46b2f3eff1ebb976f7ef2321d2f58f"
dependencies = [
 "proc-macro2",
 "quote",
 "syn",
]
```

## 文件: `data-processor/Cargo.toml`

```
[package]
name = "data-processor"
version = "0.1.0"
edition = "2021"
authors = ["Data Recorder Backend"]
description = "Data processing service for signal acquisition system"

[dependencies]
# Web server and HTTP
tokio = { version = "1.0", features = ["full"] }
axum = "0.7"
tower = "0.4"
tower-http = { version = "0.5", features = ["cors", "fs"] }
hyper = "1.0"

# WebSocket support
tokio-tungstenite = "0.21"
futures-util = "0.3"

# Serialization
serde = { version = "1.0", features = ["derive"] }
serde_json = "1.0"

# Shared memory and IPC
shared_memory = "0.12"
mio = "0.8"

# Logging
tracing = "0.1"
tracing-subscriber = "0.3"

# Error handling
anyhow = "1.0"
thiserror = "1.0"

# Async utilities
async-trait = "0.1"

# Signal processing (for future data processing)
rustfft = "6.0"

# File handling
tempfile = "3.0"

# UUID generation
uuid = { version = "1.0", features = ["v4"] }

# Time handling
chrono = { version = "0.4", features = ["serde"] }

# Windows-specific dependencies
[target.'cfg(windows)'.dependencies]
winapi = { version = "0.3", features = ["winbase", "handleapi", "synchapi", "memoryapi"] }
```

## 文件: `data-processor/src/config.rs`

```rust
use anyhow::Result;
use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Config {
    pub shared_memory_name: String,
    pub message_queue_name: String,
    pub web_server: WebServerConfig,
    pub websocket: WebSocketConfig,
    pub data_processing: DataProcessingConfig,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct WebServerConfig {
    pub host: String,
    pub port: u16,
    pub tls_cert_path: Option<String>,
    pub tls_key_path: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct WebSocketConfig {
    pub host: String,
    pub port: u16,
    pub max_connections: usize,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct DataProcessingConfig {
    pub buffer_size: usize,
    pub processing_interval_ms: u64,
    pub max_packet_age_ms: u64,
}

impl Default for Config {
    fn default() -> Self {
        Self {
            shared_memory_name: "ADC_DATA_SHARED_MEM".to_string(),
            message_queue_name: "data_reader_to_processor".to_string(),
            web_server: WebServerConfig {
                host: "127.0.0.1".to_string(),
                port: 8443,
                tls_cert_path: None,
                tls_key_path: None,
            },
            websocket: WebSocketConfig {
                host: "127.0.0.1".to_string(),
                port: 8080,
                max_connections: 100,
            },
            data_processing: DataProcessingConfig {
                buffer_size: 1024,
                processing_interval_ms: 10,
                max_packet_age_ms: 1000,
            },
        }
    }
}

impl Config {
    pub fn load() -> Result<Self> {
        // For now, use default configuration
        // In the future, this could load from a config file or environment variables
        Ok(Self::default())
    }
}
```

## 文件: `data-processor/src/data_processing.rs`

```rust
use crate::ipc::{SharedMemoryReader, ADCDataPacket};
use anyhow::Result;
use serde::{Deserialize, Serialize};
use std::collections::VecDeque;
use std::sync::Arc;
use tokio::sync::{broadcast, RwLock};
use tokio::time::{interval, Duration};
use tracing::{info, warn, error, debug};

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ProcessedData {
    pub timestamp: u64,
    pub sequence: u16,
    pub channel_count: usize,
    pub sample_rate: f64,
    pub data: Vec<f64>,
    pub metadata: DataMetadata,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct DataMetadata {
    pub packet_count: u32,
    pub processing_time_us: u64,
    pub data_quality: DataQuality,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub enum DataQuality {
    Good,
    Warning(String),
    Error(String),
}

pub struct DataProcessor {
    shared_mem: Arc<RwLock<SharedMemoryReader>>,
    processed_data_tx: broadcast::Sender<ProcessedData>,
    buffer: VecDeque<ADCDataPacket>,
    packet_count: u64,
}

impl DataProcessor {
    pub fn new(shared_mem: SharedMemoryReader) -> Self {
        let (tx, _) = broadcast::channel(1000);

        Self {
            shared_mem: Arc::new(RwLock::new(shared_mem)),
            processed_data_tx: tx,
            buffer: VecDeque::with_capacity(1024),
            packet_count: 0,
        }
    }

    pub fn get_data_receiver(&self) -> broadcast::Receiver<ProcessedData> {
        self.processed_data_tx.subscribe()
    }

    pub async fn run(&mut self) -> Result<()> {
        info!("Starting data processing loop");

        let mut interval = interval(Duration::from_millis(10));

        loop {
            interval.tick().await;

            // 读取新的数据包
            let new_packets = {
                let mut reader = self.shared_mem.write().await;
                reader.read_new_packets()?
            };

            if !new_packets.is_empty() {
                debug!("Received {} new packets", new_packets.len());

                // 添加到缓冲区
                for packet in new_packets {
                    self.buffer.push_back(packet);
                    self.packet_count += 1;
                }

                // 处理缓冲区中的数据
                self.process_buffered_data().await?;
            }

            // 清理过期数据
            self.cleanup_old_data();
        }
    }

    async fn process_buffered_data(&mut self) -> Result<()> {
        while let Some(packet) = self.buffer.pop_front() {
            let start_time = std::time::Instant::now();

            match self.process_single_packet(&packet).await {
                Ok(processed) => {
                    let processing_time = start_time.elapsed().as_micros() as u64;

                    let mut processed_with_timing = processed;
                    processed_with_timing.metadata.processing_time_us = processing_time;

                    // 发送处理后的数据
                    if let Err(e) = self.processed_data_tx.send(processed_with_timing) {
                        warn!("Failed to send processed data: {}", e);
                    }
                }
                Err(e) => {
                    error!("Failed to process packet {}: {}", packet.sequence, e);
                }
            }
        }

        Ok(())
    }

    async fn process_single_packet(&self, packet: &ADCDataPacket) -> Result<ProcessedData> {
        // 基本的数据解析和处理
        let payload = &packet.payload[..packet.payload_len as usize];

        // 假设ADC数据是16位整数，小端序
        let mut samples = Vec::new();
        for chunk in payload.chunks_exact(2) {
            if chunk.len() == 2 {
                let raw_value = u16::from_le_bytes([chunk[0], chunk[1]]);
                // 转换为电压值（假设3.3V参考电压，12位ADC）
                let voltage = (raw_value as f64 / 4095.0) * 3.3;
                samples.push(voltage);
            }
        }

        // 数据质量检查
        let quality = self.assess_data_quality(&samples);

        // 应用简单的滤波（移动平均）
        let filtered_samples = self.apply_moving_average(&samples, 5);

        Ok(ProcessedData {
            timestamp: packet.timestamp_ms as u64,
            sequence: packet.sequence,
            channel_count: 1, // 假设单通道
            sample_rate: 1000.0, // 假设1kHz采样率
            data: filtered_samples,
            metadata: DataMetadata {
                packet_count: self.packet_count as u32,
                processing_time_us: 0, // 将在调用处设置
                data_quality: quality,
            },
        })
    }

    fn assess_data_quality(&self, samples: &[f64]) -> DataQuality {
        if samples.is_empty() {
            return DataQuality::Error("No data samples".to_string());
        }

        // 检查数据范围
        let min_val = samples.iter().fold(f64::INFINITY, |a, &b| a.min(b));
        let max_val = samples.iter().fold(f64::NEG_INFINITY, |a, &b| a.max(b));

        if min_val < 0.0 || max_val > 3.3 {
            return DataQuality::Warning("Data out of expected range".to_string());
        }

        // 检查数据变化
        let mean = samples.iter().sum::<f64>() / samples.len() as f64;
        let variance = samples.iter()
            .map(|x| (x - mean).powi(2))
            .sum::<f64>() / samples.len() as f64;

        if variance < 1e-6 {
            return DataQuality::Warning("Data appears to be constant".to_string());
        }

        DataQuality::Good
    }

    fn apply_moving_average(&self, samples: &[f64], window_size: usize) -> Vec<f64> {
        if samples.len() < window_size {
            return samples.to_vec();
        }

        let mut filtered = Vec::with_capacity(samples.len());

        for i in 0..samples.len() {
            let start = if i >= window_size / 2 { i - window_size / 2 } else { 0 };
            let end = std::cmp::min(start + window_size, samples.len());

            let sum: f64 = samples[start..end].iter().sum();
            let avg = sum / (end - start) as f64;
            filtered.push(avg);
        }

        filtered
    }

    fn cleanup_old_data(&mut self) {
        // 保持缓冲区大小在合理范围内
        const MAX_BUFFER_SIZE: usize = 100;

        while self.buffer.len() > MAX_BUFFER_SIZE {
            self.buffer.pop_front();
        }
    }
}
```

## 文件: `data-processor/src/file_manager.rs`

```rust
use anyhow::{Result, anyhow};
use std::fs;
use std::path::{Path, PathBuf};
use tracing::{info, warn};

pub struct FileManager {
    data_directory: PathBuf,
}

impl FileManager {
    pub fn new<P: AsRef<Path>>(data_directory: P) -> Result<Self> {
        let data_dir = data_directory.as_ref().to_path_buf();

        // 确保数据目录存在
        if !data_dir.exists() {
            fs::create_dir_all(&data_dir)?;
            info!("Created data directory: {:?}", data_dir);
        }

        Ok(Self {
            data_directory: data_dir,
        })
    }

    pub fn list_files(&self) -> Result<Vec<FileInfo>> {
        let mut files = Vec::new();

        for entry in fs::read_dir(&self.data_directory)? {
            let entry = entry?;
            let path = entry.path();

            if path.is_file() {
                if let Some(file_info) = self.get_file_info(&path)? {
                    files.push(file_info);
                }
            }
        }

        // 按创建时间排序（最新的在前）
        files.sort_by(|a, b| b.created_at.cmp(&a.created_at));

        Ok(files)
    }

    pub fn get_file_path(&self, filename: &str) -> Result<PathBuf> {
        // 验证文件名安全性
        if filename.contains("..") || filename.contains("/") || filename.contains("\\") {
            return Err(anyhow!("Invalid filename: {}", filename));
        }

        let file_path = self.data_directory.join(filename);

        if !file_path.exists() {
            return Err(anyhow!("File not found: {}", filename));
        }

        Ok(file_path)
    }

    pub fn read_file(&self, filename: &str) -> Result<Vec<u8>> {
        let file_path = self.get_file_path(filename)?;
        let content = fs::read(&file_path)?;

        info!("Read file: {} ({} bytes)", filename, content.len());
        Ok(content)
    }

    pub fn save_processed_data(&self, data: &ProcessedDataFile) -> Result<String> {
        let timestamp = chrono::Utc::now().format("%Y%m%d_%H%M%S");
        let filename = format!("processed_data_{}.json", timestamp);
        let file_path = self.data_directory.join(&filename);

        let json_content = serde_json::to_string_pretty(data)?;
        fs::write(&file_path, json_content)?;

        info!("Saved processed data to: {}", filename);
        Ok(filename)
    }

    pub fn cleanup_old_files(&self, max_files: usize) -> Result<()> {
        let mut files = self.list_files()?;

        if files.len() <= max_files {
            return Ok(());
        }

        // 删除最旧的文件
        files.sort_by(|a, b| a.created_at.cmp(&b.created_at));
        let files_to_delete = files.len() - max_files;

        for file in files.iter().take(files_to_delete) {
            let file_path = self.data_directory.join(&file.filename);
            if let Err(e) = fs::remove_file(&file_path) {
                warn!("Failed to delete old file {}: {}", file.filename, e);
            } else {
                info!("Deleted old file: {}", file.filename);
            }
        }

        Ok(())
    }

    fn get_file_info(&self, path: &Path) -> Result<Option<FileInfo>> {
        let metadata = fs::metadata(path)?;

        if let Some(filename) = path.file_name().and_then(|n| n.to_str()) {
            let file_type = self.determine_file_type(filename);

            let created_at = metadata
                .created()
                .or_else(|_| metadata.modified())
                .unwrap_or(std::time::SystemTime::UNIX_EPOCH)
                .duration_since(std::time::SystemTime::UNIX_EPOCH)
                .unwrap_or_default()
                .as_millis() as i64;

            Ok(Some(FileInfo {
                filename: filename.to_string(),
                size_bytes: metadata.len(),
                created_at,
                file_type,
            }))
        } else {
            Ok(None)
        }
    }

    fn determine_file_type(&self, filename: &str) -> String {
        if filename.starts_with("raw_frames_") && filename.ends_with(".txt") {
            "raw_frames".to_string()
        } else if filename.starts_with("processed_data_") && filename.ends_with(".json") {
            "processed_data".to_string()
        } else {
            "unknown".to_string()
        }
    }
}

#[derive(Debug, Clone, serde::Serialize, serde::Deserialize)]
pub struct FileInfo {
    pub filename: String,
    pub size_bytes: u64,
    pub created_at: i64,
    pub file_type: String,
}

#[derive(Debug, serde::Serialize, serde::Deserialize)]
pub struct ProcessedDataFile {
    pub metadata: FileMetadata,
    pub data_packets: Vec<DataPacketRecord>,
}

#[derive(Debug, serde::Serialize, serde::Deserialize)]
pub struct FileMetadata {
    pub created_at: i64,
    pub version: String,
    pub sample_rate: f64,
    pub channel_count: usize,
    pub total_packets: usize,
    pub duration_ms: u64,
}

#[derive(Debug, serde::Serialize, serde::Deserialize)]
pub struct DataPacketRecord {
    pub timestamp: u64,
    pub sequence: u16,
    pub data: Vec<f64>,
    pub quality: String,
}
```

## 文件: `data-processor/src/ipc.rs`

```rust
use anyhow::{Result, anyhow};
use std::sync::atomic::{AtomicU32, Ordering};

// 共享内存数据结构（与C代码保持一致）
#[repr(C)]
#[derive(Debug)]
pub struct SharedMemHeader {
    pub magic: u32,           // 0xADC12345
    pub version: u32,         // 1
    pub write_index: AtomicU32,
    pub read_index: AtomicU32,
    pub buffer_size: u32,     // 1024
    pub packet_count: AtomicU32,
    pub status: u8,
    pub reserved: [u8; 7],
}

#[repr(C)]
#[derive(Debug, Clone)]
pub struct ADCDataPacket {
    pub timestamp_ms: u32,
    pub sequence: u16,
    pub payload_len: u16,
    pub payload: [u8; 4096],
}

#[repr(C)]
pub struct SharedMemory {
    pub header: SharedMemHeader,
    pub packets: [ADCDataPacket; 1024],
}

// 消息队列数据结构
#[repr(C)]
#[derive(Debug, Clone)]
pub enum MessageType {
    StartCollection = 1,
    StopCollection = 2,
    StatusRequest = 3,
    StatusResponse = 4,
    SaveWaveform = 5,
    Error = 99,
}

#[repr(C)]
#[derive(Debug, Clone)]
pub struct IPCMessage {
    pub msg_type: u32,
    pub timestamp: u32,
    pub data_len: u32,
    pub data: [u8; 256],
}

pub struct SharedMemoryReader {
    _mapping: shared_memory::Shmem,
    shared_mem: *mut SharedMemory,
    last_read_index: u32,
}

unsafe impl Send for SharedMemoryReader {}
unsafe impl Sync for SharedMemoryReader {}

impl SharedMemoryReader {
    pub fn new(name: &str) -> Result<Self> {
        // 尝试打开现有的共享内存
        let mapping = shared_memory::ShmemConf::new()
            .size(std::mem::size_of::<SharedMemory>())
            .flink(name)
            .open()
            .map_err(|e| anyhow!("Failed to open shared memory '{}': {}", name, e))?;

        let shared_mem = mapping.as_ptr() as *mut SharedMemory;

        // 验证魔数和版本
        unsafe {
            let header = &(*shared_mem).header;
            if header.magic != 0xADC12345 {
                return Err(anyhow!("Invalid magic number in shared memory"));
            }
            if header.version != 1 {
                return Err(anyhow!("Unsupported shared memory version"));
            }
        }

        Ok(Self {
            _mapping: mapping,
            shared_mem,
            last_read_index: 0,
        })
    }

    pub fn read_new_packets(&mut self) -> Result<Vec<ADCDataPacket>> {
        let mut packets = Vec::new();

        unsafe {
            let header = &(*self.shared_mem).header;
            let current_write_index = header.write_index.load(Ordering::Acquire);

            while self.last_read_index != current_write_index {
                let packet_index = (self.last_read_index % header.buffer_size) as usize;
                let packet = (*self.shared_mem).packets[packet_index].clone();
                packets.push(packet);

                self.last_read_index = (self.last_read_index + 1) % header.buffer_size;
            }

            // 更新读取索引
            header.read_index.store(self.last_read_index, Ordering::Release);
        }

        Ok(packets)
    }

    pub fn get_status(&self) -> Result<(u32, u32, u32)> {
        unsafe {
            let header = &(*self.shared_mem).header;
            Ok((
                header.write_index.load(Ordering::Acquire),
                header.read_index.load(Ordering::Acquire),
                header.packet_count.load(Ordering::Acquire),
            ))
        }
    }
}

pub struct MessageQueue {
    // 占位符 - 实际实现将使用Windows命名管道或其他IPC机制
    _name: String,
}

impl MessageQueue {
    pub fn new(name: &str) -> Result<Self> {
        // TODO: 实现实际的消息队列连接
        Ok(Self {
            _name: name.to_string(),
        })
    }

    pub async fn send_message(&self, _msg: IPCMessage) -> Result<()> {
        // TODO: 实现消息发送
        Ok(())
    }

    pub async fn receive_message(&self) -> Result<Option<IPCMessage>> {
        // TODO: 实现消息接收
        Ok(None)
    }
}
```

## 文件: `data-processor/src/main.rs`

```rust
mod ipc;
mod data_processing;
mod web_server;
mod websocket;
mod file_manager;
mod config;

use anyhow::Result;
use tracing::{info, warn, error};
use tracing_subscriber;

#[tokio::main]
async fn main() -> Result<()> {
    // Initialize logging
    tracing_subscriber::fmt::init();

    info!("Starting Data Processor Service");

    // Load configuration
    let config = config::Config::load()?;
    info!("Configuration loaded: {:?}", config);

    // Initialize shared memory connection (with fallback for development)
    let shared_mem = match ipc::SharedMemoryReader::new(&config.shared_memory_name) {
        Ok(mem) => {
            info!("Connected to shared memory");
            mem
        }
        Err(e) => {
            warn!("Failed to connect to shared memory: {}. Running in standalone mode.", e);
            // For development, we'll create a mock shared memory reader
            return Err(e);
        }
    };

    // Initialize message queue
    let _message_queue = ipc::MessageQueue::new(&config.message_queue_name)?;
    info!("Connected to message queue");

    // Start data processing task
    let mut data_processor = data_processing::DataProcessor::new(shared_mem);
    let data_receiver = data_processor.get_data_receiver();

    let processing_handle = tokio::spawn(async move {
        if let Err(e) = data_processor.run().await {
            error!("Data processing error: {}", e);
        }
    });

    // Start WebSocket server
    let mut websocket_server = websocket::WebSocketServer::new(
        config.websocket.clone(),
        data_receiver,
    );
    let websocket_handle = tokio::spawn(async move {
        if let Err(e) = websocket_server.run().await {
            error!("WebSocket server error: {}", e);
        }
    });

    // Start web server
    let web_server = web_server::WebServer::new(config.clone());
    let server_handle = tokio::spawn(async move {
        if let Err(e) = web_server.run().await {
            error!("Web server error: {}", e);
        }
    });

    info!("All services started successfully");

    // Wait for all tasks to complete
    tokio::select! {
        _ = processing_handle => {
            error!("Data processing task terminated");
        }
        _ = websocket_handle => {
            error!("WebSocket server task terminated");
        }
        _ = server_handle => {
            error!("Web server task terminated");
        }
        _ = tokio::signal::ctrl_c() => {
            info!("Received shutdown signal");
        }
    }

    info!("Shutting down Data Processor Service");
    Ok(())
}
```

## 文件: `data-processor/src/web_server.rs`

```rust
use crate::config::Config;
use anyhow::Result;
use axum::{
    extract::Path,
    http::StatusCode,
    response::{Json, Response},
    routing::{get, post},
    Router,
};
use serde::{Deserialize, Serialize};
use tower::ServiceBuilder;
use tower_http::cors::CorsLayer;
use tracing::info;

#[derive(Debug, Serialize, Deserialize)]
pub struct ApiResponse<T> {
    pub success: bool,
    pub data: Option<T>,
    pub error: Option<String>,
    pub timestamp: i64,
}

#[derive(Debug, Serialize, Deserialize)]
pub struct ControlCommand {
    pub command: String,
    pub parameters: Option<serde_json::Value>,
}

#[derive(Debug, Serialize, Deserialize)]
pub struct SystemStatus {
    pub data_collection_active: bool,
    pub connected_clients: usize,
    pub packets_processed: u64,
    pub uptime_seconds: u64,
    pub memory_usage_mb: f64,
}

#[derive(Debug, Serialize, Deserialize)]
pub struct FileInfo {
    pub filename: String,
    pub size_bytes: u64,
    pub created_at: i64,
    pub file_type: String,
}

pub struct WebServer {
    config: Config,
}

impl WebServer {
    pub fn new(config: Config) -> Self {
        Self { config }
    }

    pub async fn run(&self) -> Result<()> {
        let app = self.create_router();

        let addr = format!("{}:{}", self.config.web_server.host, self.config.web_server.port);
        info!("Starting HTTPS server on {}", addr);

        let listener = tokio::net::TcpListener::bind(&addr).await?;
        axum::serve(listener, app).await?;

        Ok(())
    }

    fn create_router(&self) -> Router {
        Router::new()
            // 控制API
            .route("/api/control/start", post(start_collection))
            .route("/api/control/stop", post(stop_collection))
            .route("/api/control/status", get(get_status))

            // 文件管理API
            .route("/api/files", get(list_files))
            .route("/api/files/:filename", get(download_file))
            .route("/api/files/save", post(save_waveform))

            // 健康检查
            .route("/health", get(health_check))

            // CORS支持
            .layer(
                ServiceBuilder::new()
                    .layer(CorsLayer::permissive())
            )
    }
}

// API处理函数

async fn start_collection() -> Result<Json<ApiResponse<String>>, StatusCode> {
    info!("Received start collection command");

    // TODO: 通过消息队列发送开始采集命令给数据采集进程

    Ok(Json(ApiResponse {
        success: true,
        data: Some("Data collection started".to_string()),
        error: None,
        timestamp: chrono::Utc::now().timestamp_millis(),
    }))
}

async fn stop_collection() -> Result<Json<ApiResponse<String>>, StatusCode> {
    info!("Received stop collection command");

    // TODO: 通过消息队列发送停止采集命令给数据采集进程

    Ok(Json(ApiResponse {
        success: true,
        data: Some("Data collection stopped".to_string()),
        error: None,
        timestamp: chrono::Utc::now().timestamp_millis(),
    }))
}

async fn get_status() -> Result<Json<ApiResponse<SystemStatus>>, StatusCode> {
    // TODO: 获取实际的系统状态
    let status = SystemStatus {
        data_collection_active: false, // 从实际状态获取
        connected_clients: 0,          // 从WebSocket服务器获取
        packets_processed: 0,          // 从数据处理器获取
        uptime_seconds: 0,             // 计算实际运行时间
        memory_usage_mb: 0.0,          // 获取内存使用情况
    };

    Ok(Json(ApiResponse {
        success: true,
        data: Some(status),
        error: None,
        timestamp: chrono::Utc::now().timestamp_millis(),
    }))
}

async fn list_files() -> Result<Json<ApiResponse<Vec<FileInfo>>>, StatusCode> {
    // TODO: 扫描文件目录，返回可用的波形文件列表
    let files = vec![
        FileInfo {
            filename: "raw_frames_001.txt".to_string(),
            size_bytes: 1024000,
            created_at: chrono::Utc::now().timestamp_millis(),
            file_type: "raw_frames".to_string(),
        }
    ];

    Ok(Json(ApiResponse {
        success: true,
        data: Some(files),
        error: None,
        timestamp: chrono::Utc::now().timestamp_millis(),
    }))
}

async fn download_file(Path(filename): Path<String>) -> Result<Response, StatusCode> {
    info!("File download requested: {}", filename);

    // TODO: 实现文件下载逻辑
    // 1. 验证文件名安全性
    // 2. 检查文件是否存在
    // 3. 返回文件内容

    // 暂时返回404
    Err(StatusCode::NOT_FOUND)
}

async fn save_waveform() -> Result<Json<ApiResponse<String>>, StatusCode> {
    info!("Waveform save requested");

    // TODO: 通过消息队列通知数据采集进程保存当前波形

    Ok(Json(ApiResponse {
        success: true,
        data: Some("Waveform save initiated".to_string()),
        error: None,
        timestamp: chrono::Utc::now().timestamp_millis(),
    }))
}

async fn health_check() -> Json<ApiResponse<String>> {
    Json(ApiResponse {
        success: true,
        data: Some("OK".to_string()),
        error: None,
        timestamp: chrono::Utc::now().timestamp_millis(),
    })
}
```

## 文件: `data-processor/src/websocket.rs`

```rust
use crate::config::WebSocketConfig;
use crate::data_processing::ProcessedData;
use anyhow::Result;
use futures_util::{SinkExt, StreamExt};
use std::collections::HashMap;
use std::sync::Arc;
use tokio::net::{TcpListener, TcpStream};
use tokio::sync::{broadcast, RwLock};
use tokio_tungstenite::{accept_async, tungstenite::Message};
use tracing::{info, warn, error, debug};
use uuid::Uuid;

pub struct WebSocketServer {
    config: WebSocketConfig,
    clients: Arc<RwLock<HashMap<String, ClientConnection>>>,
    data_receiver: broadcast::Receiver<ProcessedData>,
}

struct ClientConnection {
    id: String,
    sender: tokio::sync::mpsc::UnboundedSender<Message>,
}

impl WebSocketServer {
    pub fn new(config: WebSocketConfig, data_receiver: broadcast::Receiver<ProcessedData>) -> Self {
        Self {
            config,
            clients: Arc::new(RwLock::new(HashMap::new())),
            data_receiver,
        }
    }

    pub async fn run(&mut self) -> Result<()> {
        let addr = format!("{}:{}", self.config.host, self.config.port);
        let listener = TcpListener::bind(&addr).await?;
        info!("WebSocket server listening on {}", addr);

        // 启动数据广播任务
        let clients_clone = Arc::clone(&self.clients);
        let mut data_rx = self.data_receiver.resubscribe();
        tokio::spawn(async move {
            while let Ok(data) = data_rx.recv().await {
                Self::broadcast_data(&clients_clone, &data).await;
            }
        });

        // 接受客户端连接
        while let Ok((stream, addr)) = listener.accept().await {
            info!("New WebSocket connection from {}", addr);

            let clients = Arc::clone(&self.clients);
            tokio::spawn(async move {
                if let Err(e) = Self::handle_connection(stream, clients).await {
                    error!("WebSocket connection error: {}", e);
                }
            });
        }

        Ok(())
    }

    async fn handle_connection(
        stream: TcpStream,
        clients: Arc<RwLock<HashMap<String, ClientConnection>>>,
    ) -> Result<()> {
        let ws_stream = accept_async(stream).await?;
        let (mut ws_sender, mut ws_receiver) = ws_stream.split();

        let client_id = Uuid::new_v4().to_string();
        let (tx, mut rx) = tokio::sync::mpsc::unbounded_channel();

        // 添加客户端到连接列表
        {
            let mut clients_guard = clients.write().await;
            clients_guard.insert(client_id.clone(), ClientConnection {
                id: client_id.clone(),
                sender: tx.clone(),
            });
        }

        info!("Client {} connected", client_id);

        // 发送欢迎消息
        let welcome_msg = serde_json::json!({
            "type": "welcome",
            "client_id": client_id,
            "timestamp": chrono::Utc::now().timestamp_millis()
        });

        if let Ok(msg_text) = serde_json::to_string(&welcome_msg) {
            let _ = tx.send(Message::Text(msg_text));
        }

        // 处理发送任务
        let clients_for_sender = Arc::clone(&clients);
        let client_id_for_sender = client_id.clone();
        let sender_task = tokio::spawn(async move {
            while let Some(message) = rx.recv().await {
                if let Err(e) = ws_sender.send(message).await {
                    error!("Failed to send message to client {}: {}", client_id_for_sender, e);
                    break;
                }
            }

            // 清理客户端连接
            let mut clients_guard = clients_for_sender.write().await;
            clients_guard.remove(&client_id_for_sender);
            info!("Client {} disconnected", client_id_for_sender);
        });

        // 处理接收任务
        let receiver_task = tokio::spawn(async move {
            while let Some(msg) = ws_receiver.next().await {
                match msg {
                    Ok(Message::Text(text)) => {
                        debug!("Received message from {}: {}", client_id, text);
                        // 处理客户端消息（如果需要）
                        if let Err(e) = Self::handle_client_message(&client_id, &text).await {
                            warn!("Error handling message from {}: {}", client_id, e);
                        }
                    }
                    Ok(Message::Close(_)) => {
                        info!("Client {} requested close", client_id);
                        break;
                    }
                    Ok(Message::Ping(_data)) => {
                        debug!("Ping from client {}", client_id);
                        // Pong will be sent automatically
                    }
                    Ok(Message::Pong(_)) => {
                        debug!("Pong from client {}", client_id);
                    }
                    Ok(Message::Binary(_)) => {
                        warn!("Received binary message from {}, ignoring", client_id);
                    }
                    Ok(Message::Frame(_)) => {
                        // Handle raw frame messages (usually not needed in application code)
                        debug!("Received frame message from {}", client_id);
                    }
                    Err(e) => {
                        error!("WebSocket error for client {}: {}", client_id, e);
                        break;
                    }
                }
            }
        });

        // 等待任一任务完成
        tokio::select! {
            _ = sender_task => {},
            _ = receiver_task => {},
        }

        Ok(())
    }

    async fn handle_client_message(client_id: &str, message: &str) -> Result<()> {
        // 解析客户端消息
        if let Ok(msg) = serde_json::from_str::<serde_json::Value>(message) {
            match msg.get("type").and_then(|t| t.as_str()) {
                Some("ping") => {
                    debug!("Ping from client {}", client_id);
                    // 可以发送pong响应
                }
                Some("subscribe") => {
                    info!("Client {} subscribed to data stream", client_id);
                    // 客户端订阅数据流
                }
                Some("unsubscribe") => {
                    info!("Client {} unsubscribed from data stream", client_id);
                    // 客户端取消订阅
                }
                _ => {
                    debug!("Unknown message type from client {}: {}", client_id, message);
                }
            }
        }

        Ok(())
    }

    async fn broadcast_data(
        clients: &Arc<RwLock<HashMap<String, ClientConnection>>>,
        data: &ProcessedData,
    ) {
        let message = serde_json::json!({
            "type": "data",
            "timestamp": data.timestamp,
            "sequence": data.sequence,
            "channel_count": data.channel_count,
            "sample_rate": data.sample_rate,
            "data": data.data,
            "metadata": data.metadata
        });

        if let Ok(msg_text) = serde_json::to_string(&message) {
            let clients_guard = clients.read().await;
            let mut failed_clients = Vec::new();

            for (client_id, client) in clients_guard.iter() {
                if let Err(_) = client.sender.send(Message::Text(msg_text.clone())) {
                    failed_clients.push(client_id.clone());
                }
            }

            // 清理失败的连接将在发送任务中处理
            if !failed_clients.is_empty() {
                debug!("Failed to send to {} clients", failed_clients.len());
            }
        }
    }
}
```

