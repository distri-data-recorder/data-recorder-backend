// 文件: test-sender.c
// 描述: 高保真下位机模拟器，通过TCP Socket与data-reader通信
// 版本: v2.0
// 协议: V6

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <math.h>

#ifndef _WIN32
#include <signal.h>
#include <unistd.h>
#else
#include <io.h>
#include <fcntl.h>
#endif

#include "protocol/protocol.h"
#include "protocol/io_buffer.h"

#ifdef _WIN32
#pragma comment(lib, "Ws2_32.lib")
#endif

// --- 配置 ---
#define DEFAULT_PORT "9001"
#define DEVICE_UNIQUE_ID 0x11223344AABBCCDDULL
#define SAMPLE_DATA_FILE "sample_data.csv"
#define DATA_SEND_INTERVAL_MS 10
#define MAX_CLIENTS 1
#define MAX_CHANNELS 4
#define CSV_BUFFER_SIZE 32768

// --- 协议命令定义 (与协议规范一致) ---
#define CMD_PING                    0x01
#define CMD_PONG                    0x81
#define CMD_GET_STATUS              0x02  
#define CMD_STATUS_RESPONSE         0x82
#define CMD_GET_DEVICE_INFO         0x03
#define CMD_DEVICE_INFO_RESPONSE    0x83
#define CMD_SET_MODE_CONTINUOUS     0x10
#define CMD_SET_MODE_TRIGGER        0x11
#define CMD_START_STREAM            0x12
#define CMD_STOP_STREAM             0x13
#define CMD_CONFIGURE_STREAM        0x14
#define CMD_ACK                     0x90
#define CMD_NACK                    0x91
#define CMD_DATA_PACKET             0x40
#define CMD_EVENT_TRIGGERED         0x41
#define CMD_REQUEST_BUFFERED_DATA   0x42
#define CMD_BUFFER_TRANSFER_COMPLETE 0x4F
#define CMD_LOG_MESSAGE             0xE0

// --- 设备能力定义 ---
typedef struct {
    uint8_t channel_id;
    uint32_t max_sample_rate_hz;
    uint16_t supported_formats_mask;
    char name[32];
    bool enabled;
    uint32_t current_sample_rate;
    uint8_t current_format;
} ChannelInfo_t;

// --- 全局状态 ---
typedef enum {
    MODE_CONTINUOUS,
    MODE_TRIGGER,
} DeviceMode;

typedef enum {
    STATUS_STOPPED,
    STATUS_RUNNING,
} StreamStatus;

typedef struct {
    DeviceMode mode;
    StreamStatus stream_status;
    uint8_t seq_counter;
    uint32_t timestamp_ms;
    bool device_error;
    uint8_t error_code;
    
    // 通道配置
    ChannelInfo_t channels[MAX_CHANNELS];
    uint8_t num_channels;
    
    // CSV数据缓冲
    char csv_buffer[CSV_BUFFER_SIZE];
    int csv_rows;
    int current_csv_row;
    float** csv_data;
    
    // 触发模式相关
    bool trigger_armed;
    float trigger_threshold;
    int pre_trigger_samples;
    int post_trigger_samples;
    int16_t* trigger_buffer;
    int trigger_buffer_size;
    int trigger_buffer_pos;
    bool trigger_occurred;
    
    // 通信缓冲区
    RxBuffer_t rx_buffer;
    TxBuffer_t tx_buffer;
} DeviceState_t;

// --- 全局变量 ---
static volatile bool g_running = true;
static DeviceState_t g_device;
static HANDLE g_data_thread = NULL;
static volatile bool g_data_thread_running = false;

// --- 客户端Socket (用于回调) ---
static SOCKET g_client_socket = INVALID_SOCKET;

// --- 函数声明 ---
int main_program(void);
void init_device_state(void);
void cleanup_device_state(void);
bool load_csv_data(const char* filename);
void handle_client(SOCKET clientSocket);
void process_received_data(SOCKET clientSocket);
void process_command(uint8_t cmd, uint8_t seq, const uint8_t* payload, uint16_t payloadLen, SOCKET clientSocket);
void send_response(uint8_t commandID, uint8_t seq, const uint8_t* payload, uint16_t payloadLen, SOCKET clientSocket);
void send_log_message(uint8_t level, const char* message, SOCKET clientSocket);
DWORD WINAPI data_streaming_thread(LPVOID lpParam);
void generate_data_packet(SOCKET clientSocket);
void handle_trigger_logic(int16_t* samples, uint16_t sample_count);
void send_trigger_event(SOCKET clientSocket);
bool validate_channel_config(uint8_t channel_id, uint32_t sample_rate, uint8_t format);
void frame_received_callback(const uint8_t* frame, uint16_t frameLen);
void print_build_info(void);

