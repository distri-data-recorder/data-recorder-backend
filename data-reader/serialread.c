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

    HANDLE hSerial = open_serial(comPort);
    if (hSerial == INVALID_HANDLE_VALUE) {
        if (g_fp) fclose(g_fp);
        return 1;
    }

    printf("Start reading %s ... (ESC/q to quit)\n", comPort);
    serial_loop(hSerial);

    CloseHandle(hSerial);
    if (g_fp) fclose(g_fp);

    puts("Bye.");
    return 0;
}