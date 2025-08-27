// File: main.c
// Description: Main program entry point for device simulator
// Version: v2.1
// Protocol: V6

#include "device_simulator.h"
#include "protocol/protocol.h"
#include "protocol/io_buffer.h"

// Global variables for communication loop
static RxBuffer_t g_rx_buffer;
static volatile bool g_running = true;

// Frame processing callback
static void on_frame_parsed(const uint8_t* frame, uint16_t frameLen);

#ifdef SIMULATION_MODE
// Signal handler for graceful shutdown
static BOOL WINAPI console_ctrl_handler(DWORD dwCtrlType) {
    switch (dwCtrlType) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
            PLATFORM_PRINTF("\nShutting down...\n");
            g_running = false;
            return TRUE;
        default:
            return FALSE;
    }
}
#endif

// ===================== Communication Loop =====================

void device_communication_loop(void) {
    uint8_t recv_buffer[4096];
    
    initRxBuffer(&g_rx_buffer);
    
    PLATFORM_PRINTF("Communication loop started (Protocol V6)\n");
    PLATFORM_PRINTF("Sending initial PING response ready...\n");

    while (g_running) {
        // Handle incoming data
        int bytes_received = platform_receive_data(g_device_state.connection, 
                                                  recv_buffer, sizeof(recv_buffer));
        if (bytes_received > 0) {
            // Feed data to receive buffer
            uint16_t fed = feedRxBuffer(&g_rx_buffer, recv_buffer, (uint16_t)bytes_received);
            if (fed < bytes_received) {
                PLATFORM_PRINTF("Warning: RX buffer overflow, %d bytes lost\n", bytes_received - fed);
            }

            // Try to parse frames
            tryParseFramesFromRx(&g_rx_buffer, on_frame_parsed);
        } else if (bytes_received < 0) {
            PLATFORM_PRINTF("Connection error or closed\n");
            break;
        }

        // Handle data generation based on current mode
        if (g_device_state.stream_status == STATUS_RUNNING) {
            static uint32_t last_data_time = 0;
            uint32_t current_time = PLATFORM_TICK();

            if (current_time - last_data_time >= DATA_SEND_INTERVAL_MS) {
                if (g_device_state.mode == MODE_CONTINUOUS) {
                    // Continuous mode: send data packets regularly
                    device_generate_data_packet();
                    PLATFORM_PRINTF("Generated continuous data packet\n");
                } else if (g_device_state.mode == MODE_TRIGGER) {
                    // Trigger mode: handle trigger simulation
                    device_handle_trigger_simulation();
                }
                last_data_time = current_time;
            }
        }

        PLATFORM_SLEEP(1); // Prevent high CPU usage
    }

    PLATFORM_PRINTF("Communication loop ended\n");
}

// ===================== Frame Processing =====================

static void on_frame_parsed(const uint8_t* frame, uint16_t frameLen) {
    uint8_t cmd = 0;
    uint8_t seq = 0;
    uint8_t payload[MAX_FRAME_SIZE];
    uint16_t payloadLen = 0;

    int ret = parseFrame(frame, frameLen, &cmd, &seq, payload, &payloadLen);
    if (ret == 0) {
        PLATFORM_PRINTF("Received: %s (0x%02X) seq=%u len=%u\n", 
                        device_get_command_name(cmd), cmd, seq, payloadLen);
        
        // Process the command
        device_process_command(cmd, seq, payload, payloadLen);
    } else {
        PLATFORM_PRINTF("Frame parsing failed: ret=%d, len=%u\n", ret, frameLen);
    }
}

// ===================== Main Function =====================

int main(int argc, char* argv[]) {
    PLATFORM_PRINTF("=== Device Simulator v2.1 ===\n");
    PLATFORM_PRINTF("Protocol: V6\n");
    
#ifdef SIMULATION_MODE
    PLATFORM_PRINTF("Mode: SIMULATION\n");
    PLATFORM_PRINTF("Port: %s\n", DEFAULT_PORT);
    
    // Parse command line arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            PLATFORM_PRINTF("\nUsage: %s [options]\n", argv[0]);
            PLATFORM_PRINTF("Options:\n");
            PLATFORM_PRINTF("  --help, -h        Show this help\n");
            PLATFORM_PRINTF("  --version         Show version info\n");
            PLATFORM_PRINTF("  --csv <file>      Use custom CSV data file\n");
            PLATFORM_PRINTF("\nSimulation Mode Features:\n");
            PLATFORM_PRINTF("  - TCP server on port %s\n", DEFAULT_PORT);
            PLATFORM_PRINTF("  - CSV data loading support\n");
            PLATFORM_PRINTF("  - Trigger simulation\n");
            PLATFORM_PRINTF("  - Built-in signal generation\n");
            return 0;
        } else if (strcmp(argv[i], "--version") == 0) {
            PLATFORM_PRINTF("Version: v2.1\n");
            PLATFORM_PRINTF("Build: %s %s\n", __DATE__, __TIME__);
            #ifdef DEBUG
            PLATFORM_PRINTF("Type: Debug\n");
            #else
            PLATFORM_PRINTF("Type: Release\n");
            #endif
            return 0;
        } else if (strcmp(argv[i], "--csv") == 0 && i + 1 < argc) {
            // Custom CSV file handling would go here
            PLATFORM_PRINTF("Custom CSV file: %s\n", argv[++i]);
        }
    }
    
    // Install signal handler
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
    
#else
    PLATFORM_PRINTF("Mode: MCU\n");
    PLATFORM_PRINTF("Interface: USB CDC\n");
#endif

    PLATFORM_PRINTF("================================\n\n");

    // Initialize device
    if (!device_init()) {
        PLATFORM_PRINTF("Device initialization failed!\n");
        return 1;
    }

    // Start communication
    if (!device_start_communication()) {
        PLATFORM_PRINTF("Failed to start communication!\n");
        device_cleanup();
        return 1;
    }

    PLATFORM_PRINTF("Device ready. Waiting for commands...\n");
    
#ifdef SIMULATION_MODE
    PLATFORM_PRINTF("Press Ctrl+C to exit\n\n");
#endif

    // Main communication loop
    device_communication_loop();

    // Cleanup
    device_cleanup();
    
    PLATFORM_PRINTF("Program exited\n");
    return 0;
}

// ===================== MCU-specific Functions =====================

#ifndef SIMULATION_MODE
// MCU-specific initialization and task creation
void vApplicationIdleHook(void) {
    // Low power mode or watchdog refresh
}

void vApplicationTickHook(void) {
    // System tick processing if needed
}

void vApplicationMallocFailedHook(void) {
    // Handle malloc failures
    PLATFORM_PRINTF("Memory allocation failed!\n");
    while(1);
}

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName) {
    // Handle stack overflow
    PLATFORM_PRINTF("Stack overflow in task: %s\n", pcTaskName);
    while(1);
}
#endif