#ifdef DEBUG
void print_memory_usage(void);
#endif

#ifndef _WIN32
void unix_signal_handler(int sig);
#else
BOOL WINAPI ConsoleCtrlHandler(DWORD dwCtrlType);
#endif

// --- 主函数 ---
int main(int argc, char* argv[]) {
    // 解析命令行参数
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            // 自定义端口 - 注意这里需要动态分配或使用全局变量
            // 简化处理，这里直接修改不太合适，应该用全局变量
            printf("注意: 端口参数 %s (当前版本暂不支持运行时修改)\n", argv[++i]);
        } else if (strcmp(argv[i], "--csv") == 0 && i + 1 < argc) {
            // 自定义CSV文件 - 同样的问题
            printf("注意: CSV文件参数 %s (当前版本暂不支持运行时修改)\n", argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("用法: %s [选项]\n", argv[0]);
            printf("选项:\n");
            printf("  --port <端口>     设置监听端口 (默认: %s)\n", DEFAULT_PORT);
            printf("  --csv <文件>      指定CSV数据文件 (默认: %s)\n", SAMPLE_DATA_FILE);
            printf("  --help, -h        显示此帮助信息\n");
            printf("  --version         显示版本信息\n");
            printf("  --info            显示编译信息\n");
            printf("\n");
            printf("示例:\n");
            printf("  %s --port 9002 --csv my_data.csv\n", argv[0]);
            return 0;
        } else if (strcmp(argv[i], "--version") == 0) {
            printf("Test-Sender v2.0\n");
            printf("协议版本: V6\n");
            printf("编译时间: %s %s\n", __DATE__, __TIME__);
            #ifdef DEBUG
            printf("构建类型: Debug\n");
            #else
            printf("构建类型: Release\n");
            #endif
            return 0;
        } else if (strcmp(argv[i], "--info") == 0) {
            print_build_info();
            return 0;
        } else {
            printf("未知选项: %s\n", argv[i]);
            printf("使用 --help 查看可用选项\n");
            return 1;
        }
    }
    
    // 注册信号处理
    #ifndef _WIN32
    signal(SIGINT, unix_signal_handler);
    signal(SIGTERM, unix_signal_handler);
    #else
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
    #endif
    
    // 启动主程序逻辑
    return main_program();
}

// --- 设备状态初始化 ---
void init_device_state(void) {
    memset(&g_device, 0, sizeof(g_device));
    
    g_device.mode = MODE_CONTINUOUS;
    g_device.stream_status = STATUS_STOPPED;
    g_device.seq_counter = 0;
    g_device.timestamp_ms = 0;
    g_device.device_error = false;
    g_device.error_code = 0;
    
    // 初始化通道
    g_device.num_channels = 2;
    
    // 通道0 - 电压
    g_device.channels[0].channel_id = 0;
    g_device.channels[0].max_sample_rate_hz = 100000;
    g_device.channels[0].supported_formats_mask = 0x01 | 0x02; // int16, int32
    strcpy(g_device.channels[0].name, "Voltage");
    g_device.channels[0].enabled = false;
    g_device.channels[0].current_sample_rate = 0;
    g_device.channels[0].current_format = 0x01;
    
    // 通道1 - 电流
    g_device.channels[1].channel_id = 1;
    g_device.channels[1].max_sample_rate_hz = 100000;
    g_device.channels[1].supported_formats_mask = 0x01 | 0x02;
    strcpy(g_device.channels[1].name, "Current");
    g_device.channels[1].enabled = false;
    g_device.channels[1].current_sample_rate = 0;
    g_device.channels[1].current_format = 0x01;
    
    // 触发相关初始化
    g_device.trigger_armed = false;
    g_device.trigger_threshold = 1000.0f;
    g_device.pre_trigger_samples = 1000;
    g_device.post_trigger_samples = 1000;
    g_device.trigger_buffer_size = 4096;
    g_device.trigger_buffer = (int16_t*)malloc(g_device.trigger_buffer_size * sizeof(int16_t));
    g_device.trigger_buffer_pos = 0;
    g_device.trigger_occurred = false;
    
    // 初始化IO缓冲区
    initRxBuffer(&g_device.rx_buffer);
    initTxBuffer(&g_device.tx_buffer);
    
    // CSV数据初始化
    g_device.csv_data = NULL;
    g_device.csv_rows = 0;
    g_device.current_csv_row = 0;
}

