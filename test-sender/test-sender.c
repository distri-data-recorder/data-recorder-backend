// File: test-sender.c
// Description: High-fidelity lower computer simulator, communicates with data-reader via TCP Socket
// Version: v2.0
// Protocol: V6

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

// --- Configuration ---
#define DEFAULT_PORT "9001"
#define DEVICE_UNIQUE_ID 0x11223344AABBCCDDULL
#define SAMPLE_DATA_FILE "sample_data.csv"
#define DATA_SEND_INTERVAL_MS 10
#define MAX_CLIENTS 1
#define MAX_CHANNELS 4
#define CSV_BUFFER_SIZE 32768

// --- Protocol Command Definitions (consistent with protocol specification) ---
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

// --- Device Capabilities Definition ---
typedef struct {
    uint8_t channel_id;
    uint32_t max_sample_rate_hz;
    uint16_t supported_formats_mask;
    char name[32];
    bool enabled;
    uint32_t current_sample_rate;
    uint8_t current_format;
} ChannelInfo_t;

// --- Global State ---
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

    // Channel Configuration
    ChannelInfo_t channels[MAX_CHANNELS];
    uint8_t num_channels;

    // CSV Data Buffer
    char csv_buffer[CSV_BUFFER_SIZE];
    int csv_rows;
    int current_csv_row;
    float** csv_data;

    // Trigger Mode Related
    bool trigger_armed;
    float trigger_threshold;
    int pre_trigger_samples;
    int post_trigger_samples;
    int16_t* trigger_buffer;
    int trigger_buffer_size;
    int trigger_buffer_pos;
    bool trigger_occurred;

    // Communication Buffers
    RxBuffer_t rx_buffer;
    TxBuffer_t tx_buffer;
} DeviceState_t;

// --- Global Variables ---
static volatile bool g_running = true;
static DeviceState_t g_device;
static HANDLE g_data_thread = NULL;
static volatile bool g_data_thread_running = false;

// --- Client Socket (for callbacks) ---
static SOCKET g_client_socket = INVALID_SOCKET;

// --- Function Declarations ---
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

// --- Main Function ---
int main(int argc, char* argv[]) {
    // Parse command-line arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            // Custom port - Note: dynamic allocation or global variable usage needed
            // Simplified handling, direct modification here is not ideal
            printf("Note: Port parameter %s (runtime modification not supported in current version)\n", argv[++i]);
        } else if (strcmp(argv[i], "--csv") == 0 && i + 1 < argc) {
            // Custom CSV file - Same issue
            printf("Note: CSV file parameter %s (runtime modification not supported in current version)\n", argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: %s [options]\n", argv[0]);
            printf("Options:\n");
            printf("  --port <port>     Set listening port (default: %s)\n", DEFAULT_PORT);
            printf("  --csv <file>      Specify CSV data file (default: %s)\n", SAMPLE_DATA_FILE);
            printf("  --help, -h        Show this help information\n");
            printf("  --version         Show version information\n");
            printf("  --info            Show build information\n");
            printf("\n");
            printf("Examples:\n");
            printf("  %s --port 9002 --csv my_data.csv\n", argv[0]);
            return 0;
        } else if (strcmp(argv[i], "--version") == 0) {
            printf("Test-Sender v2.0\n");
            printf("Protocol Version: V6\n");
            printf("Build Time: %s %s\n", __DATE__, __TIME__);
            #ifdef DEBUG
            printf("Build Type: Debug\n");
            #else
            printf("Build Type: Release\n");
            #endif
            return 0;
        } else if (strcmp(argv[i], "--info") == 0) {
            print_build_info();
            return 0;
        } else {
            printf("Unknown option: %s\n", argv[i]);
            printf("Use --help to view available options\n");
            return 1;
        }
    }

    // Register signal handlers
    #ifndef _WIN32
    signal(SIGINT, unix_signal_handler);
    signal(SIGTERM, unix_signal_handler);
    #else
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
    #endif

    // Start main program logic
    return main_program();
}

