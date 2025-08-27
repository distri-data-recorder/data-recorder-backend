// File: device_simulator.c
// Description: Device simulator core implementation
// Version: v2.1
// Protocol: V6

#include "device_simulator.h"
#include "protocol/protocol.h"
#include "protocol/io_buffer.h"
#include <time.h>

// Global device state
DeviceState_t g_device_state;

// Static variables for communication
static RxBuffer_t g_rx_buffer;
static TxBuffer_t g_tx_buffer;
static volatile bool g_running = true;

// ===================== Device Lifecycle =====================

bool device_init(void) {
    PLATFORM_PRINTF("Initializing device (Protocol V6)...\n");
    
    // Initialize device state
    memset(&g_device_state, 0, sizeof(g_device_state));
    
    g_device_state.mode = MODE_CONTINUOUS;
    g_device_state.stream_status = STATUS_STOPPED;
    g_device_state.seq_counter = 0;
    g_device_state.timestamp_ms = 0;
    g_device_state.device_error = false;
    g_device_state.error_code = 0;
    g_device_state.connected = false;
    g_device_state.connection = INVALID_CONNECTION;

    // Initialize channels
    g_device_state.num_channels = 2;

    // Channel 0 - Voltage
    g_device_state.channels[0].channel_id = 0;
    g_device_state.channels[0].max_sample_rate_hz = 100000;
    g_device_state.channels[0].supported_formats_mask = 0x01 | 0x02; // int16, int32
    strcpy(g_device_state.channels[0].name, "Voltage");
    g_device_state.channels[0].enabled = false;
    g_device_state.channels[0].current_sample_rate = 0;
    g_device_state.channels[0].current_format = 0x01;

    // Channel 1 - Current
    g_device_state.channels[1].channel_id = 1;
    g_device_state.channels[1].max_sample_rate_hz = 100000;
    g_device_state.channels[1].supported_formats_mask = 0x01 | 0x02;
    strcpy(g_device_state.channels[1].name, "Current");
    g_device_state.channels[1].enabled = false;
    g_device_state.channels[1].current_sample_rate = 0;
    g_device_state.channels[1].current_format = 0x01;

    // Initialize trigger simulation
    g_device_state.trigger_simulation_active = false;
    g_device_state.trigger_armed = false;
    g_device_state.trigger_threshold = 1000.0f;
    g_device_state.pre_trigger_samples = 1000;
    g_device_state.post_trigger_samples = 1000;
    g_device_state.trigger_buffer_size = 4096;
    g_device_state.trigger_buffer = (int16_t*)PLATFORM_MALLOC(g_device_state.trigger_buffer_size * sizeof(int16_t));
    g_device_state.trigger_buffer_pos = 0;
    g_device_state.trigger_occurred = false;

    if (!g_device_state.trigger_buffer) {
        PLATFORM_PRINTF("Failed to allocate trigger buffer\n");
        return false;
    }

    // Initialize communication buffers
    initRxBuffer(&g_rx_buffer);
    initTxBuffer(&g_tx_buffer);

    // Initialize platform and data source
    if (!platform_init()) {
        PLATFORM_PRINTF("Platform initialization failed\n");
        return false;
    }

    if (!data_source_init()) {
        PLATFORM_PRINTF("Data source initialization failed\n");
        return false;
    }

#ifdef SIMULATION_MODE
    // Load test data if available
    device_load_test_data(SAMPLE_DATA_FILE);
#endif

    PLATFORM_PRINTF("Device initialized successfully\n");
    return true;
}

void device_cleanup(void) {
    PLATFORM_PRINTF("Cleaning up device...\n");
    
    device_stop_communication();
    
    if (g_device_state.trigger_buffer) {
        PLATFORM_FREE(g_device_state.trigger_buffer);
        g_device_state.trigger_buffer = NULL;
    }

    data_source_cleanup();
    platform_cleanup();
    
    PLATFORM_PRINTF("Device cleanup complete\n");
}

// ===================== Communication Management =====================

bool device_start_communication(void) {
    g_device_state.connection = platform_create_connection();
    if (g_device_state.connection == INVALID_CONNECTION) {
        PLATFORM_PRINTF("Failed to create connection\n");
        return false;
    }
    
    g_device_state.connected = true;
    g_device_state.timestamp_ms = PLATFORM_TICK();
    
    PLATFORM_PRINTF("Communication started\n");
    return true;
}

void device_stop_communication(void) {
    if (g_device_state.connected) {
        platform_close_connection(g_device_state.connection);
        g_device_state.connected = false;
        g_device_state.connection = INVALID_CONNECTION;
        PLATFORM_PRINTF("Communication stopped\n");
    }
}

