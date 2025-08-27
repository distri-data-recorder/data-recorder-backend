// File: platform_abstraction.c
// Description: Platform abstraction layer - handles simulation vs MCU environments
// Version: v2.1

#include "device_simulator.h"

#ifdef SIMULATION_MODE
    #include <ws2tcpip.h>
    #ifdef _MSC_VER
        #pragma comment(lib, "ws2_32.lib")
    #endif
#endif

// ===================== Platform Initialization =====================

bool platform_init(void) {
#ifdef SIMULATION_MODE
    // Initialize Winsock
    WSADATA wsaData;
    int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0) {
        PLATFORM_PRINTF("WSAStartup failed: %d\n", result);
        return false;
    }
    PLATFORM_PRINTF("Simulation platform initialized\n");
    return true;
#else
    // Initialize MCU hardware
    if (mcu_hal_init() != HAL_OK) {
        return false;
    }
    if (usb_cdc_init() != USB_OK) {
        return false;
    }
    PLATFORM_PRINTF("MCU platform initialized\n");
    return true;
#endif
}

void platform_cleanup(void) {
#ifdef SIMULATION_MODE
    WSACleanup();
    PLATFORM_PRINTF("Simulation platform cleaned up\n");
#else
    usb_cdc_deinit();
    mcu_hal_deinit();
    PLATFORM_PRINTF("MCU platform cleaned up\n");
#endif
}

// ===================== Connection Management =====================

connection_handle_t platform_create_connection(void) {
#ifdef SIMULATION_MODE
    struct addrinfo *result = NULL, hints;
    ZeroMemory(&hints, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_PASSIVE;

    // Resolve server address and port
    int iResult = getaddrinfo(NULL, DEFAULT_PORT, &hints, &result);
    if (iResult != 0) {
        PLATFORM_PRINTF("getaddrinfo failed: %d\n", iResult);
        return INVALID_CONNECTION;
    }

    // Create listening socket
    SOCKET listenSocket = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (listenSocket == INVALID_SOCKET) {
        PLATFORM_PRINTF("socket() failed: %d\n", (int)WSAGetLastError());
        freeaddrinfo(result);
        return INVALID_CONNECTION;
    }

    // Set socket options
    int reuse = 1;
    setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));

    // Bind socket
    iResult = bind(listenSocket, result->ai_addr, (int)result->ai_addrlen);
    if (iResult == SOCKET_ERROR) {
        PLATFORM_PRINTF("bind failed: %d\n", WSAGetLastError());
        freeaddrinfo(result);
        closesocket(listenSocket);
        return INVALID_CONNECTION;
    }
    freeaddrinfo(result);

    // Start listening
    if (listen(listenSocket, SOMAXCONN) == SOCKET_ERROR) {
        PLATFORM_PRINTF("listen failed: %d\n", (int)WSAGetLastError());
        closesocket(listenSocket);
        return INVALID_CONNECTION;
    }

    PLATFORM_PRINTF("Listening on port %s...\n", DEFAULT_PORT);

    // Accept connection
    SOCKET clientSocket = accept(listenSocket, NULL, NULL);
    closesocket(listenSocket); // Close listening socket
    
    if (clientSocket == INVALID_SOCKET) {
        PLATFORM_PRINTF("accept failed: %d\n", WSAGetLastError());
        return INVALID_CONNECTION;
    }

    // Set non-blocking mode
    u_long mode = 1;
    ioctlsocket(clientSocket, FIONBIO, &mode);

    PLATFORM_PRINTF("Client connected\n");
    return clientSocket;
#else
    // Initialize USB CDC connection
    usb_handle_t handle = usb_cdc_create_handle();
    if (handle == NULL) {
        return INVALID_CONNECTION;
    }
    
    // Wait for USB enumeration
    while (!usb_cdc_is_connected(handle)) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    
    PLATFORM_PRINTF("USB CDC connected\n");
    return handle;
#endif
}

bool platform_send_data(connection_handle_t conn, const uint8_t* data, uint32_t length) {
#ifdef SIMULATION_MODE
    int bytesSent = send(conn, (const char*)data, length, 0);
    return (bytesSent == (int)length);
#else
    return (usb_cdc_send(conn, data, length) == USB_OK);
#endif
}

int platform_receive_data(connection_handle_t conn, uint8_t* buffer, uint32_t bufferSize) {
#ifdef SIMULATION_MODE
    int bytesReceived = recv(conn, (char*)buffer, bufferSize, 0);
    if (bytesReceived == SOCKET_ERROR) {
        int error = WSAGetLastError();
        if (error == WSAEWOULDBLOCK) {
            return 0; // No data available
        }
        return -1; // Error
    } else if (bytesReceived == 0) {
        return -1; // Connection closed
    }
    return bytesReceived;
#else
    uint32_t received = 0;
    usb_status_t status = usb_cdc_receive(conn, buffer, bufferSize, &received, 0); // Non-blocking
    if (status == USB_OK) {
        return (int)received;
    } else if (status == USB_BUSY) {
        return 0; // No data
    } else {
        return -1; // Error
    }
#endif
}

void platform_close_connection(connection_handle_t conn) {
#ifdef SIMULATION_MODE
    closesocket(conn);
    PLATFORM_PRINTF("Connection closed\n");
#else
    usb_cdc_close_handle(conn);
    PLATFORM_PRINTF("USB connection closed\n");
#endif
}

// ===================== Data Source Abstraction =====================