// --- Device State Initialization ---
void init_device_state(void) {
    memset(&g_device, 0, sizeof(g_device));

    g_device.mode = MODE_CONTINUOUS;
    g_device.stream_status = STATUS_STOPPED;
    g_device.seq_counter = 0;
    g_device.timestamp_ms = 0;
    g_device.device_error = false;
    g_device.error_code = 0;

    // Initialize channels
    g_device.num_channels = 2;

    // Channel 0 - Voltage
    g_device.channels[0].channel_id = 0;
    g_device.channels[0].max_sample_rate_hz = 100000;
    g_device.channels[0].supported_formats_mask = 0x01 | 0x02; // int16, int32
    strcpy(g_device.channels[0].name, "Voltage");
    g_device.channels[0].enabled = false;
    g_device.channels[0].current_sample_rate = 0;
    g_device.channels[0].current_format = 0x01;

    // Channel 1 - Current
    g_device.channels[1].channel_id = 1;
    g_device.channels[1].max_sample_rate_hz = 100000;
    g_device.channels[1].supported_formats_mask = 0x01 | 0x02;
    strcpy(g_device.channels[1].name, "Current");
    g_device.channels[1].enabled = false;
    g_device.channels[1].current_sample_rate = 0;
    g_device.channels[1].current_format = 0x01;

    // Trigger related initialization
    g_device.trigger_armed = false;
    g_device.trigger_threshold = 1000.0f;
    g_device.pre_trigger_samples = 1000;
    g_device.post_trigger_samples = 1000;
    g_device.trigger_buffer_size = 4096;
    g_device.trigger_buffer = (int16_t*)malloc(g_device.trigger_buffer_size * sizeof(int16_t));
    g_device.trigger_buffer_pos = 0;
    g_device.trigger_occurred = false;

    // Initialize IO buffers
    initRxBuffer(&g_device.rx_buffer);
    initTxBuffer(&g_device.tx_buffer);

    // CSV data initialization
    g_device.csv_data = NULL;
    g_device.csv_rows = 0;
    g_device.current_csv_row = 0;
}

// --- Resource Cleanup ---
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

    // Stop data thread
    if (g_data_thread) {
        g_data_thread_running = false;
        WaitForSingleObject(g_data_thread, 5000);
        CloseHandle(g_data_thread);
        g_data_thread = NULL;
    }
}

// --- Load CSV Data ---
bool load_csv_data(const char* filename) {
    FILE* file = fopen(filename, "r");
    if (!file) {
        return false;
    }

    // Read file content
    size_t bytes_read = fread(g_device.csv_buffer, 1, CSV_BUFFER_SIZE - 1, file);
    g_device.csv_buffer[bytes_read] = '\0';
    fclose(file);

    // Simple CSV parsing (assuming 2 columns, comma-separated)
    char* line = strtok(g_device.csv_buffer, "\r\n");
    int row_count = 0;

    // First pass: count rows
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

    // Allocate memory
    g_device.csv_data = (float**)malloc(row_count * sizeof(float*));
    for (int i = 0; i < row_count; i++) {
        g_device.csv_data[i] = (float*)malloc(2 * sizeof(float));
    }

    // Second pass: parse and store data
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
    printf("Loaded CSV data: %d rows\n", g_device.csv_rows);

    return true;
}

// --- Client Handling ---
void handle_client(SOCKET clientSocket) {
    g_client_socket = clientSocket;

    // Set non-blocking mode
    u_long mode = 1;
    ioctlsocket(clientSocket, FIONBIO, &mode);

    // Reset device state
    g_device.stream_status = STATUS_STOPPED;
    g_device.timestamp_ms = (uint32_t)time(NULL) * 1000;

    while (g_running) {
        process_received_data(clientSocket);

        // Check if data needs to be sent
        if (g_device.stream_status == STATUS_RUNNING && g_device.mode == MODE_CONTINUOUS) {
            static DWORD last_send_time = 0;
            DWORD current_time = GetTickCount();

            if (current_time - last_send_time >= DATA_SEND_INTERVAL_MS) {
                generate_data_packet(clientSocket);
                last_send_time = current_time;
            }
        }

        Sleep(1); // Prevent high CPU usage
    }

    // Stop data thread
    if (g_data_thread) {
        g_data_thread_running = false;
        WaitForSingleObject(g_data_thread, 1000);
        CloseHandle(g_data_thread);
        g_data_thread = NULL;
    }

    closesocket(clientSocket);
    g_client_socket = INVALID_SOCKET;
}

