#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FALL_STATE_WARMUP,
    FALL_STATE_NORMAL,
    FALL_STATE_DETECTED,
    FALL_STATE_ERROR,
} fall_state_t;

// Initializes the RGB on Core 0 and starts its owner task on Core 0.
esp_err_t fall_indicator_start(QueueHandle_t states);

#ifdef __cplusplus
}
#endif
