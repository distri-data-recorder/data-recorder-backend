// File: config.h
// Description: Project configuration file
// Version: v2.1

#ifndef CONFIG_H
#define CONFIG_H

// ===================== Version Information =====================
#define PROJECT_VERSION_MAJOR       2
#define PROJECT_VERSION_MINOR       1
#define PROJECT_VERSION_PATCH       0
#define PROJECT_VERSION_STRING      "2.1.0"

// ===================== Protocol Configuration =====================
#define PROTOCOL_VERSION            6
#define DEVICE_FIRMWARE_VERSION     0x0201  // v2.1

// ===================== Device Configuration =====================
#define DEVICE_UNIQUE_ID_DEFAULT    0x11223344AABBCCDDULL
#define DEVICE_NAME_DEFAULT         "Generic Device Simulator"
#define DEVICE_MANUFACTURER         "Test Systems Inc"

// Device capabilities
#define MAX_CHANNELS_SUPPORTED      8
#define DEFAULT_CHANNELS            2
#define MAX_SAMPLE_RATE_HZ          100000
#define MIN_SAMPLE_RATE_HZ          1
#define DEFAULT_SAMPLE_RATE_HZ      10000

// Data format support flags
#define FORMAT_INT16                0x01
#define FORMAT_INT32                0x02
#define FORMAT_FLOAT32              0x04
#define DEFAULT_SUPPORTED_FORMATS   (FORMAT_INT16 | FORMAT_INT32)

// ===================== Communication Configuration =====================
#ifdef SIMULATION_MODE
    // Simulation mode settings
    #define DEFAULT_TCP_PORT            "9001"
    #define DEFAULT_TCP_HOST            "127.0.0.1"
    #define SOCKET_BACKLOG              1
    #define MAX_CLIENTS                 1
    
    // CSV data settings
    #define CSV_FILE_DEFAULT            "sample_data.csv"
    #define CSV_MAX_ROWS                10000
    #define CSV_MAX_COLUMNS             8
    #define CSV_BUFFER_SIZE             32768
#else
    // MCU mode settings
    #define USB_CDC_BUFFER_SIZE         1024
    #define USB_CDC_TX_TIMEOUT_MS       1000
    #define USB_CDC_RX_TIMEOUT_MS       100
    
    // Hardware pin definitions (customize for your MCU)
    #define LED_STATUS_PIN              13
    #define ADC_VREF_VOLTAGE            3300    // mV
    #define ADC_RESOLUTION              4096    // 12-bit
#endif

// ===================== Timing Configuration =====================
#define DATA_SEND_INTERVAL_MS       10      // Base data sending interval
#define HEARTBEAT_INTERVAL_MS       30000   // 30 seconds
#define COMMAND_TIMEOUT_MS          1000    // Command response timeout
#define CONNECTION_TIMEOUT_MS       5000    // Connection establishment timeout

// ===================== Buffer Configuration =====================
#define RX_BUFFER_SIZE              65536   // 64KB
#define TX_BUFFER_SIZE              8192    // 8KB
#define MAX_FRAME_SIZE              5120    // 5KB per frame
#define FRAME_BATCH_SIZE            100     // Frames per batch processing

// Trigger buffer settings
#define TRIGGER_BUFFER_SIZE         4096    // Samples
#define DEFAULT_PRE_TRIGGER         1000    // Pre-trigger samples
#define DEFAULT_POST_TRIGGER        1000    // Post-trigger samples
#define DEFAULT_TRIGGER_THRESHOLD   1000.0f // Trigger threshold

// ===================== Trigger Simulation Configuration =====================
#ifdef SIMULATION_MODE
    // Trigger timing (in seconds)
    #define TRIGGER_MIN_INTERVAL        10      // Minimum interval between triggers
    #define TRIGGER_MAX_INTERVAL        15      // Maximum interval between triggers
    
    // Trigger data packets (represents data duration)
    #define TRIGGER_MIN_PACKETS         5       // ~50ms of data
    #define TRIGGER_MAX_PACKETS         10      // ~100ms of data
    
    // Signal generation parameters
    #define SIG_GEN_FREQ_CH0            50.0f   // Channel 0 frequency (Hz)
    #define SIG_GEN_FREQ_CH1            60.0f   // Channel 1 frequency (Hz)
    #define SIG_GEN_AMPLITUDE_CH0       1000.0f // Channel 0 amplitude
    #define SIG_GEN_AMPLITUDE_CH1       800.0f  // Channel 1 amplitude
    #define SIG_GEN_NOISE_LEVEL         5.0f    // Noise amplitude
#endif

// ===================== File I/O Configuration =====================
#ifdef SIMULATION_MODE
    #define LOG_FILE_PREFIX             "device_log_"
    #define RAW_DATA_FILE_PREFIX        "raw_frames_"
    #define MAX_LOG_FILE_SIZE           10485760    // 10MB
    #define MAX_FILES_PER_SESSION       1000
    #define FILE_ROTATION_ENABLED       1
#endif

// ===================== Debug Configuration =====================
#ifdef DEBUG
    #define DEBUG_PRINT_FRAMES          1       // Print frame details
    #define DEBUG_PRINT_COMMANDS        1       // Print command processing
    #define DEBUG_PRINT_DATA            0       // Print data packets (verbose)
    #define DEBUG_MEMORY_TRACKING       1       // Track memory allocation
    #define DEBUG_PERFORMANCE_TIMING    1       // Measure performance