// --- Process Received Data ---
void process_received_data(SOCKET clientSocket) {
    char recv_buffer[4096];

    int bytes_received = recv(clientSocket, recv_buffer, sizeof(recv_buffer), 0);
    if (bytes_received > 0) {
        printf("Received %d bytes of data\n", bytes_received);

        // Feed data to receive buffer
        uint16_t fed = feedRxBuffer(&g_device.rx_buffer, (const uint8_t*)recv_buffer, (uint16_t)bytes_received);
        if (fed < bytes_received) {
            printf("Warning: RX buffer overflow, %d bytes lost\n", bytes_received - fed);
        }

        // Attempt to parse frames
        tryParseFramesFromRx(&g_device.rx_buffer, frame_received_callback);

    } else if (bytes_received == 0) {
        printf("Connection closed\n");
        g_running = false;
    } else {
        int error = WSAGetLastError();
        if (error != WSAEWOULDBLOCK) {
            printf("recv error: %d\n", error);
            g_running = false;
        }
    }
}

// --- Frame Reception Callback ---
void frame_received_callback(const uint8_t* frame, uint16_t frameLen) {
    uint8_t cmd, seq;
    uint8_t payload[MAX_FRAME_SIZE];
    uint16_t payloadLen;

    if (parseFrame(frame, frameLen, &cmd, &seq, payload, &payloadLen) == 0) {
        printf("Frame parsed successfully: CMD=0x%02X, Seq=%u, PayloadLen=%u\n", cmd, seq, payloadLen);
        process_command(cmd, seq, payload, payloadLen, g_client_socket);
    } else {
        printf("Frame parsing failed\n");
        send_log_message(3, "Frame parsing failed", g_client_socket);
    }
}