// --- 清理资源 ---
void cleanup_device_state(void) {
    if (g_device.trigger_buffer) {
        free(g_device.trigger_buffer);
        g_device.trigger_buffer = NULL;
    }
    
    if (g_device.csv_data) {
        for (int i = 0; i < g_device.csv_rows; i++) {
            if (g_device.csv_data[i]) {
                free(g_device.csv_data[i]);
            }
        }
        free(g_device.csv_data);
        g_device.csv_data = NULL;
    }
    
    // 停止数据线程
    if (g_data_thread) {
        g_data_thread_running = false;
        WaitForSingleObject(g_data_thread, 5000);
        CloseHandle(g_data_thread);
        g_data_thread = NULL;
    }
}

// --- 加载CSV数据 ---
bool load_csv_data(const char* filename) {
    FILE* file = fopen(filename, "r");
    if (!file) {
        return false;
    }
    
    // 读取文件内容
    size_t bytes_read = fread(g_device.csv_buffer, 1, CSV_BUFFER_SIZE - 1, file);
    g_device.csv_buffer[bytes_read] = '\0';
    fclose(file);
    
    // 简单解析CSV (假设2列数据，逗号分隔)
    char* line = strtok(g_device.csv_buffer, "\r\n");
    int row_count = 0;
    
    // 先统计行数
    char* temp_buffer = strdup(g_device.csv_buffer);
    char* temp_line = strtok(temp_buffer, "\r\n");
    while (temp_line && row_count < 10000) {
        if (temp_line[0] != '#' && strlen(temp_line) > 0) {
            row_count++;
        }
        temp_line = strtok(NULL, "\r\n");
    }
    free(temp_buffer);
    
    if (row_count == 0) {
        return false;
    }
    
    // 分配内存
    g_device.csv_data = (float**)malloc(row_count * sizeof(float*));
    for (int i = 0; i < row_count; i++) {
        g_device.csv_data[i] = (float*)malloc(2 * sizeof(float));
    }
    
    // 重新解析并存储数据
    strcpy(g_device.csv_buffer, "");
    file = fopen(filename, "r");
    bytes_read = fread(g_device.csv_buffer, 1, CSV_BUFFER_SIZE - 1, file);
    g_device.csv_buffer[bytes_read] = '\0';
    fclose(file);
    
    line = strtok(g_device.csv_buffer, "\r\n");
    int current_row = 0;
    
    while (line && current_row < row_count) {
        if (line[0] != '#' && strlen(line) > 0) {
            char* token1 = strtok(line, ",");
            char* token2 = strtok(NULL, ",");
            
            if (token1 && token2) {
                g_device.csv_data[current_row][0] = (float)atof(token1);
                g_device.csv_data[current_row][1] = (float)atof(token2);
                current_row++;
            }
        }
        line = strtok(NULL, "\r\n");
    }
    
    g_device.csv_rows = current_row;
    printf("已加载CSV数据: %d 行\n", g_device.csv_rows);
    
    return true;
}

// --- 客户端处理 ---
void handle_client(SOCKET clientSocket) {
    g_client_socket = clientSocket;
    
    // 设置非阻塞模式
    u_long mode = 1;
    ioctlsocket(clientSocket, FIONBIO, &mode);
    
    // 重置设备状态
    g_device.stream_status = STATUS_STOPPED;
    g_device.timestamp_ms = (uint32_t)time(NULL) * 1000;
    
    while (g_running) {
        process_received_data(clientSocket);
        
        // 检查是否需要发送数据
        if (g_device.stream_status == STATUS_RUNNING && g_device.mode == MODE_CONTINUOUS) {
            static DWORD last_send_time = 0;
            DWORD current_time = GetTickCount();
            
            if (current_time - last_send_time >= DATA_SEND_INTERVAL_MS) {
                generate_data_packet(clientSocket);
                last_send_time = current_time;
            }
        }
        
        Sleep(1); // 避免CPU占用过高
    }
    
    // 停止数据线程
    if (g_data_thread) {
        g_data_thread_running = false;
        WaitForSingleObject(g_data_thread, 1000);
        CloseHandle(g_data_thread);
        g_data_thread = NULL;
    }
    
    closesocket(clientSocket);
    g_client_socket = INVALID_SOCKET;
}

// --- 处理接收到的数据 ---
void process_received_data(SOCKET clientSocket) {
    char recv_buffer[4096];
    
    int bytes_received = recv(clientSocket, recv_buffer, sizeof(recv_buffer), 0);
    if (bytes_received > 0) {
        printf("收到 %d 字节数据\n", bytes_received);
        
        // 将数据喂给接收缓冲区
        uint16_t fed = feedRxBuffer(&g_device.rx_buffer, (const uint8_t*)recv_buffer, (uint16_t)bytes_received);
        if (fed < bytes_received) {
            printf("警告: RX缓冲区空间不足，丢失 %d 字节\n", bytes_received - fed);
        }
        
        // 尝试解析帧
        tryParseFramesFromRx(&g_device.rx_buffer, frame_received_callback);
        
    } else if (bytes_received == 0) {
        printf("连接已关闭\n");
        g_running = false;
    } else {
        int error = WSAGetLastError();
        if (error != WSAEWOULDBLOCK) {
            printf("recv错误: %d\n", error);
            g_running = false;
        }
    }
}

