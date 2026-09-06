#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

// Samples are imu_sample_t; states is a one-element fall_state_t queue.
esp_err_t fall_detection_start(QueueHandle_t samples, QueueHandle_t states);

#ifdef __cplusplus
}
#endif