// --- Command Processing ---
void process_command(uint8_t cmd, uint8_t seq, const uint8_t* payload, uint16_t payloadLen, SOCKET clientSocket) {
    switch (cmd) {
        case CMD_PING: { // 0x01
            uint64_t id = DEVICE_UNIQUE_ID;
            send_response(CMD_PONG, seq, (uint8_t*)&id, sizeof(id), clientSocket);
            printf("Responded to PING command\n");
            break;
        }

        case CMD_GET_STATUS: { // 0x02
            uint8_t status_payload[8] = {0};
            status_payload[0] = (g_device.mode == MODE_CONTINUOUS) ? 0x00 : 0x01;
            status_payload[1] = (g_device.stream_status == STATUS_RUNNING) ? 0x01 : 0x00;
            status_payload[2] = g_device.device_error ? 0x01 : 0x00;
            status_payload[3] = g_device.error_code;
            // Reserved for bytes 4-7
            send_response(CMD_STATUS_RESPONSE, seq, status_payload, sizeof(status_payload), clientSocket);
            printf("Responded to status query\n");
            break;
        }

        case CMD_GET_DEVICE_INFO: { // 0x03
            uint8_t info_payload[512];
            uint16_t offset = 0;

            // Protocol version
            info_payload[offset++] = 6;

            // Firmware version
            uint16_t fw_version = 0x0200; // v2.0
            memcpy(info_payload + offset, &fw_version, sizeof(fw_version));
            offset += sizeof(fw_version);

            // Number of channels
            info_payload[offset++] = g_device.num_channels;

            // Channel capabilities
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
            printf("Responded to device info query\n");
            break;
        }

        case CMD_SET_MODE_CONTINUOUS: { // 0x10
            g_device.mode = MODE_CONTINUOUS;
            g_device.trigger_armed = false;
            send_response(CMD_ACK, seq, NULL, 0, clientSocket);
            send_log_message(1, "Switched to continuous mode", clientSocket);
            printf("Set to continuous mode\n");
            break;
        }

        case CMD_SET_MODE_TRIGGER: { // 0x11
            g_device.mode = MODE_TRIGGER;
            g_device.trigger_armed = true;
            g_device.trigger_occurred = false;
            send_response(CMD_ACK, seq, NULL, 0, clientSocket);
            send_log_message(1, "Switched to trigger mode", clientSocket);
            printf("Set to trigger mode\n");
            break;
        }

        case CMD_START_STREAM: { // 0x12
            g_device.stream_status = STATUS_RUNNING;
            g_device.timestamp_ms = (uint32_t)time(NULL) * 1000;

            // Start data thread
            if (!g_data_thread && g_device.mode == MODE_CONTINUOUS) {
                g_data_thread_running = true;
                g_data_thread = CreateThread(NULL, 0, data_streaming_thread, (LPVOID)clientSocket, 0, NULL);
            }

            send_response(CMD_ACK, seq, NULL, 0, clientSocket);
            send_log_message(1, "Stream started", clientSocket);
            printf("Data stream started\n");
            break;
        }

        case CMD_STOP_STREAM: { // 0x13
            g_device.stream_status = STATUS_STOPPED;

            // Stop data thread
            if (g_data_thread) {
                g_data_thread_running = false;
                WaitForSingleObject(g_data_thread, 1000);
                CloseHandle(g_data_thread);
                g_data_thread = NULL;
            }

            send_response(CMD_ACK, seq, NULL, 0, clientSocket);
            send_log_message(1, "Stream stopped", clientSocket);
            printf("Data stream stopped\n");
            break;
        }

        case CMD_CONFIGURE_STREAM: { // 0x14
            if (payloadLen < 1) {
                uint8_t err_payload[] = {0x01, 0x01}; // Parameter error
                send_response(CMD_NACK, seq, err_payload, sizeof(err_payload), clientSocket);
                break;
            }

            uint8_t num_configs = payload[0];
            uint16_t offset = 1;
            bool config_error = false;

            printf("Configuring %u channels:\n", num_configs);

            for (uint8_t i = 0; i < num_configs && !config_error; i++) {
                if (offset + 6 > payloadLen) {
                    config_error = true;
                    break;
                }

                uint8_t channel_id = payload[offset];
                uint32_t sample_rate = *(uint32_t*)(payload + offset + 1);
                uint8_t sample_format = payload[offset + 5];
                offset += 6;

                printf("  Channel %u: %u Hz, Format 0x%02X\n", channel_id, sample_rate, sample_format);

                if (!validate_channel_config(channel_id, sample_rate, sample_format)) {
                    config_error = true;
                    break;
                }

                // Apply configuration
                if (channel_id < g_device.num_channels) {
                    g_device.channels[channel_id].enabled = (sample_rate > 0);
                    g_device.channels[channel_id].current_sample_rate = sample_rate;
                    g_device.channels[channel_id].current_format = sample_format;
                }
            }

            if (config_error) {
                uint8_t err_payload[] = {0x01, 0x02}; // Parameter error - invalid channel configuration
                send_response(CMD_NACK, seq, err_payload, sizeof(err_payload), clientSocket);
            } else {
                send_response(CMD_ACK, seq, NULL, 0, clientSocket);
                send_log_message(1, "Stream configuration updated", clientSocket);
            }
            break;
        }

        case CMD_REQUEST_BUFFERED_DATA: { // 0x42
            if (g_device.mode != MODE_TRIGGER) {
                uint8_t err_payload[] = {0x02, 0x01}; // Status error
                send_response(CMD_NACK, seq, err_payload, sizeof(err_payload), clientSocket);
                break;
            }

            if (!g_device.trigger_occurred) {
                uint8_t err_payload[] = {0x02, 0x02}; // Status error - not triggered
                send_response(CMD_NACK, seq, err_payload, sizeof(err_payload), clientSocket);
                break;
            }

            send_response(CMD_ACK, seq, NULL, 0, clientSocket);
            send_log_message(1, "Sending buffered trigger data", clientSocket);

            // Simulate sending trigger data
            for (int i = 0; i < 5; i++) {
                generate_data_packet(clientSocket);
                Sleep(10);
            }

            // Send transfer complete
            send_response(CMD_BUFFER_TRANSFER_COMPLETE, g_device.seq_counter++, NULL, 0, clientSocket);
            printf("Trigger data transfer complete\n");
            break;
        }

        default: {
            printf("Unknown command: 0x%02X\n", cmd);
            uint8_t err_payload[] = {0x05, 0x00}; // Command not supported
            send_response(CMD_NACK, seq, err_payload, sizeof(err_payload), clientSocket);
            break;
        }
    }
}

// --- Validate Channel Configuration ---
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