bool data_source_init(void) {
#ifdef SIMULATION_MODE
    // Initialize CSV data or built-in generators
    g_device_state.csv_data = NULL;
    g_device_state.csv_rows = 0;
    g_device_state.current_csv_row = 0;
    
    // Initialize random seed
    srand((unsigned int)time(NULL));
    
    PLATFORM_PRINTF("Simulation data source initialized\n");
    return true;
#else
    // Initialize ADC and sensors
    if (adc_init() != HAL_OK) {
        return false;
    }
    
    for (int i = 0; i < g_device_state.num_channels; i++) {
        g_device_state.sensor_handles[i] = sensor_init(i);
        if (g_device_state.sensor_handles[i] == NULL) {
            PLATFORM_PRINTF("Failed to initialize sensor %d\n", i);
            return false;
        }
    }
    
    g_device_state.adc_handle = adc_get_handle();
    PLATFORM_PRINTF("MCU data source initialized\n");
    return true;
#endif
}

void data_source_cleanup(void) {
#ifdef SIMULATION_MODE
    if (g_device_state.csv_data) {
        for (int i = 0; i < g_device_state.csv_rows; i++) {
            if (g_device_state.csv_data[i]) {
                PLATFORM_FREE(g_device_state.csv_data[i]);
            }
        }
        PLATFORM_FREE(g_device_state.csv_data);
        g_device_state.csv_data = NULL;
    }
    PLATFORM_PRINTF("Simulation data source cleaned up\n");
#else
    for (int i = 0; i < g_device_state.num_channels; i++) {
        if (g_device_state.sensor_handles[i]) {
            sensor_deinit(g_device_state.sensor_handles[i]);
            g_device_state.sensor_handles[i] = NULL;
        }
    }
    adc_deinit();
    PLATFORM_PRINTF("MCU data source cleaned up\n");
#endif
}

int16_t data_source_get_sample(uint8_t channel, uint32_t sample_index) {
#ifdef SIMULATION_MODE
    // Use CSV data if available
    if (g_device_state.csv_data && g_device_state.csv_rows > 0 && channel < 2) {
        int csv_index = (g_device_state.current_csv_row + sample_index) % g_device_state.csv_rows;
        return (int16_t)(g_device_state.csv_data[csv_index][channel] * 100);
    }
    
    // Generate simulated data
    float t = (g_device_state.timestamp_ms + sample_index * DATA_SEND_INTERVAL_MS / 100.0f) / 1000.0f;
    float freq = (channel == 0) ? 50.0f : 60.0f;
    float amplitude = (channel == 0) ? 1000.0f : 800.0f;
    float noise = ((rand() % 100) - 50) * 0.1f;
    
    return (int16_t)(amplitude * sinf(2.0f * 3.14159f * freq * t) + noise);
#else
    // Read real sensor data
    if (channel >= g_device_state.num_channels) {
        return 0;
    }
    
    uint16_t raw_value = 0;
    if (adc_read_channel(g_device_state.adc_handle, channel, &raw_value) == HAL_OK) {
        // Convert to signed 16-bit with appropriate scaling
        return (int16_t)(raw_value - 32768);
    }
    
    return 0;
#endif
}

// ===================== CSV Data Loading (Simulation Only) =====================

#ifdef SIMULATION_MODE
bool device_load_test_data(const char* filename) {
    if (!filename) return false;
    
    FILE* file = fopen(filename, "r");
    if (!file) {
        PLATFORM_PRINTF("Warning: Cannot load CSV file '%s', using built-in data\n", filename);
        return false;
    }

    // Read file content
    size_t bytes_read = fread(g_device_state.csv_buffer, 1, CSV_BUFFER_SIZE - 1, file);
    g_device_state.csv_buffer[bytes_read] = '\0';
    fclose(file);

    // Count rows
    int row_count = 0;
    char* temp_buffer = _strdup(g_device_state.csv_buffer);
    char* temp_line = strtok(temp_buffer, "\r\n");
    while (temp_line && row_count < MAX_CSV_ROWS) {
        if (temp_line[0] != '#' && strlen(temp_line) > 0) {
            row_count++;
        }
        temp_line = strtok(NULL, "\r\n");
    }
    free(temp_buffer);

    if (row_count == 0) {
        PLATFORM_PRINTF("No valid data rows found in CSV\n");
        return false;
    }

    // Allocate memory
    g_device_state.csv_data = (float**)PLATFORM_MALLOC(row_count * sizeof(float*));
    for (int i = 0; i < row_count; i++) {
        g_device_state.csv_data[i] = (float*)PLATFORM_MALLOC(2 * sizeof(float));
    }

    // Parse data
    file = fopen(filename, "r");
    bytes_read = fread(g_device_state.csv_buffer, 1, CSV_BUFFER_SIZE - 1, file);
    g_device_state.csv_buffer[bytes_read] = '\0';
    fclose(file);

    char* line = strtok(g_device_state.csv_buffer, "\r\n");
    int current_row = 0;

    while (line && current_row < row_count) {
        if (line[0] != '#' && strlen(line) > 0) {
            char* token1 = strtok(line, ",");
            char* token2 = strtok(NULL, ",");

            if (token1 && token2) {
                g_device_state.csv_data[current_row][0] = (float)atof(token1);
                g_device_state.csv_data[current_row][1] = (float)atof(token2);
                current_row++;
            }
        }
        line = strtok(NULL, "\r\n");
    }

    g_device_state.csv_rows = current_row;
    PLATFORM_PRINTF("Loaded CSV data: %d rows\n", g_device_state.csv_rows);
    return true;
}
#else
bool device_load_test_data(const char* filename) {
    // MCU doesn't load CSV files
    return true;
}
#endif