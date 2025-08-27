// File: mcu_hal.h
// Description: MCU Hardware Abstraction Layer template
// Note: Customize this file based on your specific MCU platform

#ifndef MCU_HAL_H
#define MCU_HAL_H

#include <stdint.h>
#include <stdbool.h>

// ===================== Platform-specific includes =====================
// Uncomment and customize based on your MCU platform:

// For STM32:
// #include "stm32f4xx_hal.h"
// #include "cmsis_os.h"

// For ESP32:
// #include "freertos/FreeRTOS.h"
// #include "freertos/task.h"
// #include "esp_system.h"

// For NXP/Kinetis:
// #include "fsl_device_registers.h"
// #include "fsl_debug_console.h"

// For Nordic nRF:
// #include "nrf.h"
// #include "nrf_delay.h"

// ===================== HAL Status Definitions =====================
typedef enum {
    HAL_OK = 0,
    HAL_ERROR,
    HAL_BUSY,
    HAL_TIMEOUT
} hal_status_t;

// ===================== USB CDC Definitions =====================
typedef void* usb_handle_t;

typedef enum {
    USB_OK = 0,
    USB_ERROR,
    USB_BUSY,
    USB_NOT_READY
} usb_status_t;

// ===================== Core HAL Functions =====================

/**
 * @brief Initialize MCU hardware abstraction layer
 * @return HAL_OK on success
 */
hal_status_t mcu_hal_init(void);

/**
 * @brief Deinitialize MCU HAL
 */
void mcu_hal_deinit(void);

/**
 * @brief Get system tick count in milliseconds
 * @return Current tick count
 */
uint32_t hal_get_tick(void);

/**
 * @brief Delay for specified milliseconds
 * @param ms Delay in milliseconds
 */
void hal_delay_ms(uint32_t ms);

/**
 * @brief Print debug message
 * @param format Printf-style format string
 */
void debug_printf(const char* format, ...);

// ===================== USB CDC Functions =====================

/**
 * @brief Initialize USB CDC interface
 * @return USB_OK on success
 */
usb_status_t usb_cdc_init(void);

/**
 * @brief Deinitialize USB CDC interface
 */
void usb_cdc_deinit(void);

/**
 * @brief Create USB CDC handle
 * @return USB handle or NULL on error
 */
usb_handle_t usb_cdc_create_handle(void);

/**
 * @brief Close USB CDC handle
 * @param handle USB handle to close
 */
void usb_cdc_close_handle(usb_handle_t handle);

/**
 * @brief Check if USB CDC is connected
 * @param handle USB handle
 * @return true if connected
 */
bool usb_cdc_is_connected(usb_handle_t handle);

/**
 * @brief Send data via USB CDC
 * @param handle USB handle
 * @param data Data buffer to send
 * @param length Data length
 * @return USB_OK on success
 */
usb_status_t usb_cdc_send(usb_handle_t handle, const uint8_t* data, uint32_t length);

/**
 * @brief Receive data via USB CDC
 * @param handle USB handle
 * @param buffer Buffer to store received data
 * @param buffer_size Buffer size
 * @param received Pointer to store actual received bytes
 * @param timeout_ms Timeout in milliseconds (0 = non-blocking)
 * @return USB_OK on success
 */
usb_status_t usb_cdc_receive(usb_handle_t handle, uint8_t* buffer, uint32_t buffer_size, 
                             uint32_t* received, uint32_t timeout_ms);

// ===================== ADC Functions =====================

/**
 * @brief Initialize ADC peripheral
 * @return HAL_OK on success
 */
hal_status_t adc_init(void);

/**
 * @brief Deinitialize ADC peripheral
 */
void adc_deinit(void);

/**
 * @brief Get ADC handle
 * @return ADC handle
 */
void* adc_get_handle(void);

/**
 * @brief Read single ADC channel
 * @param handle ADC handle
 * @param channel Channel number
 * @param value Pointer to store read value
 * @return HAL_OK on success
 */
hal_status_t adc_read_channel(void* handle, uint8_t channel, uint16_t* value);

/**
 * @brief Start continuous ADC conversion
 * @param handle ADC handle
 * @param channels Array of channel numbers
 * @param channel_count Number of channels
 * @param sample_rate Sample rate in Hz
 * @return HAL_OK on success
 */
hal_status_t adc_start_continuous(void* handle, uint8_t* channels, uint8_t channel_count, uint32_t sample_rate);

/**
 * @brief Stop continuous ADC conversion
 * @param handle ADC handle
 * @return HAL_OK on success
 */
hal_status_t adc_stop_continuous(void* handle);

// ===================== Sensor Functions =====================

/**
 * @brief Initialize sensor
 * @param sensor_id Sensor ID/channel
 * @return Sensor handle or NULL on error
 */
void* sensor_init(uint8_t sensor_id);

/**
 * @brief Deinitialize sensor
 * @param handle Sensor handle
 */
void sensor_deinit(void* handle);

/**
 * @brief Read sensor value
 * @param handle Sensor handle
 * @param value Pointer to store sensor value
 * @return HAL_OK on success
 */
hal_status_t sensor_read(void* handle, int16_t* value);

// ===================== Memory Management (FreeRTOS example) =====================

/**
 * @brief Allocate memory (FreeRTOS style)
 * @param size Size to allocate
 * @return Pointer to allocated memory or NULL
 */
void* pvPortMalloc(size_t size);

/**
 * @brief Free memory (FreeRTOS style)
 * @param ptr Pointer to free
 */
void vPortFree(void* ptr);

// ===================== Task Management (FreeRTOS example) =====================

/**
 * @brief Delay current task
 * @param ticks Ticks to delay
 */
void vTaskDelay(uint32_t ticks);

/**
 * @brief Convert milliseconds to ticks
 * @param ms Milliseconds
 * @return Ticks
 */
#define pdMS_TO_TICKS(ms) ((ms) * configTICK_RATE_HZ / 1000)

/**
 * @brief Get current tick count
 * @return Current tick count
 */
uint32_t xTaskGetTickCount(void);

// ===================== Platform-specific Configuration =====================

// Customize these based on your MCU:
#define configTICK_RATE_HZ          1000    // 1ms tick
#define MAX_ADC_CHANNELS            8       // Maximum ADC channels
#define ADC_RESOLUTION_BITS         12      // ADC resolution
#define USB_CDC_BUFFER_SIZE         1024    // USB CDC buffer size

// ===================== Hardware-specific Definitions =====================

// Example for STM32:
/*
#define ADC_CHANNEL_VOLTAGE         ADC_CHANNEL_0
#define ADC_CHANNEL_CURRENT         ADC_CHANNEL_1
#define GPIO_LED_PIN                GPIO_PIN_13
#define GPIO_LED_PORT               GPIOC
*/

// Example for ESP32:
/*
#define ADC_CHANNEL_VOLTAGE         ADC1_CHANNEL_0
#define ADC_CHANNEL_CURRENT         ADC1_CHANNEL_1
#define GPIO_LED_PIN                2
*/

#endif // MCU_HAL_H