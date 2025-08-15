#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <conio.h>      // _kbhit/_getch
#include <stdlib.h>
#include <string.h>

#include "io_buffer.h"
#include "protocol.h"

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

typedef struct {
    uint8_t* data;
    uint16_t len;
} RawFrame_t;

static RawFrame_t g_frameBatch[FRAME_BATCH_SAVE_COUNT];
static int        g_frameInBatch = 0;

static volatile bool g_running = true;

// ===================== 工具函数 =====================

static void print_usage(const char* progName)
{
    printf("Usage: %s [COM_NUMBER]\n", progName);
    printf("  COM_NUMBER: COM port number (e.g., 7 for COM7)\n");
    printf("  If not specified, defaults to COM7\n");
    printf("\nExamples:\n");
    printf("  %s 3      # Use COM3\n", progName);
    printf("  %s        # Use default COM7\n", progName);
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

// 解析成功回调（给 tryParseFramesFromRx 调用）
static void onFrameParsed(const uint8_t* frame, uint16_t frameLen)
{
    // 保存原始帧
    cache_frame(frame, frameLen);

    // 再做协议解析
    uint8_t  cmd = 0;
    uint8_t  seq = 0;
    uint8_t  payload[MAX_FRAME_SIZE];
    uint16_t payloadLen = 0;

    int ret = parseFrame(frame, frameLen, &cmd, &seq, payload, &payloadLen);
    if (ret == 0) {
        printf("[Parsed] Cmd=0x%02X Seq=%u PayloadLen=%u\n", cmd, seq, payloadLen);
    } else {
        printf("[Parse ERR] ret=%d (len=%u)\n", ret, frameLen);
    }
}

static inline bool user_wants_quit(void)
{
    if (_kbhit()) {
        int ch = _getch();
        if (ch == 27 || ch == 'q' || ch == 'Q') return true; // ESC/q
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

    initRxBuffer(&g_rx);

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

        if (user_wants_quit()) {
            printf("Quit key pressed.\n");
            g_running = false;
        }

        Sleep(1);
    }

    // 收尾：把剩余缓存写入
    if (g_frameInBatch > 0)
        flush_batch_to_file();
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