#else
    #define DEBUG_PRINT_FRAMES          0
    #define DEBUG_PRINT_COMMANDS        0
    #define DEBUG_PRINT_DATA            0
    #define DEBUG_MEMORY_TRACKING       0
    #define DEBUG_PERFORMANCE_TIMING    0
#endif

// ===================== Error Handling Configuration =====================
#define MAX_RETRY_ATTEMPTS          3       // Maximum retry attempts
#define ERROR_RECOVERY_ENABLED      1       // Enable automatic error recovery
#define WATCHDOG_TIMEOUT_MS         60000   // Watchdog timeout (MCU mode)

// Connection error handling
#define RECONNECT_ENABLED           1       // Auto-reconnect on connection loss
#define RECONNECT_DELAY_MS          5000    // Delay between reconnection attempts
#define MAX_RECONNECT_ATTEMPTS      10      // Maximum reconnection attempts

// ===================== Memory Management Configuration =====================
#define MEMORY_POOL_SIZE            65536   // Memory pool size (bytes)
#define MAX_ALLOCATIONS             1000    // Maximum concurrent allocations
#define MEMORY_ALIGNMENT            4       // Memory alignment (bytes)
#define ENABLE_MEMORY_PROTECTION    1       // Enable memory overflow protection

#ifdef SIMULATION_MODE
    // Simulation uses system malloc
    #define USE_SYSTEM_MALLOC           1
#else
    // MCU uses custom memory management
    #define USE_CUSTOM_MALLOC           1
    #define HEAP_SIZE                   32768   // MCU heap size
#endif

// ===================== Performance Configuration =====================
#define ENABLE_PERFORMANCE_COUNTERS 1       // Enable performance monitoring
#define MAX_PERFORMANCE_SAMPLES     1000    // Performance history samples
#define PERFORMANCE_SAMPLE_INTERVAL 1000    // Sample interval (ms)

// Expected performance targets
#define TARGET_LATENCY_MS           50      // Target end-to-end latency
#define TARGET_THROUGHPUT_KB_S      500     // Target throughput (KB/s)
#define TARGET_CPU_USAGE_PERCENT    10      // Target CPU usage

// ===================== Feature Flags =====================
#define FEATURE_CSV_DATA_LOADING    1       // Enable CSV data loading
#define FEATURE_TRIGGER_SIMULATION  1       // Enable trigger simulation
#define FEATURE_SIGNAL_GENERATION   1       // Enable built-in signal generation
#define FEATURE_LOG_MESSAGES        1       // Enable device log messages
#define FEATURE_HEARTBEAT           1       // Enable heartbeat mechanism
#define FEATURE_COMPRESSION         0       // Enable data compression (future)
#define FEATURE_ENCRYPTION          0       // Enable data encryption (future)

// Platform-specific features
#ifdef SIMULATION_MODE
    #define FEATURE_FILE_LOGGING        1   // Enable file logging
    #define FEATURE_CONSOLE_CONTROL     1   // Enable console commands
    #define FEATURE_NETWORK_CONFIG      1   // Enable network configuration
#else
    #define FEATURE_LOW_POWER_MODE      1   // Enable low power modes
    #define FEATURE_HARDWARE_WATCHDOG   1   // Enable hardware watchdog
    #define FEATURE_FLASH_STORAGE       1   // Enable flash data storage
#endif

// ===================== Validation Macros =====================
// Compile-time assertions
#define STATIC_ASSERT(cond, msg) typedef char static_assertion_##msg[(cond)?1:-1]

// Configuration validation
STATIC_ASSERT(MAX_FRAME_SIZE <= RX_BUFFER_SIZE/4, max_frame_size_too_large);
STATIC_ASSERT(DATA_SEND_INTERVAL_MS > 0, invalid_send_interval);
STATIC_ASSERT(DEFAULT_CHANNELS <= MAX_CHANNELS_SUPPORTED, too_many_default_channels);
STATIC_ASSERT(TRIGGER_BUFFER_SIZE >= DEFAULT_PRE_TRIGGER + DEFAULT_POST_TRIGGER, 
              trigger_buffer_too_small);

// ===================== Conditional Compilation Helpers =====================
#define IF_SIMULATION(code) do { \
    _Pragma("GCC diagnostic push") \
    _Pragma("GCC diagnostic ignored \"-Wunused-variable\"") \
    if (1) { code } \
    _Pragma("GCC diagnostic pop") \
} while(0)

#define IF_MCU(code) do { \
    _Pragma("GCC diagnostic push") \
    _Pragma("GCC diagnostic ignored \"-Wunused-variable\"") \
    if (1) { code } \
    _Pragma("GCC diagnostic pop") \
} while(0)

// Feature check macros
#define HAS_FEATURE(feature) (FEATURE_##feature)
#define REQUIRE_FEATURE(feature) STATIC_ASSERT(FEATURE_##feature, feature_required)

// ===================== Build Information =====================
#define BUILD_DATE          __DATE__
#define BUILD_TIME          __TIME__
#define BUILD_COMPILER      __VERSION__

// Build configuration string
#ifdef DEBUG
    #define BUILD_TYPE      "Debug"
#elif defined(PROFILE)
    #define BUILD_TYPE      "Profile"
#else
    #define BUILD_TYPE      "Release"
#endif

#ifdef SIMULATION_MODE
    #define BUILD_TARGET    "Simulation"
#else
    #define BUILD_TARGET    "MCU"
#endif

#define BUILD_CONFIG_STRING BUILD_TYPE " " BUILD_TARGET " v" PROJECT_VERSION_STRING

#endif // CONFIG_H