// --- Send Response ---
void send_response(uint8_t commandID, uint8_t seq, const uint8_t* payload, uint16_t payloadLen, SOCKET clientSocket) {
    uint8_t frameBuf[MAX_FRAME_SIZE];
    uint16_t frameLen = MAX_FRAME_SIZE;

    if (buildFrame(commandID, seq, payload, payloadLen, frameBuf, &frameLen) == 0) {
        int iSendResult = send(clientSocket, (const char*)frameBuf, frameLen, 0);
        if (iSendResult == SOCKET_ERROR) {
            printf("Send failed: %d\n", WSAGetLastError());
        } else {
            printf("Sent response: CMD=0x%02X, Len=%u\n", commandID, frameLen);
        }
    } else {
        printf("Failed to create response frame: CMD=0x%02X\n", commandID);
    }
}

// --- Send Log Message ---
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

// --- Generate Data Packet ---
void generate_data_packet(SOCKET clientSocket) {
    uint8_t payload[2048];
    uint16_t payload_offset = 0;
    uint16_t enabled_channels = 0;
    uint16_t sample_count = 0;

    // Calculate enabled channels and sample count
    for (int i = 0; i < g_device.num_channels; i++) {
        if (g_device.channels[i].enabled) {
            enabled_channels |= (1 << i);
            if (sample_count == 0) {
                // Simplified: all channels use the same sample count
                sample_count = (g_device.channels[i].current_sample_rate * DATA_SEND_INTERVAL_MS) / 1000;
                if (sample_count == 0) sample_count = 1;
                if (sample_count > 100) sample_count = 100; // Limit samples per packet
            }
        }
    }

    if (enabled_channels == 0 || sample_count == 0) {
        return; // No enabled channels
    }

    // Fill data packet header
    memcpy(payload + payload_offset, &g_device.timestamp_ms, sizeof(g_device.timestamp_ms));
    payload_offset += sizeof(g_device.timestamp_ms);

    memcpy(payload + payload_offset, &enabled_channels, sizeof(enabled_channels));
    payload_offset += sizeof(enabled_channels);

    memcpy(payload + payload_offset, &sample_count, sizeof(sample_count));
    payload_offset += sizeof(sample_count);

    // Generate and fill data (non-interleaved format)
    int16_t* samples_buffer = (int16_t*)malloc(sample_count * g_device.num_channels * sizeof(int16_t));

    for (int i = 0; i < g_device.num_channels; i++) {
        if (!(enabled_channels & (1 << i))) {
            continue;
        }

        // Generate data for this channel
        for (uint16_t s = 0; s < sample_count; s++) {
            int16_t sample_value;

            if (g_device.csv_data && g_device.csv_rows > 0) {
                // Use CSV data
                int csv_index = g_device.current_csv_row % g_device.csv_rows;
                sample_value = (int16_t)(g_device.csv_data[csv_index][i] * 100); // Amplify by 100
                g_device.current_csv_row++;
            } else {
                // Generate simulated data: sine wave + noise
                float t = (g_device.timestamp_ms + s * 1000.0f / g_device.channels[i].current_sample_rate) / 1000.0f;
                float freq = (i == 0) ? 50.0f : 60.0f; // Channel 0: 50Hz, Channel 1: 60Hz
                float amplitude = (i == 0) ? 1000.0f : 800.0f;
                float noise = ((rand() % 100) - 50) * 0.1f;
                sample_value = (int16_t)(amplitude * sinf(2.0f * 3.14159f * freq * t) + noise);
            }

            samples_buffer[i * sample_count + s] = sample_value;
        }

        // Trigger logic detection
        if (g_device.mode == MODE_TRIGGER && g_device.trigger_armed && i == 0) {
            handle_trigger_logic(&samples_buffer[i * sample_count], sample_count);
        }

        // Copy channel data to payload
        memcpy(payload + payload_offset, &samples_buffer[i * sample_count], sample_count * sizeof(int16_t));
        payload_offset += sample_count * sizeof(int16_t);
    }

    free(samples_buffer);

    // Send data packet
    send_response(CMD_DATA_PACKET, g_device.seq_counter++, payload, payload_offset, clientSocket);

    g_device.timestamp_ms += DATA_SEND_INTERVAL_MS;

    // printf("Sent data packet: Channels=0x%04X, Samples=%u, Timestamp=%u\n",
    //        enabled_channels, sample_count, g_device.timestamp_ms - DATA_SEND_INTERVAL_MS);
}

