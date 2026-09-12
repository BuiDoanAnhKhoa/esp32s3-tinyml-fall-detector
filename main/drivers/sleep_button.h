#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "mpu6050.h"

#ifdef __cplusplus
extern "C" {
#endif

/// Initialize the sleep/wake toggle button on CONFIG_FALL_SLEEP_BUTTON_GPIO.
/// On press the device alternates between active sensing and low-power sleep:
///   - MPU6050 is put into / woken from hardware sleep
///   - Sample queue is drained
///   - LED switches to dim cyan (sleep) or warmup blue (active)
///   - MQTT status is published
esp_err_t sleep_button_init(QueueHandle_t state_queue,
                            QueueHandle_t sample_queue,
                            mpu6050_handle_t *mpu);

/// Returns true while the device is in sleep mode.
bool sleep_button_is_sleeping(void);

#ifdef __cplusplus
}
#endif
