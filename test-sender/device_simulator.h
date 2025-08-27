// File: device_simulator.h
// Description: Device simulator header - supports both simulation and MCU environments
// Version: v2.1
// Protocol: V6

#ifndef DEVICE_SIMULATOR_H
#define DEVICE_SIMULATOR_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>

// ===================== Configuration Macros =====================
// SIMULATION_MODE is defined by Makefile, don't redefine here
#ifndef SIMULATION_MODE
// If not defined by build system, default to simulation mode
#define SIMULATION_MODE
#endif

#ifdef SIMULATION_MODE
    // Simulation environment includes
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
    #include <winsock2.h>
    #include <stdio.h>
    #include <stdlib.h>
    #include <string.h>
    #include <math.h>
    #define PLATFORM_PRINTF printf
    #define PLATFORM_MALLOC malloc
    #define PLATFORM_FREE free
    #define PLATFORM_SLEEP(ms) Sleep(ms)
    #define PLATFORM_TICK() GetTickCount()
    typedef SOCKET connection_handle_t;
    #define INVALID_CONNECTION INVALID_SOCKET
#else
    // MCU environment includes (customize based on your MCU)
    #include "mcu_hal.h"
    #include "usb_cdc.h"
    #define PLATFORM_PRINTF debug_printf
    #define PLATFORM_MALLOC pvPortMalloc
    #define PLATFORM_FREE vPortFree
    #define PLATFORM_SLEEP(ms) vTaskDelay(pdMS_TO_TICKS(ms))
    #define PLATFORM_TICK() xTaskGetTickCount()
    typedef usb_handle_t connection_handle_t;
    #define INVALID_CONNECTION NULL
#endif

// ===================== Protocol V6 Commands =====================
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

// ===================== Configuration Constants =====================
#define DEVICE_UNIQUE_ID            0x11223344AABBCCDDULL
#define MAX_CHANNELS                4
#define DATA_SEND_INTERVAL_MS       10
#define MAX_CSV_ROWS                10000
#define CSV_BUFFER_SIZE             32768

#ifdef SIMULATION_MODE
    #define DEFAULT_PORT            "9001"
    #define SAMPLE_DATA_FILE        "sample_data.csv"
#else
    // MCU doesn't use these
    #define DEFAULT_PORT            NULL
    #define SAMPLE_DATA_FILE        NULL
#endif

// ===================== Data Structures =====================
typedef enum {
    MODE_CONTINUOUS,
    MODE_TRIGGER,
} DeviceMode;

typedef enum {
    STATUS_STOPPED,
    STATUS_RUNNING,
} StreamStatus;

typedef struct {
    uint8_t channel_id;
    uint32_t max_sample_rate_hz;
    uint16_t supported_formats_mask;
    char name[32];
    bool enabled;
    uint32_t current_sample_rate;
    uint8_t current_format;
} ChannelInfo_t;

typedef struct {
    // Core device state
    DeviceMode mode;
    StreamStatus stream_status;
    uint8_t seq_counter;
    uint32_t timestamp_ms;
    bool device_error;
    uint8_t error_code;

    // Channel configuration
    ChannelInfo_t channels[MAX_CHANNELS];
    uint8_t num_channels;

    // Data source (simulation only)
#ifdef SIMULATION_MODE
    char csv_buffer[CSV_BUFFER_SIZE];
    int csv_rows;
    int current_csv_row;
    float** csv_data;
#else
    // MCU uses real ADC/sensors
    void* adc_handle;
    void* sensor_handles[MAX_CHANNELS];
#endif

    // Trigger simulation
    bool trigger_simulation_active;
    uint32_t next_trigger_time;
    int trigger_data_packets_to_send;
    int trigger_data_packets_sent;
    
    // Trigger parameters
    bool trigger_armed;
    float trigger_threshold;
    int pre_trigger_samples;
    int post_trigger_samples;
    int16_t* trigger_buffer;
    int trigger_buffer_size;
    int trigger_buffer_pos;
    bool trigger_occurred;

    // Communication
    connection_handle_t connection;
    bool connected;
} DeviceState_t;

// ===================== Function Declarations =====================

// Device lifecycle
bool device_init(void);
void device_cleanup(void);
bool device_start_communication(void);
void device_stop_communication(void);

// Communication
bool device_send_response(uint8_t commandID, uint8_t seq, const uint8_t* payload, uint16_t payloadLen);
void device_send_log_message(uint8_t level, const char* message);
void device_communication_loop(void);

// Command processing
void device_process_command(uint8_t cmd, uint8_t seq, const uint8_t* payload, uint16_t payloadLen);

// Data generation and management
void device_generate_data_packet(void);
bool device_load_test_data(const char* filename);
void device_handle_trigger_simulation(void);
void device_schedule_next_trigger(void);

// Platform abstraction
bool platform_init(void);
void platform_cleanup(void);
connection_handle_t platform_create_connection(void);
bool platform_send_data(connection_handle_t conn, const uint8_t* data, uint32_t length);
int platform_receive_data(connection_handle_t conn, uint8_t* buffer, uint32_t bufferSize);
void platform_close_connection(connection_handle_t conn);

// Data source abstraction
bool data_source_init(void);
void data_source_cleanup(void);
int16_t data_source_get_sample(uint8_t channel, uint32_t sample_index);

// Utility functions
const char* device_get_command_name(uint8_t cmd);
bool device_validate_channel_config(uint8_t channel_id, uint32_t sample_rate, uint8_t format);

// Global device state
extern DeviceState_t g_device_state;

#endif // DEVICE_SIMULATOR_H