// --- 帧接收回调 ---
void frame_received_callback(const uint8_t* frame, uint16_t frameLen) {
    uint8_t cmd, seq;
    uint8_t payload[MAX_FRAME_SIZE];
    uint16_t payloadLen;
    
    if (parseFrame(frame, frameLen, &cmd, &seq, payload, &payloadLen) == 0) {
        printf("解析帧成功: CMD=0x%02X, Seq=%u, PayloadLen=%u\n", cmd, seq, payloadLen);
        process_command(cmd, seq, payload, payloadLen, g_client_socket);
    } else {
        printf("帧解析失败\n");
        send_log_message(3, "Frame parsing failed", g_client_socket);
    }
}

// --- 命令处理 ---
void process_command(uint8_t cmd, uint8_t seq, const uint8_t* payload, uint16_t payloadLen, SOCKET clientSocket) {
    switch (cmd) {
        case CMD_PING: { // 0x01
            uint64_t id = DEVICE_UNIQUE_ID;
            send_response(CMD_PONG, seq, (uint8_t*)&id, sizeof(id), clientSocket);
            printf("响应PING命令\n");
            break;
        }
        
        case CMD_GET_STATUS: { // 0x02
            uint8_t status_payload[8] = {0};
            status_payload[0] = (g_device.mode == MODE_CONTINUOUS) ? 0x00 : 0x01;
            status_payload[1] = (g_device.stream_status == STATUS_RUNNING) ? 0x01 : 0x00;
            status_payload[2] = g_device.device_error ? 0x01 : 0x00;
            status_payload[3] = g_device.error_code;
            // 4-7字节预留
            send_response(CMD_STATUS_RESPONSE, seq, status_payload, sizeof(status_payload), clientSocket);
            printf("响应状态查询\n");
            break;
        }
        
        case CMD_GET_DEVICE_INFO: { // 0x03
            uint8_t info_payload[512];
            uint16_t offset = 0;
            
            // 协议版本
            info_payload[offset++] = 6;
            
            // 固件版本
            uint16_t fw_version = 0x0200; // v2.0
            memcpy(info_payload + offset, &fw_version, sizeof(fw_version));
            offset += sizeof(fw_version);
            
            // 通道数
            info_payload[offset++] = g_device.num_channels;
            
            // 通道能力
            for (int i = 0; i < g_device.num_channels; i++) {
                ChannelInfo_t* ch = &g_device.channels[i];
                
                info_payload[offset++] = ch->channel_id;
                memcpy(info_payload + offset, &ch->max_sample_rate_hz, sizeof(ch->max_sample_rate_hz));
                offset += sizeof(ch->max_sample_rate_hz);
                memcpy(info_payload + offset, &ch->supported_formats_mask, sizeof(ch->supported_formats_mask));
                offset += sizeof(ch->supported_formats_mask);
                
                uint8_t name_len = (uint8_t)strlen(ch->name);
                info_payload[offset++] = name_len;
                memcpy(info_payload + offset, ch->name, name_len);
                offset += name_len;
            }
            
            send_response(CMD_DEVICE_INFO_RESPONSE, seq, info_payload, offset, clientSocket);
            printf("响应设备信息查询\n");
            break;
        }
        
        case CMD_SET_MODE_CONTINUOUS: { // 0x10
            g_device.mode = MODE_CONTINUOUS;
            g_device.trigger_armed = false;
            send_response(CMD_ACK, seq, NULL, 0, clientSocket);
            send_log_message(1, "Switched to continuous mode", clientSocket);
            printf("设置为连续模式\n");
            break;
        }
        
        case CMD_SET_MODE_TRIGGER: { // 0x11
            g_device.mode = MODE_TRIGGER;
            g_device.trigger_armed = true;
            g_device.trigger_occurred = false;
            send_response(CMD_ACK, seq, NULL, 0, clientSocket);
            send_log_message(1, "Switched to trigger mode", clientSocket);
            printf("设置为触发模式\n");
            break;
        }
        
        case CMD_START_STREAM: { // 0x12
            g_device.stream_status = STATUS_RUNNING;
            g_device.timestamp_ms = (uint32_t)time(NULL) * 1000;
            
            // 启动数据线程
            if (!g_data_thread && g_device.mode == MODE_CONTINUOUS) {
                g_data_thread_running = true;
                g_data_thread = CreateThread(NULL, 0, data_streaming_thread, (LPVOID)clientSocket, 0, NULL);
            }
            
            send_response(CMD_ACK, seq, NULL, 0, clientSocket);
            send_log_message(1, "Stream started", clientSocket);
            printf("开始数据流\n");
            break;
        }
        
        case CMD_STOP_STREAM: { // 0x13
            g_device.stream_status = STATUS_STOPPED;
            
            // 停止数据线程
            if (g_data_thread) {
                g_data_thread_running = false;
                WaitForSingleObject(g_data_thread, 1000);
                CloseHandle(g_data_thread);
                g_data_thread = NULL;
            }
            
            send_response(CMD_ACK, seq, NULL, 0, clientSocket);
            send_log_message(1, "Stream stopped", clientSocket);
            printf("停止数据流\n");
            break;
        }
        
        case CMD_CONFIGURE_STREAM: { // 0x14
            if (payloadLen < 1) {
                uint8_t err_payload[] = {0x01, 0x01}; // 参数错误
                send_response(CMD_NACK, seq, err_payload, sizeof(err_payload), clientSocket);
                break;
            }
            
            uint8_t num_configs = payload[0];
            uint16_t offset = 1;
            bool config_error = false;
            
            printf("配置 %u 个通道:\n", num_configs);
            
            for (uint8_t i = 0; i < num_configs && !config_error; i++) {
                if (offset + 6 > payloadLen) {
                    config_error = true;
                    break;
                }
                
                uint8_t channel_id = payload[offset];
                uint32_t sample_rate = *(uint32_t*)(payload + offset + 1);
                uint8_t sample_format = payload[offset + 5];
                offset += 6;
                
                printf("  通道%u: %u Hz, 格式0x%02X\n", channel_id, sample_rate, sample_format);
                
                if (!validate_channel_config(channel_id, sample_rate, sample_format)) {
                    config_error = true;
                    break;
                }
                
                // 应用配置
                if (channel_id < g_device.num_channels) {
                    g_device.channels[channel_id].enabled = (sample_rate > 0);
                    g_device.channels[channel_id].current_sample_rate = sample_rate;
                    g_device.channels[channel_id].current_format = sample_format;
                }
            }
            
            if (config_error) {
                uint8_t err_payload[] = {0x01, 0x02}; // 参数错误-通道配置无效
                send_response(CMD_NACK, seq, err_payload, sizeof(err_payload), clientSocket);
            } else {
                send_response(CMD_ACK, seq, NULL, 0, clientSocket);
                send_log_message(1, "Stream configuration updated", clientSocket);
            }
            break;
        }
        
        case CMD_REQUEST_BUFFERED_DATA: { // 0x42
            if (g_device.mode != MODE_TRIGGER) {
                uint8_t err_payload[] = {0x02, 0x01}; // 状态错误
                send_response(CMD_NACK, seq, err_payload, sizeof(err_payload), clientSocket);
                break;
            }
            
            if (!g_device.trigger_occurred) {
                uint8_t err_payload[] = {0x02, 0x02}; // 状态错误-未触发
                send_response(CMD_NACK, seq, err_payload, sizeof(err_payload), clientSocket);
                break;
            }
            
            send_response(CMD_ACK, seq, NULL, 0, clientSocket);
            send_log_message(1, "Sending buffered trigger data", clientSocket);
            
            // 模拟发送触发数据
            for (int i = 0; i < 5; i++) {
                generate_data_packet(clientSocket);
                Sleep(10);
            }
            
            // 发送传输完成
            send_response(CMD_BUFFER_TRANSFER_COMPLETE, g_device.seq_counter++, NULL, 0, clientSocket);
            printf("触发数据传输完成\n");
            break;
        }
        
        default: {
            printf("未知命令: 0x%02X\n", cmd);
            uint8_t err_payload[] = {0x05, 0x00}; // 命令不支持
            send_response(CMD_NACK, seq, err_payload, sizeof(err_payload), clientSocket);
            break;
        }
    }
}