bool device_send_response(uint8_t commandID, uint8_t seq, const uint8_t* payload, uint16_t payloadLen) {
    if (!g_device_state.connected) {
        return false;
    }

    uint8_t frameBuf[MAX_FRAME_SIZE];
    uint16_t frameLen = MAX_FRAME_SIZE;

    if (buildFrame(commandID, seq, payload, payloadLen, frameBuf, &frameLen) == 0) {
        bool success = platform_send_data(g_device_state.connection, frameBuf, frameLen);
        if (success) {
            PLATFORM_PRINTF("Sent response: CMD=0x%02X, Len=%u\n", commandID, frameLen);
        }
        return success;
    }
    return false;
}

void device_send_log_message(uint8_t level, const char* message) {
    if (!g_device_state.connected || !message) {
        return;
    }

    uint8_t payload[256];
    uint8_t msg_len = strlen(message);
    if (msg_len > 253) msg_len = 253;

    payload[0] = level;
    payload[1] = msg_len;
    memcpy(payload + 2, message, msg_len);

    device_send_response(CMD_LOG_MESSAGE, g_device_state.seq_counter++, payload, msg_len + 2);
}

// ===================== Command Processing =====================

void device_process_command(uint8_t cmd, uint8_t seq, const uint8_t* payload, uint16_t payloadLen) {
    switch (cmd) {
        case CMD_PING: {
            uint64_t id = DEVICE_UNIQUE_ID;
            device_send_response(CMD_PONG, seq, (uint8_t*)&id, sizeof(id));
            PLATFORM_PRINTF("Responded to PING\n");
            break;
        }

        case CMD_GET_STATUS: {
            uint8_t status_payload[8] = {0};
            status_payload[0] = (g_device_state.mode == MODE_CONTINUOUS) ? 0x00 : 0x01;
            status_payload[1] = (g_device_state.stream_status == STATUS_RUNNING) ? 0x01 : 0x00;
            status_payload[2] = g_device_state.device_error ? 0x01 : 0x00;
            status_payload[3] = g_device_state.error_code;
            device_send_response(CMD_STATUS_RESPONSE, seq, status_payload, sizeof(status_payload));
            break;
        }

        case CMD_GET_DEVICE_INFO: {
            uint8_t info_payload[512];
            uint16_t offset = 0;

            // Protocol version
            info_payload[offset++] = 6;
            
            // Firmware version
            uint16_t fw_version = 0x0201; // v2.1
            memcpy(info_payload + offset, &fw_version, sizeof(fw_version));
            offset += sizeof(fw_version);
            
            // Number of channels
            info_payload[offset++] = g_device_state.num_channels;

            // Channel capabilities
            for (int i = 0; i < g_device_state.num_channels; i++) {
                ChannelInfo_t* ch = &g_device_state.channels[i];
                
                info_payload[offset++] = ch->channel_id;
                memcpy(info_payload + offset, &ch->max_sample_rate_hz, sizeof(ch->max_sample_rate_hz));
                offset += sizeof(ch->max_sample_rate_hz);
                memcpy(info_payload + offset, &ch->supported_formats_mask, sizeof(ch->supported_formats_mask));
                offset += sizeof(ch->supported_formats_mask);
                
                uint8_t name_len = strlen(ch->name);
                info_payload[offset++] = name_len;
                memcpy(info_payload + offset, ch->name, name_len);
                offset += name_len;
            }

            device_send_response(CMD_DEVICE_INFO_RESPONSE, seq, info_payload, offset);
            PLATFORM_PRINTF("Responded to device info query\n");
            break;
        }

        case CMD_SET_MODE_CONTINUOUS: {
            g_device_state.mode = MODE_CONTINUOUS;
            g_device_state.trigger_simulation_active = false;
            device_send_response(CMD_ACK, seq, NULL, 0);
            device_send_log_message(1, "Switched to continuous mode");
            PLATFORM_PRINTF("Set to continuous mode\n");
            break;
        }

        case CMD_SET_MODE_TRIGGER: {
            g_device_state.mode = MODE_TRIGGER;
            g_device_state.trigger_armed = true;
            g_device_state.trigger_occurred = false;
            g_device_state.trigger_simulation_active = true;
            device_send_response(CMD_ACK, seq, NULL, 0);
            device_send_log_message(1, "Switched to trigger mode");
            PLATFORM_PRINTF("Set to trigger mode\n");
            
            // Schedule first trigger
            device_schedule_next_trigger();
            break;
        }

        case CMD_START_STREAM: {
            g_device_state.stream_status = STATUS_RUNNING;
            g_device_state.timestamp_ms = PLATFORM_TICK();
            device_send_response(CMD_ACK, seq, NULL, 0);
            device_send_log_message(1, "Stream started");
            PLATFORM_PRINTF("Data stream started\n");
            break;
        }

        case CMD_STOP_STREAM: {
            g_device_state.stream_status = STATUS_STOPPED;
            g_device_state.trigger_simulation_active = false;
            device_send_response(CMD_ACK, seq, NULL, 0);
            device_send_log_message(1, "Stream stopped");
            PLATFORM_PRINTF("Data stream stopped\n");
            break;
        }

        case CMD_CONFIGURE_STREAM: {
            if (payloadLen < 1) {
                uint8_t err_payload[] = {0x01, 0x01}; // Parameter error
                device_send_response(CMD_NACK, seq, err_payload, sizeof(err_payload));
                break;
            }

            uint8_t num_configs = payload[0];
            uint16_t offset = 1;
            bool config_error = false;

            for (uint8_t i = 0; i < num_configs && !config_error; i++) {
                if (offset + 6 > payloadLen) {
                    config_error = true;
                    break;
                }

                uint8_t channel_id = payload[offset];
                uint32_t sample_rate = *(uint32_t*)(payload + offset + 1);
                uint8_t sample_format = payload[offset + 5];
                offset += 6;

                if (!device_validate_channel_config(channel_id, sample_rate, sample_format)) {
                    config_error = true;
                    break;
                }

                // Apply configuration
                if (channel_id < g_device_state.num_channels) {
                    g_device_state.channels[channel_id].enabled = (sample_rate > 0);
                    g_device_state.channels[channel_id].current_sample_rate = sample_rate;
                    g_device_state.channels[channel_id].current_format = sample_format;
                }
            }

            if (config_error) {
                uint8_t err_payload[] = {0x01, 0x02};
                device_send_response(CMD_NACK, seq, err_payload, sizeof(err_payload));
            } else {
                device_send_response(CMD_ACK, seq, NULL, 0);
                device_send_log_message(1, "Stream configuration updated");
            }
            break;
        }

        case CMD_REQUEST_BUFFERED_DATA: {
            if (g_device_state.mode != MODE_TRIGGER) {
                uint8_t err_payload[] = {0x02, 0x01}; // Status error
                device_send_response(CMD_NACK, seq, err_payload, sizeof(err_payload));
                break;
            }

            if (!g_device_state.trigger_occurred) {
                uint8_t err_payload[] = {0x02, 0x02}; // Status error - not triggered
                device_send_response(CMD_NACK, seq, err_payload, sizeof(err_payload));
                break;
            }

            device_send_response(CMD_ACK, seq, NULL, 0);
            device_send_log_message(1, "Sending buffered trigger data");

            // This will be handled by trigger simulation
            break;
        }

        default: {
            PLATFORM_PRINTF("Unknown command: 0x%02X\n", cmd);
            uint8_t err_payload[] = {0x05, 0x00}; // Command not supported
            device_send_response(CMD_NACK, seq, err_payload, sizeof(err_payload));
            break;
        }
    }
}