// --- Trigger Logic Handling ---
void handle_trigger_logic(int16_t* samples, uint16_t sample_count) {
    static int16_t last_sample = 0;

    if (!g_device.trigger_armed || g_device.trigger_occurred) {
        return;
    }

    // Add new data to circular buffer
    for (uint16_t i = 0; i < sample_count; i++) {
        g_device.trigger_buffer[g_device.trigger_buffer_pos] = samples[i];
        g_device.trigger_buffer_pos = (g_device.trigger_buffer_pos + 1) % g_device.trigger_buffer_size;

        // Check trigger condition (simple rising edge trigger)
        if (!g_device.trigger_occurred && samples[i] > g_device.trigger_threshold) {
            if (last_sample <= g_device.trigger_threshold) {
                printf("Trigger event detected! Value: %d > Threshold: %.1f\n", samples[i], g_device.trigger_threshold);
                g_device.trigger_occurred = true;
                g_device.trigger_armed = false;
                send_trigger_event(g_client_socket);
                break;
            }
        }
        last_sample = samples[i];
    }
}

// --- Send Trigger Event ---
void send_trigger_event(SOCKET clientSocket) {
    uint8_t event_payload[16]; // Increased buffer size
    uint32_t trigger_timestamp = g_device.timestamp_ms;
    uint16_t trigger_channel = 0; // Trigger source channel
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

// --- Data Streaming Thread (Alternative Solution) ---
DWORD WINAPI data_streaming_thread(LPVOID lpParam) {
    SOCKET clientSocket = (SOCKET)lpParam;

    printf("Data streaming thread started\n");

    while (g_data_thread_running && g_device.stream_status == STATUS_RUNNING) {
        if (g_device.mode == MODE_CONTINUOUS) {
            generate_data_packet(clientSocket);
        }

        Sleep(DATA_SEND_INTERVAL_MS);
    }

    printf("Data streaming thread exited\n");
    return 0;
}

// --- Main Program Logic Separation ---
int main_program() {
    WSADATA wsaData;
    int iResult;

    printf("=== High-Fidelity Lower Computer Simulator (test-sender) v2.0 ===\n");
    printf("Protocol Version: V6\n");
    printf("Port: %s\n", DEFAULT_PORT);
    printf("CSV File: %s\n\n", SAMPLE_DATA_FILE);

    // Initialize random number generator
    srand((unsigned int)time(NULL));

    // Initialize device state
    init_device_state();

    // Load CSV test data
    if (!load_csv_data(SAMPLE_DATA_FILE)) {
        printf("Warning: Unable to load CSV file '%s', using built-in test data\n", SAMPLE_DATA_FILE);
    }

    // Initialize Winsock
    iResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (iResult != 0) {
        printf("Error: WSAStartup failed: %d\n", iResult);
        cleanup_device_state();
        return 1;
    }

    struct addrinfo *result = NULL, hints;
    ZeroMemory(&hints, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_PASSIVE;

    // Resolve server address and port
    iResult = getaddrinfo(NULL, DEFAULT_PORT, &hints, &result);
    if (iResult != 0) {
        printf("Error: getaddrinfo failed: %d\n", iResult);
        WSACleanup();
        cleanup_device_state();
        return 1;
    }

    // Create listening socket
    SOCKET listenSocket = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (listenSocket == INVALID_SOCKET) {
        printf("Error: socket() failed: %d\n", (int)WSAGetLastError());
        freeaddrinfo(result);
        WSACleanup();
        cleanup_device_state();
        return 1;
    }

    // Set socket options
    int reuse = 1;
    setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));

    // Bind socket
    iResult = bind(listenSocket, result->ai_addr, (int)result->ai_addrlen);
    if (iResult == SOCKET_ERROR) {
        printf("Error: bind failed: %d\n", WSAGetLastError());
        freeaddrinfo(result);
        closesocket(listenSocket);
        WSACleanup();
        cleanup_device_state();
        return 1;
    }
    freeaddrinfo(result);

    // Start listening
    if (listen(listenSocket, SOMAXCONN) == SOCKET_ERROR) {
        printf("Error: listen failed: %d\n", (int)WSAGetLastError());
        closesocket(listenSocket);
        WSACleanup();
        cleanup_device_state();
        return 1;
    }

    printf("Simulator listening on port %s, waiting for connections...\n", DEFAULT_PORT);
    printf("Press Ctrl+C to exit the program\n\n");

    // 设置监听socket为非阻塞模式
    u_long mode = 1;
    ioctlsocket(listenSocket, FIONBIO, &mode);

    while (g_running) {
        // 非阻塞accept
        SOCKET clientSocket = accept(listenSocket, NULL, NULL);
        if (clientSocket == INVALID_SOCKET) {
            int error = WSAGetLastError();
            if (error == WSAEWOULDBLOCK) {
                // 没有连接请求，继续循环
                Sleep(100); // 短暂延迟避免占用太多CPU
                continue;
            } else if (g_running) {
                printf("Error: accept failed: %d\n", error);
            }
            continue;
        }

        printf("Client connected (Socket: %lld)\n", (long long)clientSocket);
        handle_client(clientSocket);
        printf("Client disconnected\n\n");
    }

    printf("Shutting down server...\n");
    closesocket(listenSocket);
    WSACleanup();
    cleanup_device_state();

    printf("Simulator exited\n");
    return 0;
}