// --- 验证通道配置 ---
bool validate_channel_config(uint8_t channel_id, uint32_t sample_rate, uint8_t format) {
    if (channel_id >= g_device.num_channels) {
        return false;
    }
    
    ChannelInfo_t* ch = &g_device.channels[channel_id];
    
    if (sample_rate > ch->max_sample_rate_hz) {
        return false;
    }
    
    if (format != 0x00 && !(ch->supported_formats_mask & format)) {
        return false;
    }
    
    return true;
}

// --- 发送响应 ---
void send_response(uint8_t commandID, uint8_t seq, const uint8_t* payload, uint16_t payloadLen, SOCKET clientSocket) {
    uint8_t frameBuf[MAX_FRAME_SIZE];
    uint16_t frameLen = MAX_FRAME_SIZE;

    if (buildFrame(commandID, seq, payload, payloadLen, frameBuf, &frameLen) == 0) {
        int iSendResult = send(clientSocket, (const char*)frameBuf, frameLen, 0);
        if (iSendResult == SOCKET_ERROR) {
            printf("发送失败: %d\n", WSAGetLastError());
        } else {
            printf("发送响应: CMD=0x%02X, Len=%u\n", commandID, frameLen);
        }
    } else {
        printf("创建响应帧失败: CMD=0x%02X\n", commandID);
    }
}