// ===================== Data Generation =====================

void device_generate_data_packet(void) {
    uint8_t payload[2048];
    uint16_t payload_offset = 0;
    uint16_t enabled_channels = 0;
    uint16_t sample_count = 0;

    // Calculate enabled channels and sample count
    for (int i = 0; i < g_device_state.num_channels; i++) {
        if (g_device_state.channels[i].enabled) {
            enabled_channels |= (1 << i);
            if (sample_count == 0) {
                sample_count = (g_device_state.channels[i].current_sample_rate * DATA_SEND_INTERVAL_MS) / 1000;
                if (sample_count == 0) sample_count = 1;
                if (sample_count > 100) sample_count = 100;
            }
        }
    }

    // Debug output
    PLATFORM_PRINTF("Channels enabled: 0x%04X, Sample count: %u\n", enabled_channels, sample_count);

    if (enabled_channels == 0) {
        PLATFORM_PRINTF("No channels enabled - configuring default channels\n");
        // Auto-enable default channels if none are configured
        g_device_state.channels[0].enabled = true;
        g_device_state.channels[0].current_sample_rate = 10000;
        g_device_state.channels[0].current_format = 0x01;
        g_device_state.channels[1].enabled = true;
        g_device_state.channels[1].current_sample_rate = 10000;
        g_device_state.channels[1].current_format = 0x01;
        
        // Recalculate
        enabled_channels = 0x0003; // Channel 0 and 1
        sample_count = (10000 * DATA_SEND_INTERVAL_MS) / 1000;
        if (sample_count == 0) sample_count = 1;
        if (sample_count > 100) sample_count = 100;
        
        PLATFORM_PRINTF("Auto-configured channels: 0x%04X, samples: %u\n", enabled_channels, sample_count);
    }

    if (sample_count == 0) {
        PLATFORM_PRINTF("Sample count is 0, skipping packet\n");
        return;
    }

    // Fill data packet header
    memcpy(payload + payload_offset, &g_device_state.timestamp_ms, sizeof(g_device_state.timestamp_ms));
    payload_offset += sizeof(g_device_state.timestamp_ms);
    
    memcpy(payload + payload_offset, &enabled_channels, sizeof(enabled_channels));
    payload_offset += sizeof(enabled_channels);
    
    memcpy(payload + payload_offset, &sample_count, sizeof(sample_count));
    payload_offset += sizeof(sample_count);

    // Generate and fill data (non-interleaved format)
    for (int i = 0; i < g_device_state.num_channels; i++) {
        if (!(enabled_channels & (1 << i))) {
            continue;
        }

        // Generate data for this channel
        for (uint16_t s = 0; s < sample_count; s++) {
            int16_t sample_value = data_source_get_sample(i, g_device_state.timestamp_ms / DATA_SEND_INTERVAL_MS * sample_count + s);
            memcpy(payload + payload_offset, &sample_value, sizeof(sample_value));
            payload_offset += sizeof(sample_value);
        }
    }

    // Send data packet
    device_send_response(CMD_DATA_PACKET, g_device_state.seq_counter++, payload, payload_offset);
    g_device_state.timestamp_ms += DATA_SEND_INTERVAL_MS;
}