// --- Program Entry Point Enhancement (Windows) ---
#ifdef _WIN32
int WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    // Allocate console window (if started as Windows program)
    if (AllocConsole()) {
        freopen_s(NULL, "CONOUT$", "w", stdout);
        freopen_s(NULL, "CONIN$", "r", stdin);
        freopen_s(NULL, "CONERR$", "w", stderr);
    }

    // Simple command line parsing (Windows version)
    if (lpCmdLine && strlen(lpCmdLine) > 0) {
        printf("Command line arguments: %s\n", lpCmdLine);
    }

    // Register Ctrl+C handler
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    // Call main program
    return main_program();
}
#endif

// --- Unix Signal Handling ---
#ifndef _WIN32
void unix_signal_handler(int sig) {
    switch (sig) {
        case SIGINT:
        case SIGTERM:
            printf("\nReceived exit signal, shutting down simulator...\n");
            g_running = false;
            break;
        default:
            break;
    }
}
#endif

// --- Ctrl+C Signal Handling ---
BOOL WINAPI ConsoleCtrlHandler(DWORD dwCtrlType) {
    switch (dwCtrlType) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
            printf("\nExiting simulator...\n");
            g_running = false;
            return TRUE;
        default:
            return FALSE;
    }
}

// --- Version Information and Build Information ---
void print_build_info() {
    printf("=== Build Information ===\n");
    printf("Version: v2.0\n");
    printf("Build Time: %s %s\n", __DATE__, __TIME__);
    printf("Compiler: ");
    #if defined(__GNUC__)
    printf("GCC %d.%d.%d\n", __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__);
    #elif defined(_MSC_VER)
    printf("MSVC %d\n", _MSC_VER);
    #else
    printf("Unknown\n");
    #endif

    printf("Platform: ");
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

    printf("Features: ");
    #ifdef DEBUG
    printf("Debug ");
    #endif
    #ifdef USE_MATH_DEFINES
    printf("Math ");
    #endif
    printf("\n");

    printf("Protocol Version: V6\n");
    printf("===============\n\n");
}

// --- Memory Usage Statistics (for debugging) ---
#ifdef DEBUG
void print_memory_usage() {
    printf("=== Memory Usage Statistics ===\n");
    printf("Device State: %zu bytes\n", sizeof(DeviceState_t));
    printf("RX Buffer: %d bytes\n", RX_BUFFER_SIZE);
    printf("TX Buffer: %d bytes\n", TX_BUFFER_SIZE);
    printf("Trigger Buffer: %d bytes\n", g_device.trigger_buffer_size * (int)sizeof(int16_t));
    if (g_device.csv_data) {
        printf("CSV Data: %d rows × 2 × %zu bytes = %zu bytes\n",
               g_device.csv_rows, sizeof(float),
               g_device.csv_rows * 2 * sizeof(float));
    }
    printf("==================\n\n");
}
#endif