// --- 发送日志消息 ---
void send_log_message(uint8_t level, const char* message, SOCKET clientSocket) {
    if (clientSocket == INVALID_SOCKET) {
        return;
    }
    
    uint8_t payload[256];
    uint8_t msg_len = (uint8_t)strlen(message);
    
    if (msg_len > 253) msg_len = 253;
    
    payload[0] = level;
    payload[1] = msg_len;
    memcpy(payload + 2, message, msg_len);
    
    send_response(CMD_LOG_MESSAGE, g_device.seq_counter++, payload, msg_len + 2, clientSocket);
}

// --- 生成数据包 ---
void generate_data_packet(SOCKET clientSocket) {
    uint8_t payload[2048];
    uint16_t payload_offset = 0;
    uint16_t enabled_channels = 0;
    uint16_t sample_count = 0;
    
    // 计算启用的通道和采样数
    for (int i = 0; i < g_device.num_channels; i++) {
        if (g_device.channels[i].enabled) {
            enabled_channels |= (1 << i);
            if (sample_count == 0) {
                // 简化：所有通道使用相同的采样数
                sample_count = (g_device.channels[i].current_sample_rate * DATA_SEND_INTERVAL_MS) / 1000;
                if (sample_count == 0) sample_count = 1;
                if (sample_count > 100) sample_count = 100; // 限制单包采样数
            }
        }
    }
    
    if (enabled_channels == 0 || sample_count == 0) {
        return; // 没有启用的通道
    }
    
    // 填充数据包头
    memcpy(payload + payload_offset, &g_device.timestamp_ms, sizeof(g_device.timestamp_ms));
    payload_offset += sizeof(g_device.timestamp_ms);
    
    memcpy(payload + payload_offset, &enabled_channels, sizeof(enabled_channels));
    payload_offset += sizeof(enabled_channels);
    
    memcpy(payload + payload_offset, &sample_count, sizeof(sample_count));
    payload_offset += sizeof(sample_count);
    
    // 生成并填充数据 (非交错格式)
    int16_t* samples_buffer = (int16_t*)malloc(sample_count * g_device.num_channels * sizeof(int16_t));
    
    for (int i = 0; i < g_device.num_channels; i++) {
        if (!(enabled_channels & (1 << i))) {
            continue;
        }
        
        // 生成该通道的数据
        for (uint16_t s = 0; s < sample_count; s++) {
            int16_t sample_value;
            
            if (g_device.csv_data && g_device.csv_rows > 0) {
                // 使用CSV数据
                int csv_index = g_device.current_csv_row % g_device.csv_rows;
                sample_value = (int16_t)(g_device.csv_data[csv_index][i] * 100); // 放大100倍
                g_device.current_csv_row++;
            } else {
                // 生成模拟数据：正弦波+噪声
                float t = (g_device.timestamp_ms + s * 1000.0f / g_device.channels[i].current_sample_rate) / 1000.0f;
                float freq = (i == 0) ? 50.0f : 60.0f; // 通道0:50Hz, 通道1:60Hz
                float amplitude = (i == 0) ? 1000.0f : 800.0f;
                float noise = ((rand() % 100) - 50) * 0.1f;
                sample_value = (int16_t)(amplitude * sinf(2.0f * 3.14159f * freq * t) + noise);
            }
            
            samples_buffer[i * sample_count + s] = sample_value;
        }
        
        // 触发逻辑检测
        if (g_device.mode == MODE_TRIGGER && g_device.trigger_armed && i == 0) {
            handle_trigger_logic(&samples_buffer[i * sample_count], sample_count);
        }
        
        // 将通道数据拷贝到payload
        memcpy(payload + payload_offset, &samples_buffer[i * sample_count], sample_count * sizeof(int16_t));
        payload_offset += sample_count * sizeof(int16_t);
    }
    
    free(samples_buffer);
    
    // 发送数据包
    send_response(CMD_DATA_PACKET, g_device.seq_counter++, payload, payload_offset, clientSocket);
    
    g_device.timestamp_ms += DATA_SEND_INTERVAL_MS;
    
    // printf("发送数据包: 通道=0x%04X, 采样数=%u, 时间戳=%u\n", 
    //        enabled_channels, sample_count, g_device.timestamp_ms - DATA_SEND_INTERVAL_MS);
}