// ===================== Trigger Simulation =====================

void device_schedule_next_trigger(void) {
    if (!g_device_state.trigger_simulation_active) return;
    
    // Random 10-15 seconds
    int random_seconds = 10 + (rand() % 6);
    g_device_state.next_trigger_time = PLATFORM_TICK() + (random_seconds * 1000);
    
    // Random data packets: 50ms-100ms worth (5-10 packets)
    g_device_state.trigger_data_packets_to_send = 5 + (rand() % 6);
    g_device_state.trigger_data_packets_sent = 0;
    
    PLATFORM_PRINTF("Next trigger in %d seconds, will send %d packets\n", 
           random_seconds, g_device_state.trigger_data_packets_to_send);
}

void device_handle_trigger_simulation(void) {
    if (!g_device_state.trigger_simulation_active) return;
    
    uint32_t current_time = PLATFORM_TICK();
    
    // Check if trigger time reached
    if (current_time >= g_device_state.next_trigger_time && 
        g_device_state.trigger_data_packets_sent == 0) {
        
        // Send trigger event
        uint8_t event_payload[16];
        uint32_t trigger_timestamp = current_time;
        uint16_t trigger_channel = 0;
        uint32_t pre_samples = g_device_state.pre_trigger_samples;
        uint32_t post_samples = g_device_state.post_trigger_samples;

        uint16_t offset = 0;
        memcpy(event_payload + offset, &trigger_timestamp, sizeof(trigger_timestamp));
        offset += sizeof(trigger_timestamp);
        memcpy(event_payload + offset, &trigger_channel, sizeof(trigger_channel));
        offset += sizeof(trigger_channel);
        memcpy(event_payload + offset, &pre_samples, sizeof(pre_samples));
        offset += sizeof(pre_samples);
        memcpy(event_payload + offset, &post_samples, sizeof(post_samples));
        offset += sizeof(post_samples);

        device_send_response(CMD_EVENT_TRIGGERED, g_device_state.seq_counter++, event_payload, offset);
        device_send_log_message(2, "Trigger event detected");
        PLATFORM_PRINTF("Trigger event sent!\n");
        
        g_device_state.trigger_occurred = true;
    }
    
    // Send trigger data packets
    if (g_device_state.trigger_data_packets_sent < g_device_state.trigger_data_packets_to_send) {
        static uint32_t last_packet_time = 0;
        
        if (current_time - last_packet_time >= DATA_SEND_INTERVAL_MS) {
            device_generate_data_packet();
            g_device_state.trigger_data_packets_sent++;
            last_packet_time = current_time;
            
            PLATFORM_PRINTF("Sent trigger data packet %d/%d\n", 
                   g_device_state.trigger_data_packets_sent, 
                   g_device_state.trigger_data_packets_to_send);
        }
        
        // Transfer complete
        if (g_device_state.trigger_data_packets_sent >= g_device_state.trigger_data_packets_to_send) {
            device_send_response(CMD_BUFFER_TRANSFER_COMPLETE, g_device_state.seq_counter++, NULL, 0);
            PLATFORM_PRINTF("Trigger data transfer complete\n");
            
            // Schedule next trigger
            device_schedule_next_trigger();
        }
    }
}

// ===================== Utility Functions =====================

const char* device_get_command_name(uint8_t cmd) {
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

bool device_validate_channel_config(uint8_t channel_id, uint32_t sample_rate, uint8_t format) {
    if (channel_id >= g_device_state.num_channels) {
        return false;
    }

    ChannelInfo_t* ch = &g_device_state.channels[channel_id];

    if (sample_rate > ch->max_sample_rate_hz) {
        return false;
    }

    if (format != 0x00 && !(ch->supported_formats_mask & format)) {
        return false;
    }

    return true;
}