// --- 触发逻辑处理 ---
void handle_trigger_logic(int16_t* samples, uint16_t sample_count) {
    static int16_t last_sample = 0;
    
    if (!g_device.trigger_armed || g_device.trigger_occurred) {
        return;
    }
    
    // 将新数据添加到循环缓冲区
    for (uint16_t i = 0; i < sample_count; i++) {
        g_device.trigger_buffer[g_device.trigger_buffer_pos] = samples[i];
        g_device.trigger_buffer_pos = (g_device.trigger_buffer_pos + 1) % g_device.trigger_buffer_size;
        
        // 检查触发条件 (简单上升沿触发)
        if (!g_device.trigger_occurred && samples[i] > g_device.trigger_threshold) {
            if (last_sample <= g_device.trigger_threshold) {
                printf("触发事件检测到! 值: %d > 阈值: %.1f\n", samples[i], g_device.trigger_threshold);
                g_device.trigger_occurred = true;
                g_device.trigger_armed = false;
                send_trigger_event(g_client_socket);
                break;
            }
        }
        last_sample = samples[i];
    }
}

// --- 发送触发事件 ---
void send_trigger_event(SOCKET clientSocket) {
    uint8_t event_payload[16]; // 增加缓冲区大小
    uint32_t trigger_timestamp = g_device.timestamp_ms;
    uint16_t trigger_channel = 0; // 触发源通道
    uint32_t pre_samples = g_device.pre_trigger_samples;
    uint32_t post_samples = g_device.post_trigger_samples;
    
    uint16_t offset = 0;
    memcpy(event_payload + offset, &trigger_timestamp, sizeof(trigger_timestamp));
    offset += sizeof(trigger_timestamp);
    
    memcpy(event_payload + offset, &trigger_channel, sizeof(trigger_channel));
    offset += sizeof(trigger_channel);
    
    memcpy(event_payload + offset, &pre_samples, sizeof(pre_samples));
    offset += sizeof(pre_samples);
    
    memcpy(event_payload + offset, &post_samples, sizeof(post_samples));
    offset += sizeof(post_samples);
    
    send_response(CMD_EVENT_TRIGGERED, g_device.seq_counter++, event_payload, offset, clientSocket);
    send_log_message(2, "Trigger event detected", clientSocket);
}

// --- 数据流线程 (备用方案) ---
DWORD WINAPI data_streaming_thread(LPVOID lpParam) {
    SOCKET clientSocket = (SOCKET)lpParam;
    
    printf("数据流线程启动\n");
    
    while (g_data_thread_running && g_device.stream_status == STATUS_RUNNING) {
        if (g_device.mode == MODE_CONTINUOUS) {
            generate_data_packet(clientSocket);
        }
        
        Sleep(DATA_SEND_INTERVAL_MS);
    }
    
    printf("数据流线程退出\n");
    return 0;
}

// --- 主程序逻辑分离 ---
int main_program() {
    WSADATA wsaData;
    int iResult;

    printf("=== 高保真下位机模拟器 (test-sender) v2.0 ===\n");
    printf("协议版本: V6\n");
    printf("端口: %s\n", DEFAULT_PORT);
    printf("CSV文件: %s\n\n", SAMPLE_DATA_FILE);

    // 初始化随机数种子
    srand((unsigned int)time(NULL));

    // 初始化设备状态
    init_device_state();

    // 加载CSV测试数据
    if (!load_csv_data(SAMPLE_DATA_FILE)) {
        printf("警告: 无法加载CSV文件 '%s'，将使用内置测试数据\n", SAMPLE_DATA_FILE);
    }

    // 初始化 Winsock
    iResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (iResult != 0) {
        printf("错误: WSAStartup 失败: %d\n", iResult);
        cleanup_device_state();
        return 1;
    }

    struct addrinfo *result = NULL, hints;
    ZeroMemory(&hints, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_PASSIVE;

    // 解析服务器地址和端口
    iResult = getaddrinfo(NULL, DEFAULT_PORT, &hints, &result);
    if (iResult != 0) {
        printf("错误: getaddrinfo 失败: %d\n", iResult);
        WSACleanup();
        cleanup_device_state();
        return 1;
    }

    // 创建监听Socket
    SOCKET listenSocket = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (listenSocket == INVALID_SOCKET) {
        printf("错误: socket() 失败: %d\n", (int)WSAGetLastError());
        freeaddrinfo(result);
        WSACleanup();
        cleanup_device_state();
        return 1;
    }

    // 设置Socket选项
    int reuse = 1;
    setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));

    // 绑定Socket
    iResult = bind(listenSocket, result->ai_addr, (int)result->ai_addrlen);
    if (iResult == SOCKET_ERROR) {
        printf("错误: bind 失败: %d\n", WSAGetLastError());
        freeaddrinfo(result);
        closesocket(listenSocket);
        WSACleanup();
        cleanup_device_state();
        return 1;
    }
    freeaddrinfo(result);

    // 开始监听
    if (listen(listenSocket, SOMAXCONN) == SOCKET_ERROR) {
        printf("错误: listen 失败: %d\n", (int)WSAGetLastError());
        closesocket(listenSocket);
        WSACleanup();
        cleanup_device_state();
        return 1;
    }

    printf("模拟器正在监听端口 %s，等待连接...\n", DEFAULT_PORT);
    printf("按 Ctrl+C 退出程序\n\n");

    while (g_running) {
        // 接受客户端连接
        SOCKET clientSocket = accept(listenSocket, NULL, NULL);
        if (clientSocket == INVALID_SOCKET) {
            if (g_running) {
                int error = WSAGetLastError();
                if (error != WSAEINTR) {
                    printf("错误: accept 失败: %d\n", error);
                }
            }
            continue;
        }
        
        printf("客户端已连接 (Socket: %lld)\n", (long long)clientSocket);
        
        handle_client(clientSocket);
        
        printf("客户端已断开\n\n");
    }

    printf("正在关闭服务器...\n");
    closesocket(listenSocket);
    WSACleanup();
    cleanup_device_state();
    
    printf("模拟器已退出\n");
    return 0;
}

// --- 程序入口点增强 (Windows) ---
#ifdef _WIN32
int WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    // 分配控制台窗口 (如果以Windows程序启动)
    if (AllocConsole()) {
        freopen_s(NULL, "CONOUT$", "w", stdout);
        freopen_s(NULL, "CONIN$", "r", stdin);
        freopen_s(NULL, "CONERR$", "w", stderr);
    }
    
    // 简单解析命令行 (Windows版本)
    if (lpCmdLine && strlen(lpCmdLine) > 0) {
        printf("命令行参数: %s\n", lpCmdLine);
    }
    
    // 注册Ctrl+C处理程序
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
    
    // 调用主程序
    return main_program();
}
#endif

// --- Unix信号处理 ---
#ifndef _WIN32
void unix_signal_handler(int sig) {
    switch (sig) {
        case SIGINT:
        case SIGTERM:
            printf("\n收到退出信号，正在关闭模拟器...\n");
            g_running = false;
            break;
        default:
            break;
    }
}
#endif

// --- Ctrl+C 信号处理 ---
BOOL WINAPI ConsoleCtrlHandler(DWORD dwCtrlType) {
    switch (dwCtrlType) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
            printf("\n正在退出模拟器...\n");
            g_running = false;
            return TRUE;
        default:
            return FALSE;
    }
}

// --- 版本信息和编译信息 ---
void print_build_info() {
    printf("=== 编译信息 ===\n");
    printf("版本: v2.0\n");
    printf("编译时间: %s %s\n", __DATE__, __TIME__);
    printf("编译器: ");
    #if defined(__GNUC__)
    printf("GCC %d.%d.%d\n", __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__);
    #elif defined(_MSC_VER)
    printf("MSVC %d\n", _MSC_VER);
    #else
    printf("Unknown\n");
    #endif
    
    printf("平台: ");
    #ifdef _WIN32
    printf("Windows");
    #ifdef _WIN64
    printf(" x64");
    #else
    printf(" x86");
    #endif
    #elif defined(__linux__)
    printf("Linux");
    #elif defined(__APPLE__)
    printf("macOS");
    #else
    printf("Unknown");
    #endif
    printf("\n");
    
    printf("特性: ");
    #ifdef DEBUG
    printf("Debug ");
    #endif
    #ifdef USE_MATH_DEFINES
    printf("Math ");
    #endif
    printf("\n");
    
    printf("协议版本: V6\n");
    printf("===============\n\n");
}

// --- 内存使用统计 (调试用) ---
#ifdef DEBUG
void print_memory_usage() {
    printf("=== 内存使用统计 ===\n");
    printf("设备状态: %zu bytes\n", sizeof(DeviceState_t));
    printf("RX缓冲区: %d bytes\n", RX_BUFFER_SIZE);
    printf("TX缓冲区: %d bytes\n", TX_BUFFER_SIZE);
    printf("触发缓冲区: %d bytes\n", g_device.trigger_buffer_size * (int)sizeof(int16_t));
    if (g_device.csv_data) {
        printf("CSV数据: %d rows × 2 × %zu bytes = %zu bytes\n", 
               g_device.csv_rows, sizeof(float), 
               g_device.csv_rows * 2 * sizeof(float));
    }
    printf("==================\n\n");
}
#endif